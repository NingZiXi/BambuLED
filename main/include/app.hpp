#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

extern "C" {
#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "dns_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "mqtt_client.h"
}

namespace bambuled {

constexpr uint32_t kConfigVersion = 2;
constexpr EventBits_t kWifiConnectedBit = BIT0;
constexpr EventBits_t kMqttConnectedBit = BIT1;
constexpr EventBits_t kPortalModeBit = BIT2;

struct RgbColor {
    uint8_t r;
    uint8_t g;
    uint8_t b;
};

struct StateLightProfile {
    uint8_t effect;
    uint8_t speed;
    RgbColor color;
};

enum class LedEffect : uint8_t {
    Solid = 0,
    Breathe,
    Fade,
    Chase,
    Rainbow,
    Progress,
    Blink,
    Off,
};

enum class LedMessageType : uint8_t {
    Command = 0,
    ApplyConfig,
};

enum class PrinterState : uint8_t {
    Idle = 0,
    Heating,
    Printing,
    Finished,
    Paused,
    Alert,
};

enum class AppEventType : uint8_t {
    WifiConnected = 0,
    WifiDisconnected,
    PortalStarted,
    PortalStopped,
    ConfigUpdated,
    PrinterStatus,
    MqttConnected,
    MqttDisconnected,
};

enum class WifiConnectState : uint8_t {
    Idle = 0,
    Connecting,
    Failed,
    RestartPending,
};

struct DeviceConfig {
    uint32_t version;
    char wifi_ssid[33];
    char wifi_password[65];
    char printer_ip[64];
    uint16_t mqtt_port;
    bool mqtt_use_tls;
    char printer_access_code[65];
    char printer_serial[32];
    uint16_t led_count;
    int16_t led_gpio;
    uint8_t brightness;
    uint8_t effect_speed;
    RgbColor default_color;
    StateLightProfile idle_profile;
    StateLightProfile heating_profile;
    StateLightProfile printing_profile;
    StateLightProfile finished_profile;
    StateLightProfile paused_profile;
    StateLightProfile alert_profile;
    RgbColor progress_background_color;
};

struct PrinterStatusSnapshot {
    PrinterState state;
    uint8_t progress;
    bool has_error;
    bool chamber_light_known;
    bool chamber_light_on;
    bool chamber_light2_known;
    bool chamber_light2_on;
};

struct LedCommand {
    LedEffect effect;
    RgbColor primary;
    RgbColor secondary;
    uint8_t brightness;
    uint8_t speed;
    uint8_t progress;
};

struct LedMessage {
    LedMessageType type;
    LedCommand command;
    DeviceConfig config;
};

struct AppEvent {
    AppEventType type;
    DeviceConfig config;
    PrinterStatusSnapshot printer;
};

template <size_t N>
inline void copy_cstr(char (&dest)[N], const char *src) {
    if (src == nullptr) {
        dest[0] = '\0';
        return;
    }
    std::strncpy(dest, src, N - 1);
    dest[N - 1] = '\0';
}

inline RgbColor make_color(uint8_t r, uint8_t g, uint8_t b) {
    return RgbColor{r, g, b};
}

inline uint8_t effect_value(LedEffect effect) {
    return static_cast<uint8_t>(effect);
}

inline LedEffect sanitize_effect(uint8_t effect, LedEffect fallback) {
    return effect <= effect_value(LedEffect::Off) ? static_cast<LedEffect>(effect) : fallback;
}

inline StateLightProfile make_profile(LedEffect effect, RgbColor color, uint8_t speed) {
    return StateLightProfile{effect_value(effect), speed, color};
}

inline DeviceConfig default_config() {
    DeviceConfig cfg{};
    cfg.version = kConfigVersion;
    cfg.mqtt_port = 8883;
    cfg.mqtt_use_tls = true;
    cfg.led_count = 10;
    cfg.led_gpio = 1;
    cfg.brightness = 64;
    cfg.effect_speed = 40;
    cfg.default_color = make_color(32, 32, 32);
    cfg.idle_profile = make_profile(LedEffect::Solid, make_color(0, 0, 0), 40);
    cfg.heating_profile = make_profile(LedEffect::Blink, make_color(255, 170, 48), 70);
    cfg.printing_profile = make_profile(LedEffect::Progress, make_color(0, 96, 255), 40);
    cfg.finished_profile = make_profile(LedEffect::Rainbow, make_color(96, 32, 255), 18);
    cfg.paused_profile = make_profile(LedEffect::Breathe, make_color(255, 128, 0), 45);
    cfg.alert_profile = make_profile(LedEffect::Blink, make_color(255, 0, 0), 8);
    cfg.progress_background_color = make_color(4, 4, 12);
    return cfg;
}

inline bool has_wifi_credentials(const DeviceConfig &cfg) {
    return cfg.wifi_ssid[0] != '\0';
}

inline bool has_printer_credentials(const DeviceConfig &cfg) {
    return cfg.printer_ip[0] != '\0' && cfg.printer_access_code[0] != '\0' && cfg.printer_serial[0] != '\0';
}

inline DeviceConfig sanitize_config(const DeviceConfig &input) {
    DeviceConfig cfg = input;
    if (cfg.version != kConfigVersion) {
        cfg = default_config();
    }
    if (cfg.mqtt_port == 0) {
        cfg.mqtt_port = 8883;
    }
    if (cfg.printer_serial[0] != '\0' && cfg.mqtt_port == 1883 && cfg.mqtt_use_tls) {
        cfg.mqtt_port = 8883;
    }
    if (cfg.printer_serial[0] != '\0' && cfg.mqtt_port == 1883 && !cfg.mqtt_use_tls) {
        cfg.mqtt_port = 8883;
        cfg.mqtt_use_tls = true;
    }
    if (cfg.led_count == 0 || cfg.led_count > 300) {
        cfg.led_count = 10;
    }
    if (cfg.led_gpio < 0) {
        cfg.led_gpio = 1;
    }
    if (cfg.brightness == 0) {
        cfg.brightness = 64;
    }
    if (cfg.effect_speed == 0) {
        cfg.effect_speed = 40;
    }
    cfg.idle_profile.effect = effect_value(sanitize_effect(cfg.idle_profile.effect, LedEffect::Breathe));
    cfg.heating_profile.effect = effect_value(sanitize_effect(cfg.heating_profile.effect, LedEffect::Blink));
    cfg.printing_profile.effect = effect_value(sanitize_effect(cfg.printing_profile.effect, LedEffect::Progress));
    cfg.finished_profile.effect = effect_value(sanitize_effect(cfg.finished_profile.effect, LedEffect::Rainbow));
    cfg.paused_profile.effect = effect_value(sanitize_effect(cfg.paused_profile.effect, LedEffect::Breathe));
    cfg.alert_profile.effect = effect_value(sanitize_effect(cfg.alert_profile.effect, LedEffect::Blink));
    if (cfg.idle_profile.speed == 0) {
        cfg.idle_profile.speed = 40;
    }
    if (cfg.heating_profile.speed == 0) {
        cfg.heating_profile.speed = 70;
    }
    if (cfg.printing_profile.speed == 0) {
        cfg.printing_profile.speed = 40;
    }
    if (cfg.finished_profile.speed == 0) {
        cfg.finished_profile.speed = 18;
    }
    if (cfg.paused_profile.speed == 0) {
        cfg.paused_profile.speed = 45;
    }
    if (cfg.alert_profile.speed == 0) {
        cfg.alert_profile.speed = 8;
    }
    return cfg;
}

inline LedCommand make_led_command(LedEffect effect, RgbColor color, uint8_t brightness, uint8_t speed, uint8_t progress = 0) {
    LedCommand cmd{};
    cmd.effect = effect;
    cmd.primary = color;
    cmd.secondary = make_color(0, 0, 0);
    cmd.brightness = brightness;
    cmd.speed = speed;
    cmd.progress = progress;
    return cmd;
}

class ConfigStore {
public:
    esp_err_t init();
    DeviceConfig load();
    esp_err_t save(const DeviceConfig &config);

private:
    static constexpr const char *kNamespace = "bambuled";
    static constexpr const char *kBlobKey = "cfg";
};

class LedController {
public:
    LedController();
    ~LedController();

