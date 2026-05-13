#include "app.hpp"

#include <algorithm>
#include <cstdio>
#include <string>

extern "C" {
#include "cJSON.h"
#include "esp_log.h"
}

namespace bambuled {
namespace {
constexpr const char *kTag = "MqttService";

static const char kBambuCaCertPem[] = R"pem(-----BEGIN CERTIFICATE-----
MIIDZTCCAk2gAwIBAgIUV1FckwXElyek1onFnQ9kL7Bk4N8wDQYJKoZIhvcNAQEL
BQAwQjELMAkGA1UEBhMCQ04xIjAgBgNVBAoMGUJCTCBUZWNobm9sb2dpZXMgQ28u
LCBMdGQxDzANBgNVBAMMBkJCTCBDQTAeFw0yMjA0MDQwMzQyMTFaFw0zMjA0MDEw
MzQyMTFaMEIxCzAJBgNVBAYTAkNOMSIwIAYDVQQKDBlCQkwgVGVjaG5vbG9naWVz
IENvLiwgTHRkMQ8wDQYDVQQDDAZCQkwgQ0EwggEiMA0GCSqGSIb3DQEBAQUAA4IB
DwAwggEKAoIBAQDL3pnDdxGOk5Z6vugiT4dpM0ju+3Xatxz09UY7mbj4tkIdby4H
oeEdiYSZjc5LJngJuCHwtEbBJt1BriRdSVrF6M9D2UaBDyamEo0dxwSaVxZiDVWC
eeCPdELpFZdEhSNTaT4O7zgvcnFsfHMa/0vMAkvE7i0qp3mjEzYLfz60axcDoJLk
p7n6xKXI+cJbA4IlToFjpSldPmC+ynOo7YAOsXt7AYKY6Glz0BwUVzSJxU+/+VFy
/QrmYGNwlrQtdREHeRi0SNK32x1+bOndfJP0sojuIrDjKsdCLye5CSZIvqnbowwW
1jRwZgTBR29Zp2nzCoxJYcU9TSQp/4KZuWNVAgMBAAGjUzBRMB0GA1UdDgQWBBSP
NEJo3GdOj8QinsV8SeWr3US+HjAfBgNVHSMEGDAWgBSPNEJo3GdOj8QinsV8SeWr
3US+HjAPBgNVHRMBAf8EBTADAQH/MA0GCSqGSIb3DQEBCwUAA4IBAQABlBIT5ZeG
fgcK1LOh1CN9sTzxMCLbtTPFF1NGGA13mApu6j1h5YELbSKcUqfXzMnVeAb06Htu
3CoCoe+wj7LONTFO++vBm2/if6Jt/DUw1CAEcNyqeh6ES0NX8LJRVSe0qdTxPJuA
BdOoo96iX89rRPoxeed1cpq5hZwbeka3+CJGV76itWp35Up5rmmUqrlyQOr/Wax6
itosIzG0MfhgUzU51A2P/hSnD3NDMXv+wUY/AvqgIL7u7fbDKnku1GzEKIkfH8hm
Rs6d8SCU89xyrwzQ0PR853irHas3WrHVqab3P+qNwR0YirL0Qk7Xt/q3O1griNg2
Blbjg3obpHo9
-----END CERTIFICATE-----
)pem";

uint32_t log_tick_ms() {
    return static_cast<uint32_t>(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

double json_number(cJSON *obj, const char *key, double fallback = 0) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(item) ? item->valuedouble : fallback;
}

int json_int(cJSON *obj, const char *key, int fallback = 0) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(item) ? item->valueint : fallback;
}

std::string json_string(cJSON *obj, const char *key) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(item) && item->valuestring != nullptr ? item->valuestring : "";
}

