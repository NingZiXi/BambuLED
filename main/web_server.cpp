#include "app.hpp"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>

extern "C" {
#include "cJSON.h"
#include "esp_log.h"
#include "esp_system.h"
#include "lwip/inet.h"
}

namespace bambuled {
namespace {
constexpr uint32_t kWifiConnectTimeoutMs = 15000;
constexpr const char *kTag = "WebServer";

bool wifi_credentials_match(const DeviceConfig &lhs, const DeviceConfig &rhs) {
    return std::strncmp(lhs.wifi_ssid, rhs.wifi_ssid, sizeof(lhs.wifi_ssid)) == 0 &&
           std::strncmp(lhs.wifi_password, rhs.wifi_password, sizeof(lhs.wifi_password)) == 0;
}

bool is_captive_probe_uri(const char *uri) {
    if (uri == nullptr) {
        return false;
    }
    return std::strcmp(uri, "/generate_204") == 0 || std::strcmp(uri, "/generate204") == 0 || std::strcmp(uri, "/hotspot-detect.html") == 0 ||
           std::strcmp(uri, "/canonical.html") == 0 || std::strcmp(uri, "/success.txt") == 0 || std::strcmp(uri, "/ncsi.txt") == 0 ||
           std::strcmp(uri, "/connecttest.txt") == 0 || std::strcmp(uri, "/redirect") == 0 || std::strcmp(uri, "/fwlink") == 0 ||
           std::strcmp(uri, "/wpad.dat") == 0;
}

esp_err_t send_absolute_portal_redirect(httpd_req_t *req, uint32_t ip_addr) {
    char ip_str[16] = {0};
    inet_ntoa_r(ip_addr, ip_str, sizeof(ip_str));

    char location[32] = {0};
    std::snprintf(location, sizeof(location), "http://%s/", ip_str);

    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Location", location);
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    return httpd_resp_sendstr(req, "Redirect to captive portal");
}

esp_err_t send_portal_redirect(httpd_req_t *req) {
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    return httpd_resp_sendstr(req, "Redirect to captive portal");
}

uint8_t clamp_u8(int value, int fallback) {
    if (value < 0 || value > 255) {
        return static_cast<uint8_t>(fallback);
    }
    return static_cast<uint8_t>(value);
}

RgbColor parse_hex_color(const std::string &text, const RgbColor &fallback) {
    if (text.size() != 7 || text[0] != '#') {
        return fallback;
    }
    unsigned int r = 0;
    unsigned int g = 0;
    unsigned int b = 0;
    if (std::sscanf(text.c_str(), "#%02x%02x%02x", &r, &g, &b) != 3) {
        return fallback;
    }
    return make_color(static_cast<uint8_t>(r), static_cast<uint8_t>(g), static_cast<uint8_t>(b));
}

void color_to_hex(const RgbColor &color, char *buffer, size_t size) {
    std::snprintf(buffer, size, "#%02X%02X%02X", color.r, color.g, color.b);
}

bool has_form_key(const std::string &body, const char *key) {
    return body.find(std::string(key) + "=") != std::string::npos;
}

void add_profile_json(cJSON *root, const char *prefix, const StateLightProfile &profile) {
    cJSON_AddNumberToObject(root, (std::string(prefix) + "_effect").c_str(), profile.effect);
    cJSON_AddNumberToObject(root, (std::string(prefix) + "_speed").c_str(), profile.speed);
    char hex[16];
    color_to_hex(profile.color, hex, sizeof(hex));
    cJSON_AddStringToObject(root, (std::string(prefix) + "_hex").c_str(), hex);
}

extern const uint8_t data_index_html_start[] asm("_binary_index_html_start");
extern const uint8_t data_index_html_end[] asm("_binary_index_html_end");

void restart_task(void *arg) {
    const uint32_t delay_ms = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(arg));
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
    esp_restart();
}

const char *wifi_connect_state_name(WifiConnectState state) {
    switch (state) {
        case WifiConnectState::Idle:
            return "idle";
        case WifiConnectState::Connecting:
            return "connecting";
        case WifiConnectState::Failed:
            return "failed";
        case WifiConnectState::RestartPending:
            return "restart_pending";
    }
    return "idle";
}
}  // namespace

