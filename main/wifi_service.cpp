#include "app.hpp"

#include <cstdio>
#include <cstring>

extern "C" {
#include "esp_log.h"
#include "esp_mac.h"
#include "lwip/inet.h"
}

namespace bambuled {
namespace {
constexpr const char *kTag = "WifiService";
}

WifiService::WifiService()
    : app_queue_(nullptr),
      event_group_(nullptr),
      sta_netif_(nullptr),
      ap_netif_(nullptr),
      config_(default_config()),
      ap_ssid_{0},
      started_(false),
      portal_active_(false),
      reconnect_attempts_(0) {}

WifiService::~WifiService() {
    stop();
}

esp_err_t WifiService::start(const DeviceConfig &config, QueueHandle_t app_queue, EventGroupHandle_t event_group) {
    if (started_) {
        return apply_config(config);
    }

    config_ = sanitize_config(config);
    app_queue_ = app_queue;
    event_group_ = event_group;

    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t loop_err = esp_event_loop_create_default();
    if (loop_err != ESP_OK && loop_err != ESP_ERR_INVALID_STATE) {
        return loop_err;
    }

    sta_netif_ = esp_netif_create_default_wifi_sta();
    ap_netif_ = esp_netif_create_default_wifi_ap();

    wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_init_cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &WifiService::event_handler, this));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &WifiService::event_handler, this));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    generate_ap_ssid();
    started_ = true;
    return apply_config(config_);
}

esp_err_t WifiService::apply_config(const DeviceConfig &config) {
    config_ = sanitize_config(config);
    reconnect_attempts_ = 0;
    return update_mode();
}

void WifiService::stop() {
    if (!started_) {
        return;
    }
    esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &WifiService::event_handler);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &WifiService::event_handler);
    esp_wifi_stop();
    esp_wifi_deinit();
    started_ = false;
}

uint32_t WifiService::portal_ip() const {
    esp_netif_ip_info_t ip_info{};
    if (ap_netif_ != nullptr && esp_netif_get_ip_info(ap_netif_, &ip_info) == ESP_OK) {
        return ip_info.ip.addr;
    }
    return inet_addr("192.168.4.1");
}

void WifiService::event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    static_cast<WifiService *>(arg)->handle_event(event_base, event_id, event_data);
}

void WifiService::handle_event(esp_event_base_t event_base, int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(kTag, "STA started for SSID: %s", config_.wifi_ssid);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(event_group_, kWifiConnectedBit);
        post_simple_event(AppEventType::WifiDisconnected);
        wifi_event_sta_disconnected_t *disconnected = static_cast<wifi_event_sta_disconnected_t *>(event_data);
        ESP_LOGW(kTag, "STA disconnected from SSID: %s, reason=%d, retry=%d", config_.wifi_ssid,
                 disconnected != nullptr ? disconnected->reason : -1, reconnect_attempts_);
        if (has_wifi_credentials(config_)) {
            ++reconnect_attempts_;
            esp_wifi_connect();
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        reconnect_attempts_ = 0;
        xEventGroupSetBits(event_group_, kWifiConnectedBit);
        ESP_LOGI(kTag, "STA got IP address");
        post_simple_event(AppEventType::WifiConnected);
    }
}

