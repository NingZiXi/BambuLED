#include "app.hpp"

#include <algorithm>
#include <cstring>

#include "esp_log.h"

namespace bambuled {
namespace {
constexpr const char *kTag = "AppController";

LedCommand make_profile_command(const StateLightProfile &profile, uint8_t brightness, uint8_t fallback_speed) {
    return make_led_command(sanitize_effect(profile.effect, LedEffect::Breathe), profile.color, brightness,
                            profile.speed == 0 ? fallback_speed : profile.speed);
}

void log_printer_config(const DeviceConfig &config, const char *reason) {
    const bool has_ip = config.printer_ip[0] != '\0';
    const bool has_serial = config.printer_serial[0] != '\0';
    const bool has_code = config.printer_access_code[0] != '\0';

    char masked[24] = {0};
    const size_t code_len = std::strlen(config.printer_access_code);
    if (!has_code) {
        std::snprintf(masked, sizeof(masked), "<empty>");
    } else if (code_len <= 2) {
        std::snprintf(masked, sizeof(masked), "**");
    } else if (code_len <= 4) {
        std::snprintf(masked, sizeof(masked), "%c***", config.printer_access_code[0]);
    } else {
        std::snprintf(masked, sizeof(masked), "%c%c***%c%c", config.printer_access_code[0], config.printer_access_code[1],
                      config.printer_access_code[code_len - 2], config.printer_access_code[code_len - 1]);
    }

    ESP_LOGI(kTag,
             "Printer config (%s): ip=%s, mqtt_port=%u, tls=%s, serial=%s, access_code=%s, creds=%s",
             reason != nullptr ? reason : "update",
             has_ip ? config.printer_ip : "<empty>",
             static_cast<unsigned>(config.mqtt_port),
             config.mqtt_use_tls ? "on" : "off",
             has_serial ? config.printer_serial : "<empty>",
             masked,
             has_printer_credentials(config) ? "ready" : "incomplete");
}
}  // namespace

AppController::AppController()