    void bind_queue(QueueHandle_t led_queue);
    esp_err_t start(const DeviceConfig &config);
    void stop();

private:
    static void task_entry(void *arg);
    void task_loop();
    esp_err_t recreate_strip(const DeviceConfig &config);
    void render_frame(uint32_t tick_ms);
    void render_solid(const RgbColor &color, uint8_t scale);
    void render_breathe(const RgbColor &color, uint32_t tick_ms);
    void render_fade(const RgbColor &color, uint32_t tick_ms);
    void render_blink(const RgbColor &color, uint32_t tick_ms);
    void render_chase(const RgbColor &color, uint32_t tick_ms);
    void render_rainbow(uint32_t tick_ms);
    void render_progress(const RgbColor &color);
    void set_all(uint8_t r, uint8_t g, uint8_t b);
    uint8_t scaled(uint8_t value, uint8_t scale) const;

    QueueHandle_t led_queue_;
    TaskHandle_t task_handle_;
    led_strip_handle_t strip_;
    DeviceConfig config_;
    LedCommand current_command_;
    volatile bool running_;
};

class MqttService {
public:
    MqttService();
    ~MqttService();

    esp_err_t start(const DeviceConfig &config, QueueHandle_t app_queue, EventGroupHandle_t event_group);
    void stop();
    bool is_running() const;

private:
    static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);
    void handle_event(esp_mqtt_event_handle_t event);
    void post_event(AppEventType type) const;
    void handle_message(const char *data, int len);
    PrinterStatusSnapshot parse_printer_status(const char *json, int len);