esp_err_t WifiService::start_station() {
    ESP_LOGI(kTag, "Starting station mode for SSID: %s", config_.wifi_ssid);
    const bool wifi_already_running = portal_active_;
    wifi_config_t ap_cfg = {};
    std::strncpy(reinterpret_cast<char *>(ap_cfg.ap.ssid), ap_ssid_, sizeof(ap_cfg.ap.ssid) - 1);
    ap_cfg.ap.ssid_len = std::strlen(ap_ssid_);
    ap_cfg.ap.channel = 1;
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN;

    wifi_config_t sta_cfg = {};
    std::strncpy(reinterpret_cast<char *>(sta_cfg.sta.ssid), config_.wifi_ssid, sizeof(sta_cfg.sta.ssid) - 1);
    std::strncpy(reinterpret_cast<char *>(sta_cfg.sta.password), config_.wifi_password, sizeof(sta_cfg.sta.password) - 1);
    sta_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    sta_cfg.sta.pmf_cfg.capable = true;
    sta_cfg.sta.pmf_cfg.required = false;

    if (wifi_already_running) {
        esp_err_t disconnect_err = esp_wifi_disconnect();
        if (disconnect_err != ESP_OK && disconnect_err != ESP_ERR_WIFI_NOT_CONNECT) {
            ESP_LOGW(kTag, "esp_wifi_disconnect before reconfigure failed: %s", esp_err_to_name(disconnect_err));
        }

        esp_err_t stop_err = esp_wifi_stop();
        if (stop_err != ESP_OK) {
            ESP_LOGE(kTag, "Failed to stop WiFi before STA reconfigure: %s", esp_err_to_name(stop_err));
            return stop_err;
        }
    }

    esp_err_t mode_err = esp_wifi_set_mode(portal_active_ ? WIFI_MODE_APSTA : WIFI_MODE_STA);
    if (mode_err != ESP_OK) {
        ESP_LOGE(kTag, "Failed to set WiFi mode before STA config: %s", esp_err_to_name(mode_err));
        return mode_err;
    }

    if (portal_active_) {
        esp_err_t ap_config_err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
        if (ap_config_err != ESP_OK) {
            ESP_LOGE(kTag, "Failed to set AP config before STA config: %s", esp_err_to_name(ap_config_err));
            return ap_config_err;
        }
    }

    esp_err_t config_err = esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    if (config_err != ESP_OK) {
        ESP_LOGE(kTag, "Failed to set STA config for SSID %s: %s", config_.wifi_ssid, esp_err_to_name(config_err));
        return config_err;
    }

    esp_err_t start_err = esp_wifi_start();
    if (start_err != ESP_OK) {
        ESP_LOGE(kTag, "Failed to start WiFi for SSID %s: %s", config_.wifi_ssid, esp_err_to_name(start_err));
        return start_err;
    }

    if (portal_active_) {
        configure_dhcp_captive_portal();
    }
    ESP_LOGI(kTag, "Triggering STA connect for SSID: %s", config_.wifi_ssid);
    esp_err_t connect_err = esp_wifi_connect();
    if (connect_err == ESP_ERR_WIFI_CONN) {
        ESP_LOGI(kTag, "STA is already connecting to SSID: %s", config_.wifi_ssid);
        return ESP_OK;
    }
    if (connect_err != ESP_OK) {
        ESP_LOGE(kTag, "Failed to trigger STA connect for SSID %s: %s", config_.wifi_ssid, esp_err_to_name(connect_err));
        return connect_err;
    }
    return ESP_OK;
}

esp_err_t WifiService::start_portal() {
    if (portal_active_) {
        return ESP_OK;
    }

    wifi_config_t ap_cfg = {};
    std::strncpy(reinterpret_cast<char *>(ap_cfg.ap.ssid), ap_ssid_, sizeof(ap_cfg.ap.ssid) - 1);
    ap_cfg.ap.ssid_len = std::strlen(ap_ssid_);
    ap_cfg.ap.channel = 1;
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(has_wifi_credentials(config_) ? WIFI_MODE_APSTA : WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    configure_dhcp_captive_portal();
    portal_active_ = true;
    xEventGroupSetBits(event_group_, kPortalModeBit);
    post_simple_event(AppEventType::PortalStarted);
    return ESP_OK;
}

esp_err_t WifiService::stop_portal() {
    if (!portal_active_) {
        return ESP_OK;
    }
    portal_active_ = false;
    xEventGroupClearBits(event_group_, kPortalModeBit);
    post_simple_event(AppEventType::PortalStopped);
    if (has_wifi_credentials(config_)) {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    }
    return ESP_OK;
}

void WifiService::configure_dhcp_captive_portal() const {
    if (ap_netif_ == nullptr) {
        return;
    }

    esp_netif_ip_info_t ip_info{};
    if (esp_netif_get_ip_info(ap_netif_, &ip_info) != ESP_OK) {
        return;
    }

    char ip_addr[16] = {0};
    inet_ntoa_r(ip_info.ip.addr, ip_addr, sizeof(ip_addr));

    char captive_portal_uri[32] = {0};
    std::snprintf(captive_portal_uri, sizeof(captive_portal_uri), "http://%s", ip_addr);

    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_stop(ap_netif_));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_option(ap_netif_, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI,
                                                         captive_portal_uri, std::strlen(captive_portal_uri)));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_start(ap_netif_));
}

void WifiService::post_simple_event(AppEventType type) const {
    if (app_queue_ == nullptr) {
        return;
    }
    AppEvent event{};
    event.type = type;
    xQueueSend(app_queue_, &event, 0);
}

esp_err_t WifiService::update_mode() {
    ESP_ERROR_CHECK(start_portal());
    if (has_wifi_credentials(config_)) {
        return start_station();
    } else {
        ESP_LOGI(kTag, "No WiFi credentials saved, switching to AP only");
        esp_err_t err = esp_wifi_disconnect();
        if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_CONNECT) {
            ESP_LOGW(kTag, "esp_wifi_disconnect failed: %s", esp_err_to_name(err));
        }
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    }
    return ESP_OK;
}

void WifiService::generate_ap_ssid() {
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    std::snprintf(ap_ssid_, sizeof(ap_ssid_), "BambuLED-%02X%02X", mac[4], mac[5]);
}

}  // namespace bambuled