WebServer::WebServer()
    : server_(nullptr),
      app_queue_(nullptr),
      store_(nullptr),
      config_(default_config()),
      saved_config_(default_config()),
      portal_mode_(false),
      portal_ip_addr_(inet_addr("192.168.4.1")),
      wifi_connected_(false),
      restart_scheduled_(false),
      wifi_connect_state_(WifiConnectState::Idle),
      wifi_connect_started_at_(0) {
    connected_wifi_ssid_[0] = '\0';
    wifi_connect_target_ssid_[0] = '\0';
    wifi_connect_message_[0] = '\0';
    wifi_connect_detail_[0] = '\0';
}
WebServer::~WebServer() { stop(); }

esp_err_t WebServer::start(QueueHandle_t app_queue, ConfigStore *store, const DeviceConfig &config) {
    if (server_ != nullptr) {
        return ESP_OK;
    }

    app_queue_ = app_queue;
    store_ = store;
    config_ = sanitize_config(config);
    saved_config_ = config_;

    httpd_config_t http_config = HTTPD_DEFAULT_CONFIG();
    http_config.stack_size = 6144;
    http_config.lru_purge_enable = true;
    http_config.keep_alive_enable = false;
    http_config.enable_so_linger = true;
    http_config.linger_timeout = 0;
    http_config.recv_wait_timeout = 2;
    http_config.send_wait_timeout = 2;
    http_config.uri_match_fn = httpd_uri_match_wildcard;

    esp_err_t err = httpd_start(&server_, &http_config);
    if (err != ESP_OK) {
        return err;
    }

    httpd_uri_t root_uri = {.uri = "/", .method = HTTP_GET, .handler = &WebServer::root_get_handler, .user_ctx = this};
    httpd_uri_t api_get_uri = {.uri = "/api/config", .method = HTTP_GET, .handler = &WebServer::api_get_handler, .user_ctx = this};
    httpd_uri_t api_post_uri = {.uri = "/api/config", .method = HTTP_POST, .handler = &WebServer::config_post_handler, .user_ctx = this};
    httpd_uri_t wifi_disconnect_uri = {
        .uri = "/api/wifi/disconnect", .method = HTTP_POST, .handler = &WebServer::wifi_disconnect_post_handler, .user_ctx = this};
    httpd_uri_t portal_probe_uri = {.uri = "/*",
                                    .method = static_cast<httpd_method_t>(HTTP_ANY),
                                    .handler = &WebServer::portal_probe_get_handler,
                                    .user_ctx = this};

    httpd_register_uri_handler(server_, &root_uri);
    httpd_register_uri_handler(server_, &api_get_uri);
    httpd_register_uri_handler(server_, &api_post_uri);
    httpd_register_uri_handler(server_, &wifi_disconnect_uri);
    httpd_register_uri_handler(server_, &portal_probe_uri);
    httpd_register_err_handler(server_, HTTPD_404_NOT_FOUND, &WebServer::error_handler_404);
    httpd_register_err_handler(server_, HTTPD_414_URI_TOO_LONG, &WebServer::error_handler_414);
    return ESP_OK;
}

void WebServer::stop() {
    if (server_ != nullptr) {
        httpd_stop(server_);
        server_ = nullptr;
    }
}

void WebServer::set_config(const DeviceConfig &config) {
    config_ = sanitize_config(config);
}

void WebServer::set_portal_mode(bool enabled) {
    portal_mode_ = enabled;
}

void WebServer::set_portal_ip(uint32_t ip_addr) {
    if (ip_addr != 0) {
        portal_ip_addr_ = ip_addr;
    }
}

void WebServer::begin_wifi_connect_attempt(const DeviceConfig &config) {
    config_ = sanitize_config(config);
    wifi_connected_ = false;
    connected_wifi_ssid_[0] = '\0';
    wifi_connect_state_ = WifiConnectState::Connecting;
    wifi_connect_started_at_ = xTaskGetTickCount();
    copy_cstr(wifi_connect_target_ssid_, config_.wifi_ssid);
    copy_cstr(wifi_connect_message_, "正在连接 WiFi");

    char detail[sizeof(wifi_connect_detail_)] = {0};
    std::snprintf(detail, sizeof(detail), "正在尝试连接到 %s，连接成功后会自动重启。", wifi_connect_target_ssid_[0] != '\0'
                                                                                           ? wifi_connect_target_ssid_
                                                                                           : "目标 WiFi");
    copy_cstr(wifi_connect_detail_, detail);
}

