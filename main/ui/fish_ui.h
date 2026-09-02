#pragma once

#include "anim_engine.h"
#include "nvs_config.h"
#include "resource_cache.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*fish_ui_refresh_cb_t)(void *arg);

typedef struct {
    fish_config_t *cfg;
    fish_tank_state_t *tank;
    anim_engine_t *anim;
    lv_obj_t *screen;
    lv_obj_t *lbl_title;
    lv_obj_t *lbl_nickname;
    lv_obj_t *lbl_temp;
    lv_obj_t *btn_wifi;
    lv_obj_t *lbl_wifi;
    lv_obj_t *bar_satiety;
    lv_obj_t *lbl_satiety;
    lv_obj_t *provision_panel;
    lv_obj_t *lbl_pin;
    lv_obj_t *lbl_ble_pin;
    lv_obj_t *ble_panel;
    lv_obj_t *toast_bar;
    lv_obj_t *toast_lbl;
    lv_obj_t *loading_panel;
    lv_obj_t *loading_lbl;
    lv_obj_t *loading_bar;
    lv_obj_t *update_panel;
    lv_obj_t *update_lbl;
    lv_obj_t *update_bar;
    lv_timer_t *toast_timer;
    lv_timer_t *wifi_timer;
    fish_ui_refresh_cb_t refresh_cb;
    void *refresh_arg;
    bool content_ready;
    bool provisioning;
    bool wifi_connected_last;
    bool wifi_status_known;
    bool wifi_long_pressed;
} fish_ui_t;

fish_ui_t *fish_ui_create(fish_config_t *cfg, bool provisioning);
void fish_ui_destroy(fish_ui_t *ui);
void fish_ui_set_tank(fish_ui_t *ui, fish_tank_state_t *tank);
void fish_ui_set_refresh_handler(fish_ui_t *ui, fish_ui_refresh_cb_t cb, void *arg);
void fish_ui_request_refresh(fish_ui_t *ui);
void fish_ui_set_temp(fish_ui_t *ui, float temp, bool stale);
void fish_ui_set_pin(fish_ui_t *ui, const char *pin);
void fish_ui_show_ble_provision(fish_ui_t *ui);
void fish_ui_show_toast(fish_ui_t *ui, const char *msg);
void fish_ui_show_status(fish_ui_t *ui, const char *msg);
void fish_ui_update_wifi_status(fish_ui_t *ui);
/** Hide boot loading overlay once tank assets are on screen (idempotent). */
void fish_ui_set_content_ready(fish_ui_t *ui);
/** True while feed/sync worker holds or queues an HTTPS job. */
bool fish_ui_http_job_busy(void);
void fish_ui_show_update_loading(fish_ui_t *ui, const char *msg);
void fish_ui_hide_update_loading(fish_ui_t *ui);

#ifdef __cplusplus
}
#endif
