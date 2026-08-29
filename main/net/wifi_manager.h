#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_wifi_types.h"
#include "nvs_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FISH_WIFI_SCAN_MAX 32

typedef void (*fish_wifi_connected_cb_t)(void *arg);

esp_err_t fish_wifi_init(void);
esp_err_t fish_wifi_scan(wifi_ap_record_t *out, uint16_t *count);
esp_err_t fish_wifi_connect(const fish_config_t *cfg);
bool fish_wifi_is_connected(void);
void fish_wifi_wait_connected(int timeout_ms);
void fish_wifi_set_connected_cb(fish_wifi_connected_cb_t cb, void *arg);

#ifdef __cplusplus
}
#endif
