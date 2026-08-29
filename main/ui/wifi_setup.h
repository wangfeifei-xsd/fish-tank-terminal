#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "nvs_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Show on-screen WiFi setup (scan list + soft keyboard).
 * Blocks until credentials are saved and connection succeeds, then returns ESP_OK.
 * On cancel/fail returns an error; caller may keep BLE provisioning available.
 */
esp_err_t fish_wifi_setup_run(fish_config_t *cfg);

#ifdef __cplusplus
}
#endif
