#include "app.hpp"

extern "C" {
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
}

namespace bambuled {
namespace {
const char *TAG = "ConfigStore";
}

esp_err_t ConfigStore::init() {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

DeviceConfig ConfigStore::load() {
    nvs_handle_t handle = 0;
    DeviceConfig cfg = default_config();

    esp_err_t err = nvs_open(kNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return cfg;
    }

    size_t required = sizeof(DeviceConfig);
    err = nvs_get_blob(handle, kBlobKey, &cfg, &required);
    if (err == ESP_ERR_NVS_NOT_FOUND || required != sizeof(DeviceConfig)) {
        cfg = default_config();
        save(cfg);
        nvs_close(handle);
        return cfg;
    }

    nvs_close(handle);
    return sanitize_config(cfg);
}

esp_err_t ConfigStore::save(const DeviceConfig &config) {
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(kNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    DeviceConfig sanitized = sanitize_config(config);
    err = nvs_set_blob(handle, kBlobKey, &sanitized, sizeof(sanitized));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

}  // namespace bambuled
