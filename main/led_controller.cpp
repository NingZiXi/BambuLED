#include "app.hpp"

#include <algorithm>
#include <cmath>

extern "C" {
#include "led_strip_rmt.h"
}

namespace bambuled {
namespace {
constexpr uint32_t kLedRmtResolutionHz = 10 * 1000 * 1000;
}

LedController::LedController()
    : led_queue_(nullptr),
      task_handle_(nullptr),
      strip_(nullptr),
      config_(default_config()),
      current_command_(make_led_command(LedEffect::Breathe, make_color(32, 32, 32), 64, 40)),
      running_(false) {}

LedController::~LedController() {
    stop();
}

void LedController::bind_queue(QueueHandle_t led_queue) {
    led_queue_ = led_queue;
}

esp_err_t LedController::start(const DeviceConfig &config) {
    if (led_queue_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    config_ = sanitize_config(config);
    running_ = true;
    if (xTaskCreate(&LedController::task_entry, "led_task", 6144, this, 3, &task_handle_) != pdPASS) {
        running_ = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void LedController::stop() {
    running_ = false;
    if (task_handle_ != nullptr) {
        for (int i = 0; i < 20 && task_handle_ != nullptr; ++i) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
    if (strip_ != nullptr) {
        led_strip_clear(strip_);
        led_strip_del(strip_);
        strip_ = nullptr;
    }
}

void LedController::task_entry(void *arg) {
    static_cast<LedController *>(arg)->task_loop();
}

void LedController::task_loop() {
    uint32_t tick_ms = 0;
    while (running_) {
        LedMessage message{};
        while (xQueueReceive(led_queue_, &message, 0) == pdTRUE) {
            if (message.type == LedMessageType::ApplyConfig) {
                recreate_strip(message.config);
                current_command_ = make_led_command(LedEffect::Breathe, config_.default_color, config_.brightness, config_.effect_speed);
            } else {
                current_command_ = message.command;
            }
        }

        if (strip_ != nullptr) {
            render_frame(tick_ms);
        }
        tick_ms += 30;
        vTaskDelay(pdMS_TO_TICKS(30));
    }

    task_handle_ = nullptr;
    vTaskDelete(nullptr);
}

esp_err_t LedController::recreate_strip(const DeviceConfig &config) {
    config_ = sanitize_config(config);
    if (strip_ != nullptr) {
        led_strip_clear(strip_);
        led_strip_del(strip_);
        strip_ = nullptr;
    }

    led_strip_config_t strip_config = {};
    strip_config.strip_gpio_num = config_.led_gpio;
    strip_config.max_leds = config_.led_count;
    strip_config.led_model = LED_MODEL_WS2812;
    strip_config.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;
    strip_config.flags.invert_out = false;

    led_strip_rmt_config_t rmt_config = {};
    rmt_config.clk_src = RMT_CLK_SRC_DEFAULT;
    rmt_config.resolution_hz = kLedRmtResolutionHz;
    rmt_config.mem_block_symbols = 0;
    rmt_config.flags.with_dma = false;

    esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &strip_);
    if (err == ESP_OK) {
        led_strip_clear(strip_);
    }
    return err;
}

void LedController::render_frame(uint32_t tick_ms) {
    switch (current_command_.effect) {
        case LedEffect::Solid:
            render_solid(current_command_.primary, current_command_.brightness);
            break;
        case LedEffect::Breathe:
            render_breathe(current_command_.primary, tick_ms);
            break;
        case LedEffect::Fade:
            render_fade(current_command_.primary, tick_ms);
            break;
        case LedEffect::Chase:
            render_chase(current_command_.primary, tick_ms);
            break;
        case LedEffect::Rainbow:
            render_rainbow(tick_ms);
            break;
        case LedEffect::Progress:
            render_progress(current_command_.primary);
            break;
        case LedEffect::Blink:
            render_blink(current_command_.primary, tick_ms);
            break;
        case LedEffect::Off:
            set_all(0, 0, 0);
            break;
    }
    led_strip_refresh(strip_);
}

void LedController::render_solid(const RgbColor &color, uint8_t scale) {
    set_all(scaled(color.r, scale), scaled(color.g, scale), scaled(color.b, scale));
}

void LedController::render_breathe(const RgbColor &color, uint32_t tick_ms) {
    float phase = std::sin((tick_ms / static_cast<float>(std::max<uint8_t>(1, current_command_.speed) * 25.0f)) * 2.0f * 3.1415926f);
    uint8_t scale = static_cast<uint8_t>(20 + ((phase + 1.0f) * 0.5f) * current_command_.brightness);
    render_solid(color, scale);
}

void LedController::render_fade(const RgbColor &color, uint32_t tick_ms) {
    uint32_t period = std::max<uint32_t>(300, current_command_.speed * 40U);
    uint32_t local = tick_ms % period;
    float factor = (local < period / 2) ? (static_cast<float>(local) / (period / 2)) : (1.0f - static_cast<float>(local - period / 2) / (period / 2));
    render_solid(color, static_cast<uint8_t>(factor * current_command_.brightness));
}

void LedController::render_blink(const RgbColor &color, uint32_t tick_ms) {
    uint32_t period = std::max<uint32_t>(100, current_command_.speed * 10U);
    bool on = ((tick_ms / period) % 2U) == 0U;
    render_solid(color, on ? current_command_.brightness : 0);
}

void LedController::render_chase(const RgbColor &color, uint32_t tick_ms) {
    set_all(0, 0, 0);
    uint32_t delay = std::max<uint32_t>(40, 260U - static_cast<uint32_t>(current_command_.speed) * 5U);
    uint16_t count = std::max<uint16_t>(1, config_.led_count);
    uint16_t head = static_cast<uint16_t>((tick_ms / delay) % count);
    for (uint16_t i = 0; i < count; ++i) {
        uint16_t distance = static_cast<uint16_t>((i + count - head) % count);
        uint8_t fade = (distance < 6) ? static_cast<uint8_t>((6 - distance) * current_command_.brightness / 6) : 0;
        led_strip_set_pixel(strip_, i, scaled(color.r, fade), scaled(color.g, fade), scaled(color.b, fade));
    }
}

void LedController::render_rainbow(uint32_t tick_ms) {
    uint16_t count = std::max<uint16_t>(1, config_.led_count);
    for (uint16_t i = 0; i < count; ++i) {
        uint16_t hue = static_cast<uint16_t>((i * 360 / count + (tick_ms / std::max<uint8_t>(1, current_command_.speed))) % 360);
        led_strip_set_pixel_hsv(strip_, i, hue, 255, current_command_.brightness);
    }
}

void LedController::render_progress(const RgbColor &color) {
    uint16_t count = std::max<uint16_t>(1, config_.led_count);
    uint16_t lit = static_cast<uint16_t>((count * current_command_.progress) / 100U);
    for (uint16_t i = 0; i < count; ++i) {
        if (i < lit) {
            led_strip_set_pixel(strip_, i, scaled(color.r, current_command_.brightness), scaled(color.g, current_command_.brightness), scaled(color.b, current_command_.brightness));
        } else {
            led_strip_set_pixel(strip_, i, scaled(current_command_.secondary.r, current_command_.brightness),
                                scaled(current_command_.secondary.g, current_command_.brightness),
                                scaled(current_command_.secondary.b, current_command_.brightness));
        }
    }
}

void LedController::set_all(uint8_t r, uint8_t g, uint8_t b) {
    uint16_t count = std::max<uint16_t>(1, config_.led_count);
    for (uint16_t i = 0; i < count; ++i) {
        led_strip_set_pixel(strip_, i, r, g, b);
    }
}

uint8_t LedController::scaled(uint8_t value, uint8_t scale) const {
    return static_cast<uint8_t>((static_cast<uint16_t>(value) * scale) / 255U);
}

}  // namespace bambuled
