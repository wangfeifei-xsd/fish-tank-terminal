#pragma once

#include <stddef.h>

#include "cJSON.h"
#include "device_api.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FISH_MAX_FISH 16
#define FISH_MAX_DECO 8

typedef struct {
    char deco_key[32];
    char image_url[256];
    char png_path[64];
    char bin_path[64];
    float x;
    float y;
    float scale;
    float rotation_deg;
    float default_w_cm;
    float default_h_cm;
} fish_deco_item_t;

typedef struct {
    fish_tank_info_t tank;
    fish_item_t fish[FISH_MAX_FISH];
    int fish_count;
    fish_deco_item_t decos[FISH_MAX_DECO];
    int deco_count;
    fish_interaction_t interaction;
    char tank_json[FISH_API_RESP_MAX];
} fish_tank_state_t;

esp_err_t fish_cache_init(void);
esp_err_t fish_cache_load_local(fish_tank_state_t *state);
esp_err_t fish_cache_sync_tank(const char *tank_id, fish_tank_state_t *state);
/** Parse + save tank JSON only (no image downloads). Use before painting UI. */
esp_err_t fish_cache_apply_detail_meta(const char *json, fish_tank_state_t *state);
esp_err_t fish_cache_apply_detail_json(const char *json, fish_tank_state_t *state);
bool fish_cache_needs_update(const fish_tank_state_t *state, const char *new_json);
bool fish_cache_assets_local_ready(const fish_tank_state_t *state);
void fish_cache_sync_remote_assets(fish_tank_state_t *state);
const char *fish_cache_icon_path(const fish_tank_state_t *state, int fish_idx);
const char *fish_cache_icon_path_flip(const fish_tank_state_t *state, int fish_idx);
const char *fish_cache_bg_path(const fish_tank_state_t *state);
const char *fish_cache_bg_src_path(const fish_tank_state_t *state);
esp_err_t fish_cache_prepare_fish_sprite(const fish_item_t *fish, int target_w);
esp_err_t fish_cache_prepare_bg(fish_tank_info_t *tank, int target_w, int target_h);
esp_err_t fish_cache_prepare_deco(const fish_deco_item_t *deco, float px_per_cm, int *out_w, int *out_h);

void fish_cache_prepare_assets(fish_tank_state_t *state, int canvas_w, int canvas_h);
const char *fish_cache_deco_path(const fish_tank_state_t *state, int deco_idx);
void fish_cache_gc(const fish_tank_state_t *state);

#ifdef __cplusplus
}
#endif
