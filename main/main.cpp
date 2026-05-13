#include "app.hpp"

extern "C" {
#include "esp_log.h"
}

using namespace bambuled;

extern "C" void app_main(void) {
    static AppController app;
    esp_err_t err = app.start();
    if (err != ESP_OK) {
        ESP_LOGE("BambuLED", "app start failed: %s", esp_err_to_name(err));
    }
}
