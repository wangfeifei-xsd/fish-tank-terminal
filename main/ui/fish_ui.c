#include "fish_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "device_api.h"
#include "esp_err.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "fish_ui_style.h"
#include "fonts/fish_font_24.h"
#include "fonts/fish_font_36.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "nvs_config.h"
#include "resource_cache.h"
#include "sdkconfig.h"
#include "anim_engine.h"
#include "display_port.h"
#include "wifi_manager.h"
#include "wifi_setup.h"

#define FISH_BAR_W      380
#define FISH_BAR_H      18
#define FISH_BAR_BOTTOM 52

typedef enum {
    FISH_JOB_FEED = 0,
    FISH_JOB_CLEAN,
    FISH_JOB_WATER,
    FISH_JOB_SYNC,
} fish_job_type_t;

typedef struct {
    fish_job_type_t type;
    char region[16];
    fish_ui_t *ui;
} fish_job_t;

typedef struct {
    fish_ui_t *ui;
    fish_interaction_t interaction;
    bool has_interaction;
    bool sync_ok;
    char toast[64];
} fish_job_result_t;

static QueueHandle_t s_job_q;
static bool s_worker_started;
static int64_t s_last_feed_job_ms;

static void job_result_apply(void *user_data)
{
    fish_job_result_t *r = user_data;
    if (!r || !r->ui) {
        free(r);
        return;
    }
    if (r->sync_ok && r->ui->tank) {
        fish_ui_set_tank(r->ui, r->ui->tank);
    }
    if (r->has_interaction && r->ui->anim) {
        anim_engine_set_interaction(r->ui->anim, &r->interaction);
        if (r->ui->bar_satiety) {
            lv_bar_set_value(r->ui->bar_satiety, r->interaction.satiety, LV_ANIM_OFF);
        }
        if (r->ui->bar_water) {
            lv_bar_set_value(r->ui->bar_water, r->interaction.water_quality, LV_ANIM_OFF);
        }
        if (r->ui->tank) {
            r->ui->tank->interaction = r->interaction;
        }
    }
    if (r->toast[0]) {
        fish_ui_show_toast(r->ui, r->toast);
    }
    free(r);
}