void WebServer::set_wifi_state(bool connected, const char *ssid) {
    wifi_connected_ = connected;
    copy_cstr(connected_wifi_ssid_, connected ? ssid : "");

    if (connected) {
        const bool should_restart_after_connect = (wifi_connect_state_ == WifiConnectState::Connecting);

        if (!wifi_credentials_match(config_, saved_config_)) {
            esp_err_t err = ESP_FAIL;
            if (store_ != nullptr) {
                err = store_->save(config_);
            }
            if (store_ != nullptr && err != ESP_OK) {
                wifi_connect_state_ = WifiConnectState::Failed;
                copy_cstr(wifi_connect_message_, "WiFi 已连接但保存失败");
                std::snprintf(wifi_connect_detail_, sizeof(wifi_connect_detail_),
                              "设备已连接到 %s，但写入配置失败，请稍后重试。", connected_wifi_ssid_[0] != '\0' ? connected_wifi_ssid_ : "目标 WiFi");
                ESP_LOGE(kTag, "Failed to persist WiFi config: %s", esp_err_to_name(err));
                return;
            }
            saved_config_ = config_;
        }

        if (!should_restart_after_connect) {
            wifi_connect_state_ = WifiConnectState::Idle;
            wifi_connect_message_[0] = '\0';
            wifi_connect_detail_[0] = '\0';
            copy_cstr(wifi_connect_target_ssid_, connected_wifi_ssid_);
            return;
        }

        copy_cstr(wifi_connect_target_ssid_, connected_wifi_ssid_);
        copy_cstr(wifi_connect_message_, "WiFi 连接成功");
        std::snprintf(wifi_connect_detail_, sizeof(wifi_connect_detail_), "设备已连接到 %s，3 秒后自动重启。", connected_wifi_ssid_);
        wifi_connect_state_ = WifiConnectState::RestartPending;
        if (!restart_scheduled_) {
            restart_scheduled_ = true;
            schedule_restart(3000);
        }
        return;
    }

    if (wifi_connect_state_ != WifiConnectState::Connecting) {
        wifi_connect_state_ = WifiConnectState::Idle;
        wifi_connect_message_[0] = '\0';
        wifi_connect_detail_[0] = '\0';
    }
}

bool WebServer::is_running() const {
    return server_ != nullptr;
}

esp_err_t WebServer::root_get_handler(httpd_req_t *req) {
    return static_cast<WebServer *>(req->user_ctx)->handle_root(req);
}

esp_err_t WebServer::api_get_handler(httpd_req_t *req) {
    return static_cast<WebServer *>(req->user_ctx)->handle_api_get(req);
}

esp_err_t WebServer::config_post_handler(httpd_req_t *req) {
    return static_cast<WebServer *>(req->user_ctx)->handle_config_post(req);
}

esp_err_t WebServer::wifi_disconnect_post_handler(httpd_req_t *req) {
    return static_cast<WebServer *>(req->user_ctx)->handle_wifi_disconnect(req);
}

esp_err_t WebServer::portal_probe_get_handler(httpd_req_t *req) {
    return static_cast<WebServer *>(req->user_ctx)->handle_portal_probe(req);
}

esp_err_t WebServer::error_handler_404(httpd_req_t *req, httpd_err_code_t) {
    return send_portal_redirect(req);
}

esp_err_t WebServer::error_handler_414(httpd_req_t *req, httpd_err_code_t) {
    return send_portal_redirect(req);
}

esp_err_t WebServer::handle_root(httpd_req_t *req) {
    std::string html = render_index_html();
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, html.c_str(), html.size());
}

esp_err_t WebServer::handle_portal_probe(httpd_req_t *req) {
    const char *uri = req->uri;
    if (uri != nullptr && std::strcmp(uri, "/") == 0) {
        return handle_root(req);
    }

    if (is_captive_probe_uri(uri)) {
        return send_absolute_portal_redirect(req, portal_ip_addr_);
    }

    ESP_LOGI(kTag, "Captive portal probe: %s", uri != nullptr ? uri : "<null>");
    return send_absolute_portal_redirect(req, portal_ip_addr_);
}

