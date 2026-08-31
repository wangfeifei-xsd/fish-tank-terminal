#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "nvs_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*fish_ble_notify_cb_t)(const char *json);

esp_err_t fish_ble_init(const fish_config_t *cfg, bool provisioning);
void fish_ble_set_pin_display_cb(void (*cb)(const char *pin));
void fish_ble_set_notify_cb(fish_ble_notify_cb_t cb);
const char *fish_ble_get_pin(void);
void fish_ble_refresh_pin(void);
bool fish_ble_is_provisioning(void);
esp_err_t fish_ble_notify_param(const char *json);

#ifdef __cplusplus
}
#endif