static void api_worker_task(void *arg)
{
    (void)arg;
    fish_job_t job;
    while (1) {
        if (xQueueReceive(s_job_q, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        fish_ui_t *ui = job.ui;
        fish_job_result_t *res = calloc(1, sizeof(*res));
        if (!res) {
            continue;
        }
        res->ui = ui;
        if (!ui || !ui->cfg || !ui->cfg->tank_id[0]) {
            if (ui && ui->cfg && fish_api_fetch_first_tank_id(ui->cfg->tank_id, sizeof(ui->cfg->tank_id)) == ESP_OK) {
                fish_config_save(ui->cfg);
            } else {
                strncpy(res->toast, "未绑定鱼缸", sizeof(res->toast) - 1);
                lv_async_call(job_result_apply, res);
                continue;
            }
        }
        switch (job.type) {
        case FISH_JOB_FEED:
            if (fish_api_feed(ui->cfg->tank_id, &res->interaction) == ESP_OK) {
                res->has_interaction = true;
                strncpy(res->toast, "喂食成功", sizeof(res->toast) - 1);
            } else {
                strncpy(res->toast, "喂食失败", sizeof(res->toast) - 1);
            }
            break;
        case FISH_JOB_CLEAN:
            if (fish_api_clean(ui->cfg->tank_id, job.region[0] ? job.region : "left", &res->interaction) == ESP_OK) {
                res->has_interaction = true;
                strncpy(res->toast, "刮藻完成", sizeof(res->toast) - 1);
            } else {
                strncpy(res->toast, "刮藻失败", sizeof(res->toast) - 1);
            }
            break;
        case FISH_JOB_WATER:
            if (fish_api_water(ui->cfg->tank_id, "virtual", &res->interaction) == ESP_OK) {
                res->has_interaction = true;
                strncpy(res->toast, "换水完成", sizeof(res->toast) - 1);
            } else {
                strncpy(res->toast, "换水失败", sizeof(res->toast) - 1);
            }
            break;
#define FISH_CANVAS_H ANIM_VIEW_H

        case FISH_JOB_SYNC:
            if (ui->tank && fish_cache_sync_tank(ui->cfg->tank_id, ui->tank) == ESP_OK) {
                UBaseType_t prio = uxTaskPriorityGet(NULL);
                vTaskPrioritySet(NULL, 3);
                fish_cache_prepare_assets(ui->tank, CONFIG_FISH_LOGICAL_WIDTH, FISH_CANVAS_H);
                if (ui->anim) {
                    anim_engine_prepare_assets(ui->anim, ui->tank);
                }
                vTaskPrioritySet(NULL, prio);
                res->sync_ok = true;
                strncpy(res->toast, "鱼缸已更新", sizeof(res->toast) - 1);
            } else {
                strncpy(res->toast, "网络错误", sizeof(res->toast) - 1);
            }
            break;
        default:
            break;
        }
        lv_async_call(job_result_apply, res);
    }
}

static void ensure_worker(void)
{
    if (s_worker_started) {
        return;
    }
    s_job_q = xQueueCreate(8, sizeof(fish_job_t));
    if (!s_job_q) {
        return;
    }
    /* HTTPS/mbedtls needs headroom; interaction buffers are heap-backed. */
    if (xTaskCreate(api_worker_task, "fish_api", 24576, NULL, 4, NULL) == pdPASS) {
        s_worker_started = true;
    }
}

static void enqueue_job(fish_ui_t *ui, fish_job_type_t type, const char *region)
{
    if (!ui) {
        return;
    }
    ensure_worker();
    if (!s_job_q) {
        return;
    }
    fish_job_t job = {
        .type = type,
        .ui = ui,
    };
    if (region) {
        strncpy(job.region, region, sizeof(job.region) - 1);
    }
    xQueueSend(s_job_q, &job, 0);
}

static void btn_refresh_cb(lv_event_t *e)
{
    fish_ui_t *ui = lv_event_get_user_data(e);
    if (!ui || !ui->cfg) {
        return;
    }
    if (!ui->cfg->tank_id[0]) {
        if (fish_api_fetch_first_tank_id(ui->cfg->tank_id, sizeof(ui->cfg->tank_id)) != ESP_OK) {
            fish_ui_show_toast(ui, "未绑定鱼缸");
            return;
        }
        fish_config_save(ui->cfg);
    }
    fish_ui_show_toast(ui, "同步中…");
    enqueue_job(ui, FISH_JOB_SYNC, NULL);
}

static void wifi_resume_cb(void *arg)
{
    fish_ui_t *ui = arg;
    if (ui && ui->anim) {
        anim_engine_set_paused(ui->anim, false);
    }
    if (ui) {
        fish_ui_update_wifi_status(ui);
    }
}

static void btn_wifi_cb(lv_event_t *e)
{
    fish_ui_t *ui = lv_event_get_user_data(e);
    if (!ui || !ui->cfg || ui->provisioning) {
        return;
    }
    if (ui->anim) {
        anim_engine_set_paused(ui->anim, true);
    }
    fish_wifi_setup_open_from_ui(ui->cfg, ui->screen, wifi_resume_cb, ui);
}

static void wifi_status_timer_cb(lv_timer_t *t)
{
    fish_ui_t *ui = t->user_data;
    fish_ui_update_wifi_status(ui);
}

void fish_ui_set_content_ready(fish_ui_t *ui)
{
    if (!ui || ui->content_ready) {
        return;
    }
    ui->content_ready = true;
    if (ui->loading_panel) {
        lv_obj_add_flag(ui->loading_panel, LV_OBJ_FLAG_HIDDEN);
    }
}

void fish_ui_update_wifi_status(fish_ui_t *ui)
{
    if (!ui || !ui->lbl_wifi) {
        return;
    }
    bool connected = fish_wifi_is_connected();
    if (ui->wifi_status_known && connected == ui->wifi_connected_last) {
        return;
    }
    ui->wifi_connected_last = connected;
    ui->wifi_status_known = true;
    lv_obj_set_style_text_color(ui->lbl_wifi, connected ? lv_color_hex(0x4ade80) : lv_color_hex(0xf87171), 0);
}

static void interaction_cb(const char *action, const char *region, void *user)
{
    fish_ui_t *ui = user;
    if (!ui || !ui->cfg || !ui->tank) {
        return;
    }
    if (strcmp(action, "feed") == 0) {
        int64_t now = esp_timer_get_time() / 1000;
        if (s_last_feed_job_ms > 0 && (now - s_last_feed_job_ms) < 1500) {
            return;
        }
        s_last_feed_job_ms = now;
        enqueue_job(ui, FISH_JOB_FEED, NULL);
    } else if (strcmp(action, "clean") == 0) {
        enqueue_job(ui, FISH_JOB_CLEAN, region);
    } else if (strcmp(action, "water") == 0) {
        enqueue_job(ui, FISH_JOB_WATER, NULL);
    }
}

static void toast_hide_cb(lv_timer_t *t)
{
    fish_ui_t *ui = t->user_data;
    if (ui && ui->toast_bar) {
        lv_obj_add_flag(ui->toast_bar, LV_OBJ_FLAG_HIDDEN);
    }
    ui->toast_timer = NULL;
    lv_timer_del(t);
}

static void on_display_tank_tap(int screen_x, int screen_y, void *user)
{
    fish_ui_t *ui = user;
    if (!ui || !ui->anim) {
        return;
    }
    anim_engine_screen_tap(ui->anim, screen_x, screen_y);
}

fish_ui_t *fish_ui_create(fish_config_t *cfg, bool provisioning)
{
    fish_ui_t *ui = calloc(1, sizeof(*ui));
    if (!ui) {
        return NULL;
    }
    ui->cfg = cfg;
    ui->provisioning = provisioning;
    ui->screen = lv_obj_create(NULL);
    lv_scr_load(ui->screen);
    fish_ui_apply_default_font(ui->screen);
    lv_obj_set_style_bg_color(ui->screen, lv_color_hex(0x0f172a), 0);

    ui->lbl_title = lv_label_create(ui->screen);
    lv_obj_set_style_text_color(ui->lbl_title, lv_color_white(), 0);
    lv_obj_set_style_text_font(ui->lbl_title, &fish_font_24, 0);
    lv_obj_align(ui->lbl_title, LV_ALIGN_TOP_LEFT, 16, 14);
    lv_label_set_text(ui->lbl_title, cfg->device_name[0] ? cfg->device_name : "桌面鱼缸");

    ui->btn_wifi = lv_btn_create(ui->screen);
    lv_obj_set_size(ui->btn_wifi, 40, 40);
    lv_obj_align(ui->btn_wifi, LV_ALIGN_TOP_RIGHT, -56, 8);
    ui->lbl_wifi = lv_label_create(ui->btn_wifi);
    lv_label_set_text(ui->lbl_wifi, LV_SYMBOL_WIFI);
    lv_obj_center(ui->lbl_wifi);
    lv_obj_add_event_cb(ui->btn_wifi, btn_wifi_cb, LV_EVENT_CLICKED, ui);
    fish_ui_update_wifi_status(ui);

    lv_obj_t *btn_refresh = lv_btn_create(ui->screen);
    lv_obj_set_size(btn_refresh, 40, 40);
    lv_obj_align(btn_refresh, LV_ALIGN_TOP_RIGHT, -10, 8);
    lv_obj_t *lbl_r = lv_label_create(btn_refresh);
    lv_label_set_text(lbl_r, LV_SYMBOL_REFRESH);
    lv_obj_center(lbl_r);
    lv_obj_add_event_cb(btn_refresh, btn_refresh_cb, LV_EVENT_CLICKED, ui);

    if (provisioning) {
        ui->provision_panel = lv_obj_create(ui->screen);
        lv_obj_set_size(ui->provision_panel, CONFIG_FISH_LOGICAL_WIDTH - 32, 110);
        lv_obj_align(ui->provision_panel, LV_ALIGN_TOP_MID, 0, 84);
        lv_obj_set_style_bg_color(ui->provision_panel, lv_color_hex(0x1e293b), 0);
        lv_obj_set_style_border_color(ui->provision_panel, lv_color_hex(0x38bdf8), 0);
        lv_obj_set_style_border_width(ui->provision_panel, 2, 0);
        lv_obj_set_style_radius(ui->provision_panel, 12, 0);
        lv_obj_t *t1 = lv_label_create(ui->provision_panel);
        lv_obj_set_style_text_color(t1, lv_color_hex(0xcbd5e1), 0);
        lv_label_set_text(t1, "可用屏上配网或小程序 BLE");
        lv_obj_align(t1, LV_ALIGN_TOP_MID, 0, 10);
        ui->lbl_pin = lv_label_create(ui->provision_panel);
        lv_obj_set_style_text_color(ui->lbl_pin, lv_color_hex(0xf8fafc), 0);
        lv_obj_set_style_text_font(ui->lbl_pin, &fish_font_36, 0);
        lv_obj_align(ui->lbl_pin, LV_ALIGN_CENTER, 0, 16);
        lv_label_set_text(ui->lbl_pin, "PIN: ------");
    }

    ui->anim = anim_engine_create(ui->screen);
    anim_engine_set_interaction_cb(ui->anim, interaction_cb, ui);
    fish_display_set_tank_tap_handler(on_display_tank_tap, ui);

    ui->bar_satiety = lv_bar_create(ui->screen);
    lv_obj_set_size(ui->bar_satiety, FISH_BAR_W, FISH_BAR_H);
    lv_obj_align(ui->bar_satiety, LV_ALIGN_BOTTOM_LEFT, 16, -FISH_BAR_BOTTOM);
    lv_bar_set_range(ui->bar_satiety, 0, 10);
    ui->lbl_satiety = lv_label_create(ui->screen);
    lv_label_set_text(ui->lbl_satiety, "饱食");
    lv_obj_align_to(ui->lbl_satiety, ui->bar_satiety, LV_ALIGN_OUT_TOP_LEFT, 0, -6);

    ui->bar_water = lv_bar_create(ui->screen);
    lv_obj_set_size(ui->bar_water, FISH_BAR_W, FISH_BAR_H);
    lv_obj_align(ui->bar_water, LV_ALIGN_BOTTOM_LEFT, 16 + FISH_BAR_W + 40, -FISH_BAR_BOTTOM);
    lv_bar_set_range(ui->bar_water, 0, 10);
    ui->lbl_water = lv_label_create(ui->screen);
    lv_label_set_text(ui->lbl_water, "水质");
    lv_obj_align_to(ui->lbl_water, ui->bar_water, LV_ALIGN_OUT_TOP_LEFT, 0, -6);

    ui->toast_bar = lv_obj_create(ui->screen);
    lv_obj_set_size(ui->toast_bar, 420, 52);
    lv_obj_align(ui->toast_bar, LV_ALIGN_TOP_MID, 0, 88);
    lv_obj_set_style_bg_color(ui->toast_bar, lv_color_hex(0x1e293b), 0);
    lv_obj_set_style_bg_opa(ui->toast_bar, LV_OPA_90, 0);
    lv_obj_set_style_radius(ui->toast_bar, 10, 0);
    lv_obj_set_style_border_color(ui->toast_bar, lv_color_hex(0x38bdf8), 0);
    lv_obj_set_style_border_width(ui->toast_bar, 1, 0);
    lv_obj_add_flag(ui->toast_bar, LV_OBJ_FLAG_HIDDEN);
    ui->toast_lbl = lv_label_create(ui->toast_bar);
    lv_obj_set_style_text_color(ui->toast_lbl, lv_color_white(), 0);
    lv_obj_center(ui->toast_lbl);
    lv_label_set_text(ui->toast_lbl, "");

    if (ui->anim) {
        lv_obj_t *anim_root = anim_engine_get_root(ui->anim);
        if (anim_root) {
            lv_obj_move_foreground(anim_root);
        }
    }

    if (provisioning) {
        ui->content_ready = true;
        anim_engine_start(ui->anim);
    } else {
        ui->loading_panel = lv_obj_create(ui->screen);
        lv_obj_set_size(ui->loading_panel, CONFIG_FISH_LOGICAL_WIDTH, CONFIG_FISH_LOGICAL_HEIGHT);
        lv_obj_align(ui->loading_panel, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_set_style_bg_color(ui->loading_panel, lv_color_hex(0x0f172a), 0);
        lv_obj_set_style_bg_opa(ui->loading_panel, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(ui->loading_panel, 0, 0);
        lv_obj_set_style_radius(ui->loading_panel, 0, 0);
        lv_obj_clear_flag(ui->loading_panel, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *loading_lbl = lv_label_create(ui->loading_panel);
        lv_obj_set_style_text_color(loading_lbl, lv_color_hex(0x94a3b8), 0);
        lv_obj_set_style_text_font(loading_lbl, &fish_font_24, 0);
        lv_label_set_text(loading_lbl, "加载中");
        lv_obj_center(loading_lbl);
        lv_obj_move_foreground(ui->loading_panel);
    }

    ensure_worker();
    if (!provisioning) {
        ui->wifi_timer = lv_timer_create(wifi_status_timer_cb, 3000, ui);
    }
    return ui;
}

void fish_ui_destroy(fish_ui_t *ui)
{
    if (!ui) {
        return;
    }
    fish_display_set_tank_tap_handler(NULL, NULL);
    if (ui->toast_timer) {
        lv_timer_del(ui->toast_timer);
        ui->toast_timer = NULL;
    }
    if (ui->wifi_timer) {
        lv_timer_del(ui->wifi_timer);
        ui->wifi_timer = NULL;
    }
    if (ui->anim) {
        anim_engine_stop(ui->anim);
        anim_engine_destroy(ui->anim);
    }
    free(ui);
}

void fish_ui_set_tank(fish_ui_t *ui, fish_tank_state_t *tank)
{
    if (!ui || !tank) {
        return;
    }
    ui->tank = tank;
    if (ui->lbl_title) {
        lv_label_set_text(ui->lbl_title, tank->tank.name[0] ? tank->tank.name : ui->cfg->device_name);
    }
    if (ui->anim) {
        anim_engine_set_tank(ui->anim, tank);
    }
    if (ui->bar_satiety) {
        lv_bar_set_value(ui->bar_satiety, tank->interaction.satiety, LV_ANIM_OFF);
    }
    if (ui->bar_water) {
        lv_bar_set_value(ui->bar_water, tank->interaction.water_quality, LV_ANIM_OFF);
    }
}

void fish_ui_set_temp(fish_ui_t *ui, float temp, bool stale)
{
    if (!ui || !ui->lbl_temp) {
        return;
    }
    char buf[24];
    snprintf(buf, sizeof(buf), stale ? "%.1f°C!" : "%.1f°C", temp);
    lv_label_set_text(ui->lbl_temp, buf);
    lv_obj_set_style_text_color(ui->lbl_temp, stale ? lv_color_hex(0x94a3b8) : lv_color_white(), 0);
}

void fish_ui_set_pin(fish_ui_t *ui, const char *pin)
{
    if (!ui || !ui->lbl_pin || !pin) {
        return;
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "PIN: %s", pin);
    lv_label_set_text(ui->lbl_pin, buf);
}

void fish_ui_show_toast(fish_ui_t *ui, const char *msg)
{
    if (!ui || !msg || !ui->toast_bar || !ui->toast_lbl) {
        return;
    }
    if (ui->toast_timer) {
        lv_timer_del(ui->toast_timer);
        ui->toast_timer = NULL;
    }
    lv_label_set_text(ui->toast_lbl, msg);
    lv_obj_clear_flag(ui->toast_bar, LV_OBJ_FLAG_HIDDEN);
    ui->toast_timer = lv_timer_create(toast_hide_cb, 1500, ui);
    lv_timer_set_repeat_count(ui->toast_timer, 1);
}

void fish_ui_show_status(fish_ui_t *ui, const char *msg)
{
    fish_ui_show_toast(ui, msg);
}