esp_err_t WebServer::handle_api_get(httpd_req_t *req) {
    refresh_wifi_connect_state();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "wifi_ssid", config_.wifi_ssid);
    cJSON_AddBoolToObject(root, "wifi_connected", wifi_connected_);
    cJSON_AddStringToObject(root, "connected_wifi_ssid", connected_wifi_ssid_);
    cJSON_AddStringToObject(root, "wifi_connect_state", wifi_connect_state_name(wifi_connect_state_));
    cJSON_AddStringToObject(root, "wifi_connect_target_ssid", wifi_connect_target_ssid_);
    cJSON_AddStringToObject(root, "wifi_connect_message", wifi_connect_message_);
    cJSON_AddStringToObject(root, "wifi_connect_detail", wifi_connect_detail_);
    cJSON_AddNumberToObject(root, "restart_delay", wifi_connect_state_ == WifiConnectState::RestartPending ? 3 : 0);
    cJSON_AddStringToObject(root, "printer_ip", config_.printer_ip);
    cJSON_AddNumberToObject(root, "mqtt_port", config_.mqtt_port);
    cJSON_AddBoolToObject(root, "mqtt_use_tls", config_.mqtt_use_tls);
    cJSON_AddStringToObject(root, "printer_serial", config_.printer_serial);
    cJSON_AddNumberToObject(root, "led_count", config_.led_count);
    cJSON_AddNumberToObject(root, "led_gpio", config_.led_gpio);
    cJSON_AddNumberToObject(root, "brightness", config_.brightness);
    cJSON_AddNumberToObject(root, "effect_speed", config_.effect_speed);
    add_profile_json(root, "idle", config_.idle_profile);
    add_profile_json(root, "heating", config_.heating_profile);
    add_profile_json(root, "printing", config_.printing_profile);
    add_profile_json(root, "finished", config_.finished_profile);
    add_profile_json(root, "paused", config_.paused_profile);
    add_profile_json(root, "alert", config_.alert_profile);

    char hex[16];
    color_to_hex(config_.default_color, hex, sizeof(hex));
    cJSON_AddStringToObject(root, "default_hex", hex);
    color_to_hex(config_.progress_background_color, hex, sizeof(hex));
    cJSON_AddStringToObject(root, "progress_background_hex", hex);

    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    cJSON_free(json);
    cJSON_Delete(root);
    return err;
}

