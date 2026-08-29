#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "nvs_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FISH_API_RESP_MAX 16384

typedef struct {
    int satiety;
    int water_quality;
    int algae_level;
    int algae_left;
    int algae_mid;
    int algae_right;
} fish_interaction_t;

typedef struct {
    char id[FISH_TANK_ID_LEN];
    char name[64];
    int length_cm;
    int width_cm;
    int height_cm;
    char updated_at[32];
    char image_url[256];
    char bg_path[64];
    char bg_bin_path[64];
} fish_tank_info_t;

typedef struct {
    char id[48];
    char name[48];
    char icon_path[64];
    char icon_path_flip[64];
    char icon_bin_path[64];
    char icon_url[256];
    char updated_at[32];
    float size_cm;
    char swim_layer[16];
    char temperament[16];
} fish_item_t;

typedef struct {
    float temperature1;
    float temperature2;
    int sensor_count;
    bool data_stale;
    int stale_minutes;
} fish_temp_info_t;

esp_err_t fish_api_init(const fish_config_t *cfg);
esp_err_t fish_api_health(void);
esp_err_t fish_api_tank_list(char *json_out, size_t out_len);
bool fish_api_pick_first_tank_id(const char *list_json, char *tank_id, size_t tank_id_len);
esp_err_t fish_api_fetch_first_tank_id(char *tank_id, size_t tank_id_len);
esp_err_t fish_api_tank_detail(const char *tank_id, char *json_out, size_t out_len);
esp_err_t fish_api_feed(const char *tank_id, fish_interaction_t *out);
esp_err_t fish_api_clean(const char *tank_id, const char *region, fish_interaction_t *out);
esp_err_t fish_api_water(const char *tank_id, const char *mode, fish_interaction_t *out);
esp_err_t fish_api_temp_latest(fish_temp_info_t *out);
esp_err_t fish_api_bind(const char *device_id, const char *device_name);
esp_err_t fish_api_deco_list(char *json_out, size_t out_len);

esp_err_t fish_api_parse_interaction(const char *json, fish_interaction_t *out);
esp_err_t fish_api_download_url(const char *url, const char *dest_path);

#ifdef __cplusplus
}
#endif