void log_mqtt_payload_summary(const char *topic, int topic_len, const char *data, int data_len) {
    static uint32_t last_log_ms = 0;
    static uint32_t suppressed = 0;
    std::string topic_str = (topic != nullptr && topic_len > 0) ? std::string(topic, topic_len) : "<no-topic>";

    char preview[201] = {0};
    const int copy_len = std::max(0, std::min(200, data_len));
    if (data != nullptr && copy_len > 0) {
        std::memcpy(preview, data, static_cast<size_t>(copy_len));
        preview[copy_len] = '\0';
    }

    std::string print_command;
    std::string gcode_state;
    int mc_percent = -1;
    std::string wifi_signal;
    double bed_temper = -10000;
    double bed_target = -10000;
    double nozzle_temper = -10000;
    double nozzle_target = -10000;

    std::string sys_command;
    std::string led_node;
    std::string led_mode;
    std::string sys_result;
    std::string sys_reason;

    if (data != nullptr && data_len > 0) {
        cJSON *root = cJSON_ParseWithLength(data, static_cast<size_t>(data_len));
        if (root != nullptr) {
            cJSON *print = cJSON_GetObjectItemCaseSensitive(root, "print");
            if (cJSON_IsObject(print)) {
                print_command = json_string(print, "command");
                gcode_state = json_string(print, "gcode_state");
                mc_percent = json_int(print, "mc_percent", -1);
                wifi_signal = json_string(print, "wifi_signal");
                bed_temper = json_number(print, "bed_temper", -10000);
                bed_target = json_number(print, "bed_target_temper", -10000);
                nozzle_temper = json_number(print, "nozzle_temper", -10000);
                nozzle_target = json_number(print, "nozzle_target_temper", -10000);
            }

            cJSON *system = cJSON_GetObjectItemCaseSensitive(root, "system");
            if (cJSON_IsObject(system)) {
                sys_command = json_string(system, "command");
                led_node = json_string(system, "led_node");
                led_mode = json_string(system, "led_mode");
                sys_result = json_string(system, "result");
                sys_reason = json_string(system, "reason");
            }
            cJSON_Delete(root);
        }
    }

    const bool is_ledctrl = (sys_command == "ledctrl");
    const uint32_t now = log_tick_ms();
    if (!is_ledctrl && now - last_log_ms < 600) {
        ++suppressed;
        return;
    }
    last_log_ms = now;

    if (suppressed > 0) {
        ESP_LOGI(kTag, "MQTT RX: %s (%d bytes) [suppressed=%u]", topic_str.c_str(), data_len, suppressed);
        suppressed = 0;
    }

    if (is_ledctrl) {
        ESP_LOGI(kTag, "MQTT RX: %s ledctrl node=%s mode=%s result=%s reason=%s",
                 topic_str.c_str(),
                 led_node.empty() ? "-" : led_node.c_str(),
                 led_mode.empty() ? "-" : led_mode.c_str(),
                 sys_result.empty() ? "-" : sys_result.c_str(),
                 sys_reason.empty() ? "-" : sys_reason.c_str());
        return;
    }

    const bool has_print =
        !print_command.empty() || !gcode_state.empty() || mc_percent >= 0 || !wifi_signal.empty() || bed_temper > -9999 || nozzle_temper > -9999;
    if (!has_print) {
        ESP_LOGI(kTag, "MQTT RX: %s (%d bytes) preview=%s", topic_str.c_str(), data_len, preview[0] ? preview : "<empty>");
        return;
    }

    ESP_LOGI(kTag,
             "MQTT RX: %s cmd=%s state=%s progress=%d%% nozzle=%.1f/%.1f bed=%.1f/%.1f wifi=%s",
             topic_str.c_str(),
             print_command.empty() ? "-" : print_command.c_str(),
             gcode_state.empty() ? "-" : gcode_state.c_str(),
             mc_percent,
             nozzle_temper > -9999 ? nozzle_temper : -1.0,
             nozzle_target > -9999 ? nozzle_target : -1.0,
             bed_temper > -9999 ? bed_temper : -1.0,
             bed_target > -9999 ? bed_target : -1.0,
             wifi_signal.empty() ? "-" : wifi_signal.c_str());
}
}  // namespace

MqttService::MqttService()
    : client_(nullptr),
      app_queue_(nullptr),
      event_group_(nullptr),
      config_(default_config()),
      running_(false),
      chamber_light_known_(false),
      chamber_light_on_(false),
      chamber_light2_known_(false),
      chamber_light2_on_(false) {}
MqttService::~MqttService() { stop(); }