esp_err_t WebServer::handle_config_post(httpd_req_t *req) {
    std::string body(static_cast<size_t>(req->content_len), '\0');
    int received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, body.data() + received, req->content_len - received);
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (ret <= 0) {
            ESP_LOGW(kTag, "httpd_req_recv failed: %d (received=%d len=%d)", ret, received, static_cast<int>(req->content_len));
            return send_json_result(req, false, false, "保存失败", "接收请求失败，请稍后重试。");
        }
        received += ret;
    }

    DeviceConfig next = config_;
    const std::string wifi_ssid = url_decode(get_form_value(body, "wifi_ssid"));
    const std::string wifi_password = url_decode(get_form_value(body, "wifi_password"));
    const std::string printer_ip = url_decode(get_form_value(body, "printer_ip"));
    const std::string printer_access_code = url_decode(get_form_value(body, "printer_access_code"));
    const std::string printer_serial = url_decode(get_form_value(body, "printer_serial"));

    bool wifi_changed = false;
    if (has_form_key(body, "wifi_ssid")) {
        wifi_changed = wifi_changed || std::strncmp(wifi_ssid.c_str(), config_.wifi_ssid, sizeof(next.wifi_ssid)) != 0;
        const bool wifi_ssid_changed = std::strncmp(wifi_ssid.c_str(), config_.wifi_ssid, sizeof(next.wifi_ssid)) != 0;
        copy_cstr(next.wifi_ssid, wifi_ssid.c_str());
        if (wifi_ssid_changed && !has_form_key(body, "wifi_password")) {
            next.wifi_password[0] = '\0';
            wifi_changed = true;
        }
    }
    if (has_form_key(body, "wifi_password")) {
        if (wifi_password.empty() && has_form_key(body, "wifi_ssid")) {
            next.wifi_password[0] = '\0';
        } else if (!wifi_password.empty()) {
            copy_cstr(next.wifi_password, wifi_password.c_str());
        }
        wifi_changed = wifi_changed || std::strncmp(next.wifi_password, config_.wifi_password, sizeof(next.wifi_password)) != 0;
    }

    if (has_form_key(body, "printer_ip")) {
        copy_cstr(next.printer_ip, printer_ip.c_str());
    }
    if (has_form_key(body, "printer_access_code")) {
        if (!printer_access_code.empty()) {
            copy_cstr(next.printer_access_code, printer_access_code.c_str());
        } else {
            next.printer_access_code[0] = '\0';
        }
    }
    if (has_form_key(body, "printer_serial")) {
        copy_cstr(next.printer_serial, printer_serial.c_str());
    }

    if (has_form_key(body, "mqtt_port")) {
        std::string mqtt_port = get_form_value(body, "mqtt_port");
        next.mqtt_port = static_cast<uint16_t>(std::atoi(mqtt_port.empty() ? "8883" : mqtt_port.c_str()));
    }
    if (has_form_key(body, "mqtt_use_tls")) {
        next.mqtt_use_tls = is_checked(body, "mqtt_use_tls");
    }
    if (has_form_key(body, "led_count")) {
        std::string led_count = get_form_value(body, "led_count");
        next.led_count = static_cast<uint16_t>(std::atoi(led_count.empty() ? "10" : led_count.c_str()));
    }
    if (has_form_key(body, "led_gpio")) {
        std::string led_gpio = get_form_value(body, "led_gpio");
        next.led_gpio = static_cast<int16_t>(std::atoi(led_gpio.empty() ? "1" : led_gpio.c_str()));
    }
    if (has_form_key(body, "brightness")) {
        std::string brightness = get_form_value(body, "brightness");
        next.brightness = clamp_u8(std::atoi(brightness.empty() ? "64" : brightness.c_str()), 64);
    }
    if (has_form_key(body, "effect_speed")) {
        std::string effect_speed = get_form_value(body, "effect_speed");
        next.effect_speed = clamp_u8(std::atoi(effect_speed.empty() ? "40" : effect_speed.c_str()), 40);
    }
    if (has_form_key(body, "default_hex")) {
        next.default_color = parse_hex_color(url_decode(get_form_value(body, "default_hex")), default_config().default_color);
    }
    if (has_form_key(body, "progress_background_hex")) {
        next.progress_background_color = parse_hex_color(url_decode(get_form_value(body, "progress_background_hex")),
                                                         default_config().progress_background_color);
    }
    if (has_form_key(body, "idle_effect")) {
        next.idle_profile.effect = clamp_u8(std::atoi(get_form_value(body, "idle_effect").c_str()), effect_value(LedEffect::Off));
    }
    if (has_form_key(body, "idle_speed")) {
        next.idle_profile.speed = clamp_u8(std::atoi(get_form_value(body, "idle_speed").c_str()), default_config().idle_profile.speed);
    }
    if (has_form_key(body, "idle_hex")) {
        next.idle_profile.color = parse_hex_color(url_decode(get_form_value(body, "idle_hex")), default_config().idle_profile.color);
    }
    if (has_form_key(body, "heating_effect")) {
        next.heating_profile.effect = clamp_u8(std::atoi(get_form_value(body, "heating_effect").c_str()), effect_value(LedEffect::Off));
    }
    if (has_form_key(body, "heating_speed")) {
        next.heating_profile.speed = clamp_u8(std::atoi(get_form_value(body, "heating_speed").c_str()), default_config().heating_profile.speed);
    }
    if (has_form_key(body, "heating_hex")) {
        next.heating_profile.color = parse_hex_color(url_decode(get_form_value(body, "heating_hex")), default_config().heating_profile.color);
    }
    if (has_form_key(body, "printing_effect")) {
        next.printing_profile.effect = clamp_u8(std::atoi(get_form_value(body, "printing_effect").c_str()), effect_value(LedEffect::Off));
    }
    if (has_form_key(body, "printing_speed")) {
        next.printing_profile.speed = clamp_u8(std::atoi(get_form_value(body, "printing_speed").c_str()), default_config().printing_profile.speed);
    }
    if (has_form_key(body, "printing_hex")) {
        next.printing_profile.color = parse_hex_color(url_decode(get_form_value(body, "printing_hex")), default_config().printing_profile.color);
    }
    if (has_form_key(body, "finished_effect")) {
        next.finished_profile.effect = clamp_u8(std::atoi(get_form_value(body, "finished_effect").c_str()), effect_value(LedEffect::Off));
    }
    if (has_form_key(body, "finished_speed")) {
        next.finished_profile.speed = clamp_u8(std::atoi(get_form_value(body, "finished_speed").c_str()), default_config().finished_profile.speed);
    }
    if (has_form_key(body, "finished_hex")) {
        next.finished_profile.color = parse_hex_color(url_decode(get_form_value(body, "finished_hex")), default_config().finished_profile.color);
    }
    if (has_form_key(body, "paused_effect")) {
        next.paused_profile.effect = clamp_u8(std::atoi(get_form_value(body, "paused_effect").c_str()), effect_value(LedEffect::Off));
    }
    if (has_form_key(body, "paused_speed")) {
        next.paused_profile.speed = clamp_u8(std::atoi(get_form_value(body, "paused_speed").c_str()), default_config().paused_profile.speed);
    }
    if (has_form_key(body, "paused_hex")) {
        next.paused_profile.color = parse_hex_color(url_decode(get_form_value(body, "paused_hex")), default_config().paused_profile.color);
    }
    if (has_form_key(body, "alert_effect")) {
        next.alert_profile.effect = clamp_u8(std::atoi(get_form_value(body, "alert_effect").c_str()), effect_value(LedEffect::Off));
    }
    if (has_form_key(body, "alert_speed")) {
        next.alert_profile.speed = clamp_u8(std::atoi(get_form_value(body, "alert_speed").c_str()), default_config().alert_profile.speed);
    }
    if (has_form_key(body, "alert_hex")) {
        next.alert_profile.color = parse_hex_color(url_decode(get_form_value(body, "alert_hex")), default_config().alert_profile.color);
    }
    next.version = kConfigVersion;
    next = sanitize_config(next);
    config_ = next;

    if (wifi_changed && has_wifi_credentials(next)) {
        ESP_LOGI(kTag, "Applying WiFi config for SSID: %s", next.wifi_ssid);
        begin_wifi_connect_attempt(next);
    } else {
        if (store_ != nullptr) {
            esp_err_t err = store_->save(next);
            if (err != ESP_OK) {
                ESP_LOGE(kTag, "Failed to save config: %s", esp_err_to_name(err));
                return send_json_result(req, false, false, "保存失败", "写入配置失败，请稍后重试。");
            }
        }
        saved_config_ = next;
    }

    if (app_queue_ != nullptr) {
        AppEvent event{};
        event.type = AppEventType::ConfigUpdated;
        event.config = next;
        xQueueSend(app_queue_, &event, 0);
    }

    if (wifi_changed) {
        if (!has_wifi_credentials(next)) {
            wifi_connect_state_ = WifiConnectState::Idle;
            wifi_connect_message_[0] = '\0';
            wifi_connect_detail_[0] = '\0';
            return send_json_result(req, true, false, "WiFi 已清空", "已移除保存的 WiFi 信息，如需断开当前连接，请使用“断开当前 WiFi”。");
        }
        return send_json_result(req, true, false, "正在连接 WiFi", wifi_connect_detail_);
    }

    return send_json_result(req, true, false, "配置已保存", "打印机与灯光参数已经立即生效。");
}

