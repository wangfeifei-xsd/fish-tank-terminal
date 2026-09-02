#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FISH_SERIAL_LEN   20
#define FISH_NAME_LEN     32
#define FISH_TANK_ID_LEN  48
#define FISH_WIFI_SSID_LEN 33
#define FISH_WIFI_PASS_LEN 65
#define FISH_KEY_LEN      64
#define FISH_BIND_TOKEN_LEN 33

typedef struct {
    char wifi_ssid[FISH_WIFI_SSID_LEN];
    char wifi_pass[FISH_WIFI_PASS_LEN];
    char device_id[FISH_SERIAL_LEN];
    char device_name[FISH_NAME_LEN];
    char tank_id[FISH_TANK_ID_LEN];
    char app_key[FISH_KEY_LEN];
    char app_secret[FISH_KEY_LEN];
    char bind_token[FISH_BIND_TOKEN_LEN];
    float temp_high;
    float temp_low;
    bool provisioned;
} fish_config_t;

esp_err_t fish_config_init(void);
esp_err_t fish_config_load(fish_config_t *cfg);
esp_err_t fish_config_save(const fish_config_t *cfg);
bool fish_config_has_wifi(const fish_config_t *cfg);
void fish_config_generate_serial(char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