esp_err_t MqttService::start(const DeviceConfig &config, QueueHandle_t app_queue, EventGroupHandle_t event_group) {
    stop();
    config_ = sanitize_config(config);
    app_queue_ = app_queue;
    event_group_ = event_group;

    char uri[128];
    std::snprintf(uri, sizeof(uri), "%s://%s:%u", config_.mqtt_use_tls ? "mqtts" : "mqtt", config_.printer_ip, config_.mqtt_port);

    esp_mqtt_client_config_t mqtt_cfg = {};
    mqtt_cfg.broker.address.uri = uri;
    mqtt_cfg.broker.verification.certificate = kBambuCaCertPem;
    mqtt_cfg.broker.verification.common_name = config_.printer_serial;
    mqtt_cfg.broker.verification.skip_cert_common_name_check = false;
    mqtt_cfg.credentials.username = "bblp";
    mqtt_cfg.credentials.authentication.password = config_.printer_access_code;
    mqtt_cfg.session.keepalive = 30;
    mqtt_cfg.network.disable_auto_reconnect = false;
    mqtt_cfg.network.timeout_ms = 20000;

    ESP_LOGI(kTag, "Starting MQTT client: %s (tls=%s)", uri, config_.mqtt_use_tls ? "on" : "off");
    client_ = esp_mqtt_client_init(&mqtt_cfg);
    if (client_ == nullptr) {
        return ESP_FAIL;
    }

    esp_mqtt_client_register_event(client_, static_cast<esp_mqtt_event_id_t>(ESP_EVENT_ANY_ID), &MqttService::mqtt_event_handler, this);
    running_ = true;
    esp_err_t start_err = esp_mqtt_client_start(client_);
    if (start_err != ESP_OK) {
        ESP_LOGE(kTag, "esp_mqtt_client_start failed: %s", esp_err_to_name(start_err));
    }
    return start_err;
}

void MqttService::stop() {
    if (client_ != nullptr) {
        esp_mqtt_client_stop(client_);
        esp_mqtt_client_destroy(client_);
        client_ = nullptr;
    }
    if (event_group_ != nullptr) {
        xEventGroupClearBits(event_group_, kMqttConnectedBit);
    }
    running_ = false;
}

bool MqttService::is_running() const {
    return running_;
}

void MqttService::mqtt_event_handler(void *handler_args, esp_event_base_t, int32_t, void *event_data) {
    static_cast<MqttService *>(handler_args)->handle_event(static_cast<esp_mqtt_event_handle_t>(event_data));
}

void MqttService::handle_event(esp_mqtt_event_handle_t event) {
    switch (static_cast<esp_mqtt_event_id_t>(event->event_id)) {
        case MQTT_EVENT_BEFORE_CONNECT:
            ESP_LOGI(kTag, "MQTT connecting...");
            break;
        case MQTT_EVENT_CONNECTED: {
            ESP_LOGI(kTag, "MQTT connected");
            xEventGroupSetBits(event_group_, kMqttConnectedBit);
            post_event(AppEventType::MqttConnected);
            char report_topic[96];
            char request_topic[96];
            std::snprintf(report_topic, sizeof(report_topic), "device/%s/report", config_.printer_serial);
            std::snprintf(request_topic, sizeof(request_topic), "device/%s/request", config_.printer_serial);
            const int subscribe_id = esp_mqtt_client_subscribe(client_, report_topic, 0);
            ESP_LOGI(kTag, "Subscribed: %s (msg_id=%d)", report_topic, subscribe_id);
            const char *pushall = "{\"pushing\":{\"sequence_id\":\"1\",\"command\":\"pushall\",\"version\":1,\"push_target\":1}}";
            const int publish_id = esp_mqtt_client_publish(client_, request_topic, pushall, 0, 0, 0);
            ESP_LOGI(kTag, "Published pushall: %s (msg_id=%d)", request_topic, publish_id);
            break;
        }
        case MQTT_EVENT_ERROR: {
            if (event->error_handle != nullptr) {
                ESP_LOGE(kTag,
                         "MQTT error: type=%d tls_err=0x%x stack=%d verify=0x%x errno=%d",
                         static_cast<int>(event->error_handle->error_type),
                         static_cast<int>(event->error_handle->esp_tls_last_esp_err),
                         event->error_handle->esp_tls_stack_err,
                         event->error_handle->esp_tls_cert_verify_flags,
                         event->error_handle->esp_transport_sock_errno);
            } else {
                ESP_LOGE(kTag, "MQTT error: <no details>");
            }
            break;
        }
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(kTag, "MQTT disconnected");
            xEventGroupClearBits(event_group_, kMqttConnectedBit);
            post_event(AppEventType::MqttDisconnected);
            break;
        case MQTT_EVENT_DATA:
            if (event->data_len == event->total_data_len) {
                log_mqtt_payload_summary(event->topic, event->topic_len, event->data, event->data_len);
                handle_message(event->data, event->data_len);
            }
            break;
        default:
            break;
    }
}