esp_err_t WebServer::handle_wifi_disconnect(httpd_req_t *req) {
    DeviceConfig next = config_;
    next.wifi_ssid[0] = '\0';
    next.wifi_password[0] = '\0';

    if (store_ != nullptr) {
        esp_err_t err = store_->save(next);
        if (err != ESP_OK) {
            ESP_LOGE(kTag, "Failed to clear WiFi config: %s", esp_err_to_name(err));
            return send_json_result(req, false, false, "断开失败", "清除 WiFi 配置失败，请稍后重试。");
        }
    }
    config_ = next;
    saved_config_ = next;
    wifi_connected_ = false;
    connected_wifi_ssid_[0] = '\0';
    wifi_connect_state_ = WifiConnectState::Idle;
    wifi_connect_message_[0] = '\0';
    wifi_connect_detail_[0] = '\0';
    wifi_connect_target_ssid_[0] = '\0';

    if (app_queue_ != nullptr) {
        AppEvent event{};
        event.type = AppEventType::ConfigUpdated;
        event.config = next;
        xQueueSend(app_queue_, &event, 0);
    }

    return send_json_result(req, true, false, "已断开 WiFi", "设备已断开当前 WiFi，配置热点会继续保持开启。");
}

std::string WebServer::url_decode(const std::string &text) {
    std::string output;
    output.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '+') {
            output.push_back(' ');
        } else if (text[i] == '%' && i + 2 < text.size()) {
            int value = 0;
            std::sscanf(text.substr(i + 1, 2).c_str(), "%x", &value);
            output.push_back(static_cast<char>(value));
            i += 2;
        } else {
            output.push_back(text[i]);
        }
    }
    return output;
}

