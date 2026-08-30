#pragma once

#include "anim_engine.h"
#include "nvs_config.h"
#include "resource_cache.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    fish_config_t *cfg;
    fish_tank_state_t *tank;
    anim_engine_t *anim;
    lv_obj_t *screen;
    lv_obj_t *lbl_title;
    lv_obj_t *lbl_temp;
    lv_obj_t *btn_wifi;
    lv_obj_t *lbl_wifi;
    lv_obj_t *bar_satiety;
    lv_obj_t *lbl_satiety;
    lv_obj_t *provision_panel;
    lv_obj_t *lbl_pin;
    lv_obj_t *toast_bar;
    lv_obj_t *toast_lbl;
    lv_obj_t *loading_panel;
    lv_timer_t *toast_timer;
    lv_timer_t *wifi_timer;
    bool content_ready;
    bool provisioning;
    bool wifi_connected_last;
    bool wifi_status_known;
} fish_ui_t;

fish_ui_t *fish_ui_create(fish_config_t *cfg, bool provisioning);
void fish_ui_destroy(fish_ui_t *ui);
void fish_ui_set_tank(fish_ui_t *ui, fish_tank_state_t *tank);
void fish_ui_set_temp(fish_ui_t *ui, float temp, bool stale);
void fish_ui_set_pin(fish_ui_t *ui, const char *pin);
void fish_ui_show_toast(fish_ui_t *ui, const char *msg);
void fish_ui_show_status(fish_ui_t *ui, const char *msg);
void fish_ui_update_wifi_status(fish_ui_t *ui);
/** Hide boot loading overlay once tank assets are on screen (idempotent). */
void fish_ui_set_content_ready(fish_ui_t *ui);

#ifdef __cplusplus
}
#endif