void MqttService::post_event(AppEventType type) const {
    if (app_queue_ == nullptr) {
        return;
    }
    AppEvent event{};
    event.type = type;
    xQueueSend(app_queue_, &event, 0);
}

void MqttService::handle_message(const char *data, int len) {
    if (app_queue_ == nullptr) {
        return;
    }
    AppEvent event{};
    event.type = AppEventType::PrinterStatus;
    event.printer = parse_printer_status(data, len);
    xQueueSend(app_queue_, &event, 0);
}

PrinterStatusSnapshot MqttService::parse_printer_status(const char *json, int len) {
    PrinterStatusSnapshot snapshot{};
    snapshot.state = PrinterState::Idle;
    snapshot.progress = 0;
    snapshot.has_error = false;
    snapshot.chamber_light_known = chamber_light_known_;
    snapshot.chamber_light_on = chamber_light_on_;
    snapshot.chamber_light2_known = chamber_light2_known_;
    snapshot.chamber_light2_on = chamber_light2_on_;

    std::string payload(json, len);
    cJSON *root = cJSON_ParseWithLength(payload.c_str(), payload.size());
    if (root == nullptr) {
        return snapshot;
    }

    cJSON *print = cJSON_GetObjectItemCaseSensitive(root, "print");
    if (!cJSON_IsObject(print)) {
        print = nullptr;
    }

    std::string gcode_state;
    double nozzle_temper = 0;
    double nozzle_target = 0;
    double bed_temper = 0;
    double bed_target = 0;
    int print_error = 0;
    std::string fail_reason;
    cJSON *hms = nullptr;

    if (print != nullptr) {
        gcode_state = json_string(print, "gcode_state");
        nozzle_temper = json_number(print, "nozzle_temper");
        nozzle_target = json_number(print, "nozzle_target_temper");
        bed_temper = json_number(print, "bed_temper");
        bed_target = json_number(print, "bed_target_temper");
        print_error = json_int(print, "print_error");
        fail_reason = json_string(print, "fail_reason");
        hms = cJSON_GetObjectItemCaseSensitive(print, "hms");
        snapshot.progress = static_cast<uint8_t>(std::max(0, std::min(100, json_int(print, "mc_percent", 0))));
        snapshot.has_error = (print_error != 0) || (!fail_reason.empty() && fail_reason != "0") ||
                             (cJSON_IsArray(hms) && cJSON_GetArraySize(hms) > 0);
    }

    cJSON *system = cJSON_GetObjectItemCaseSensitive(root, "system");
    if (cJSON_IsObject(system)) {
        const std::string command = json_string(system, "command");
        const std::string led_node = json_string(system, "led_node");
        const std::string led_mode = json_string(system, "led_mode");
        if (command == "ledctrl" && led_node == "chamber_light" && !led_mode.empty()) {
            chamber_light_known_ = true;
            chamber_light_on_ = (led_mode == "on");
            snapshot.chamber_light_known = true;
            snapshot.chamber_light_on = chamber_light_on_;
        }
        if (command == "ledctrl" && led_node == "chamber_light2" && !led_mode.empty()) {
            chamber_light2_known_ = true;
            chamber_light2_on_ = (led_mode == "on");
            snapshot.chamber_light2_known = true;
            snapshot.chamber_light2_on = chamber_light2_on_;
        }
    }

    if (snapshot.has_error || gcode_state == "FAILED") {
        snapshot.state = PrinterState::Alert;
    } else if (gcode_state == "PAUSE" || gcode_state == "PAUSED") {
        snapshot.state = PrinterState::Paused;
    } else if (gcode_state == "FINISH" || gcode_state == "COMPLETED") {
        snapshot.state = PrinterState::Finished;
    } else if (gcode_state == "RUNNING" || gcode_state == "PRINTING") {
        snapshot.state = PrinterState::Printing;
    } else if ((nozzle_target > nozzle_temper + 5.0) || (bed_target > bed_temper + 3.0) || gcode_state == "PREPARE" || gcode_state == "HEATING") {
        snapshot.state = PrinterState::Heating;
    } else {
        snapshot.state = PrinterState::Idle;
    }

    cJSON_Delete(root);
    return snapshot;
}

}  // namespace bambuled