std::string WebServer::get_form_value(const std::string &body, const char *key) {
    const std::string token = std::string(key) + "=";
    size_t start = body.find(token);
    if (start == std::string::npos) {
        return "";
    }
    start += token.size();
    size_t end = body.find('&', start);
    return body.substr(start, end == std::string::npos ? body.size() - start : end - start);
}

bool WebServer::is_checked(const std::string &body, const char *key) {
    return body.find(std::string(key) + "=on") != std::string::npos;
}

void WebServer::schedule_restart(uint32_t delay_ms) {
    xTaskCreate(&restart_task, "web_restart", 2048, reinterpret_cast<void *>(static_cast<uintptr_t>(delay_ms)), 4, nullptr);
}

esp_err_t WebServer::send_json_result(httpd_req_t *req, bool ok, bool restart_required, const char *message, const char *detail) {
    refresh_wifi_connect_state();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", ok);
    cJSON_AddBoolToObject(root, "restart_required", restart_required);
    cJSON_AddNumberToObject(root, "restart_delay", restart_required ? 3 : 0);
    cJSON_AddStringToObject(root, "message", message != nullptr ? message : "");
    cJSON_AddStringToObject(root, "detail", detail != nullptr ? detail : "");
    cJSON_AddStringToObject(root, "wifi_connect_state", wifi_connect_state_name(wifi_connect_state_));
    cJSON_AddStringToObject(root, "wifi_connect_target_ssid", wifi_connect_target_ssid_);
    cJSON_AddStringToObject(root, "wifi_connect_message", wifi_connect_message_);
    cJSON_AddStringToObject(root, "wifi_connect_detail", wifi_connect_detail_);
    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    esp_err_t err = httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    cJSON_free(json);
    cJSON_Delete(root);
    return err;
}

void WebServer::refresh_wifi_connect_state() {
    if (wifi_connect_state_ != WifiConnectState::Connecting) {
        return;
    }

    if ((xTaskGetTickCount() - wifi_connect_started_at_) < pdMS_TO_TICKS(kWifiConnectTimeoutMs)) {
        return;
    }

    wifi_connect_state_ = WifiConnectState::Failed;
    copy_cstr(wifi_connect_message_, "WiFi 连接失败");

    char detail[sizeof(wifi_connect_detail_)] = {0};
    std::snprintf(detail, sizeof(detail), "连接 %s 超时，请检查 WiFi 名称、密码和 2.4GHz 网络状态后重试。",
                  wifi_connect_target_ssid_[0] != '\0' ? wifi_connect_target_ssid_ : "目标 WiFi");
    copy_cstr(wifi_connect_detail_, detail);
}

std::string WebServer::render_index_html() const {
    const char *html_template = reinterpret_cast<const char *>(data_index_html_start);
    const size_t template_len = static_cast<size_t>(data_index_html_end - data_index_html_start);
    return std::string(html_template, template_len);
}

}  // namespace bambuled