    : app_queue_(nullptr),
      led_queue_(nullptr),
      event_group_(nullptr),
      app_task_handle_(nullptr),
      dns_server_handle_(nullptr),
      led_controller_() {}

AppController::~AppController() = default;

esp_err_t AppController::start() {
    ESP_ERROR_CHECK(config_store_.init());
    config_ = config_store_.load();
    log_printer_config(config_, "boot");

    app_queue_ = xQueueCreate(16, sizeof(AppEvent));
    led_queue_ = xQueueCreate(16, sizeof(LedMessage));
    event_group_ = xEventGroupCreate();
    if (app_queue_ == nullptr || led_queue_ == nullptr || event_group_ == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    led_controller_.bind_queue(led_queue_);
    ESP_ERROR_CHECK(led_controller_.start(config_));
    send_led_config(config_);
    apply_led_command(make_led_command(LedEffect::Breathe, config_.default_color, config_.brightness, config_.effect_speed));

    ESP_ERROR_CHECK(wifi_service_.start(config_, app_queue_, event_group_));
    ESP_ERROR_CHECK(web_server_.start(app_queue_, &config_store_, config_));
    web_server_.set_portal_ip(wifi_service_.portal_ip());
    web_server_.set_wifi_state((xEventGroupGetBits(event_group_) & kWifiConnectedBit) != 0, config_.wifi_ssid);

    if (xTaskCreate(&AppController::app_task_entry, "app_task", 6144, this, 4, &app_task_handle_) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void AppController::app_task_entry(void *arg) {
    static_cast<AppController *>(arg)->app_task_loop();
}

void AppController::app_task_loop() {
    AppEvent event{};
    while (true) {
        if (xQueueReceive(app_queue_, &event, portMAX_DELAY) == pdTRUE) {
            handle_event(event);
        }
    }
}

void AppController::handle_event(const AppEvent &event) {
    switch (event.type) {
        case AppEventType::WifiConnected:
            leave_portal_mode();
            web_server_.set_wifi_state(true, config_.wifi_ssid);
            start_mqtt_if_ready();
            apply_led_command(make_led_command(LedEffect::Breathe, make_color(255, 255, 255), config_.brightness, config_.effect_speed));
            break;
        case AppEventType::WifiDisconnected:
            mqtt_service_.stop();
            xEventGroupClearBits(event_group_, kMqttConnectedBit);
            web_server_.set_wifi_state(false, "");
            apply_led_command(make_led_command(LedEffect::Breathe, make_color(0, 24, 255), config_.brightness, config_.effect_speed));
            break;
        case AppEventType::PortalStarted:
            enter_portal_mode();
            break;
        case AppEventType::PortalStopped:
            leave_portal_mode();
            break;
        case AppEventType::ConfigUpdated:
            {
            const DeviceConfig previous = config_;
            config_ = sanitize_config(event.config);
            web_server_.set_config(config_);
            log_printer_config(config_, "saved");
            const bool wifi_credentials_changed =
                std::strncmp(previous.wifi_ssid, config_.wifi_ssid, sizeof(config_.wifi_ssid)) != 0 ||
                std::strncmp(previous.wifi_password, config_.wifi_password, sizeof(config_.wifi_password)) != 0;
            const bool wifi_connected = (xEventGroupGetBits(event_group_) & kWifiConnectedBit) != 0;
            if (!wifi_credentials_changed) {
                web_server_.set_wifi_state(wifi_connected, wifi_connected ? config_.wifi_ssid : "");
            }
            send_led_config(config_);
            wifi_service_.apply_config(config_);
            if (wifi_connected) {
                mqtt_service_.stop();
                start_mqtt_if_ready();
            }
            break;
            }
        case AppEventType::PrinterStatus:
            apply_led_command(map_printer_to_led(event.printer));
            break;
        case AppEventType::MqttConnected:
            break;
        case AppEventType::MqttDisconnected:
            apply_led_command(make_led_command(LedEffect::Breathe, make_color(255, 64, 160), config_.brightness, config_.effect_speed));
            break;
    }
}

void AppController::apply_led_command(const LedCommand &command) {
    if (led_queue_ == nullptr) {
        return;
    }
    LedMessage message{};
    message.type = LedMessageType::Command;
    message.command = command;
    xQueueSend(led_queue_, &message, 0);
}

LedCommand AppController::map_printer_to_led(const PrinterStatusSnapshot &printer) const {
    const bool chamber_known = printer.chamber_light_known || printer.chamber_light2_known;
    const bool chamber_on =
        (printer.chamber_light_known && printer.chamber_light_on) || (printer.chamber_light2_known && printer.chamber_light2_on);
    if (chamber_known) {
        if (chamber_on) {
            return make_led_command(LedEffect::Solid, make_color(255, 255, 255), config_.brightness, config_.effect_speed);
        }
        return make_led_command(LedEffect::Solid, make_color(0, 0, 0), 0, config_.effect_speed);
    }
    switch (printer.state) {
        case PrinterState::Idle:
            return make_profile_command(config_.idle_profile, config_.brightness, config_.effect_speed);
        case PrinterState::Heating:
            return make_profile_command(config_.heating_profile, config_.brightness, std::max<uint8_t>(config_.effect_speed, 30));
        case PrinterState::Printing: {
            LedCommand cmd = make_profile_command(config_.printing_profile, config_.brightness, config_.effect_speed);
            cmd.progress = printer.progress;
            cmd.secondary = config_.progress_background_color;
            return cmd;
        }
        case PrinterState::Finished:
            return make_profile_command(config_.finished_profile, config_.brightness, std::max<uint8_t>(10, config_.effect_speed / 2));
        case PrinterState::Paused:
            return make_profile_command(config_.paused_profile, config_.brightness, config_.effect_speed);
        case PrinterState::Alert:
            return make_profile_command(config_.alert_profile, config_.brightness, 8);
    }
    return make_led_command(LedEffect::Breathe, config_.default_color, config_.brightness, config_.effect_speed);
}

void AppController::send_led_config(const DeviceConfig &config) {
    if (led_queue_ == nullptr) {
        return;
    }
    LedMessage message{};
    message.type = LedMessageType::ApplyConfig;
    message.config = config;
    xQueueSend(led_queue_, &message, portMAX_DELAY);
}

void AppController::start_mqtt_if_ready() {
    if (has_printer_credentials(config_)) {
        log_printer_config(config_, "start_mqtt");
        mqtt_service_.start(config_, app_queue_, event_group_);
    }
}

void AppController::enter_portal_mode() {
    web_server_.set_portal_mode(true);
    if (dns_server_handle_ == nullptr) {
        dns_server_config_t config = DNS_SERVER_CONFIG_SINGLE("*", "WIFI_AP_DEF");
        dns_server_handle_ = start_dns_server(&config);
    }
}

void AppController::leave_portal_mode() {
    web_server_.set_portal_mode(false);
}

}  // namespace bambuled