    esp_mqtt_client_handle_t client_;
    QueueHandle_t app_queue_;
    EventGroupHandle_t event_group_;
    DeviceConfig config_;
    bool running_;
    bool chamber_light_known_;
    bool chamber_light_on_;
    bool chamber_light2_known_;
    bool chamber_light2_on_;
};

class WebServer {
public:
    WebServer();
    ~WebServer();

    esp_err_t start(QueueHandle_t app_queue, ConfigStore *store, const DeviceConfig &config);
    void stop();
    void set_config(const DeviceConfig &config);
    void set_portal_mode(bool enabled);
    void set_portal_ip(uint32_t ip_addr);
    void begin_wifi_connect_attempt(const DeviceConfig &config);
    void set_wifi_state(bool connected, const char *ssid);
    bool is_running() const;

private:
    static esp_err_t root_get_handler(httpd_req_t *req);
    static esp_err_t api_get_handler(httpd_req_t *req);
    static esp_err_t config_post_handler(httpd_req_t *req);
    static esp_err_t wifi_disconnect_post_handler(httpd_req_t *req);
    static esp_err_t portal_probe_get_handler(httpd_req_t *req);
    static esp_err_t error_handler_404(httpd_req_t *req, httpd_err_code_t err);
    static esp_err_t error_handler_414(httpd_req_t *req, httpd_err_code_t err);

    esp_err_t handle_root(httpd_req_t *req);
    esp_err_t handle_api_get(httpd_req_t *req);
    esp_err_t handle_config_post(httpd_req_t *req);
    esp_err_t handle_wifi_disconnect(httpd_req_t *req);
    esp_err_t handle_portal_probe(httpd_req_t *req);

    static std::string url_decode(const std::string &text);
    static std::string get_form_value(const std::string &body, const char *key);
    static bool is_checked(const std::string &body, const char *key);
    static void schedule_restart(uint32_t delay_ms);
    esp_err_t send_json_result(httpd_req_t *req, bool ok, bool restart_required, const char *message, const char *detail);
    void refresh_wifi_connect_state();
    std::string render_index_html() const;

    httpd_handle_t server_;
    QueueHandle_t app_queue_;
    ConfigStore *store_;
    DeviceConfig config_;
    DeviceConfig saved_config_;
    bool portal_mode_;
    uint32_t portal_ip_addr_;
    bool wifi_connected_;
    bool restart_scheduled_;
    WifiConnectState wifi_connect_state_;
    TickType_t wifi_connect_started_at_;
    char connected_wifi_ssid_[33];
    char wifi_connect_target_ssid_[33];
    char wifi_connect_message_[96];
    char wifi_connect_detail_[160];
};

class WifiService {
public:
    WifiService();
    ~WifiService();

    esp_err_t start(const DeviceConfig &config, QueueHandle_t app_queue, EventGroupHandle_t event_group);
    esp_err_t apply_config(const DeviceConfig &config);
    void stop();
    uint32_t portal_ip() const;

private:
    static void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);
    void handle_event(esp_event_base_t event_base, int32_t event_id, void *event_data);
    esp_err_t start_station();
    esp_err_t start_portal();
    esp_err_t stop_portal();
    void configure_dhcp_captive_portal() const;
    void post_simple_event(AppEventType type) const;
    esp_err_t update_mode();
    void generate_ap_ssid();

    QueueHandle_t app_queue_;
    EventGroupHandle_t event_group_;
    esp_netif_t *sta_netif_;
    esp_netif_t *ap_netif_;
    DeviceConfig config_;
    char ap_ssid_[32];
    bool started_;
    bool portal_active_;
    int reconnect_attempts_;
};

class AppController {
public:
    AppController();
    ~AppController();

    esp_err_t start();

private:
    static void app_task_entry(void *arg);
    void app_task_loop();
    void handle_event(const AppEvent &event);
    void apply_led_command(const LedCommand &command);
    LedCommand map_printer_to_led(const PrinterStatusSnapshot &printer) const;
    void send_led_config(const DeviceConfig &config);
    void start_mqtt_if_ready();
    void enter_portal_mode();
    void leave_portal_mode();

    QueueHandle_t app_queue_;
    QueueHandle_t led_queue_;
    EventGroupHandle_t event_group_;
    TaskHandle_t app_task_handle_;
    dns_server_handle_t dns_server_handle_;

    DeviceConfig config_;
    ConfigStore config_store_;
    LedController led_controller_;
    MqttService mqtt_service_;
    WebServer web_server_;
    WifiService wifi_service_;
};

}  // namespace bambuled
