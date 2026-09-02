#include "fish_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "device_api.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
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
#include "ble/fish_ble.h"
#include "display_port.h"
#include "sntp_sync.h"
#include "wifi_manager.h"

#define FISH_BAR_W      380
#define FISH_BAR_H      18
#define FISH_BAR_BOTTOM 52

typedef enum {
    FISH_JOB_FEED = 0,
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
static volatile bool s_feed_inflight;
static volatile bool s_http_job_busy;

bool fish_ui_http_job_busy(void)
{
    return s_http_job_busy || s_feed_inflight;
}

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
    fish_ui_hide_update_loading(r->ui);
    if (r->has_interaction && r->ui->anim) {
        anim_engine_set_interaction(r->ui->anim, &r->interaction);
        if (r->ui->bar_satiety) {
            lv_bar_set_value(r->ui->bar_satiety, r->interaction.satiety, LV_ANIM_OFF);
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
        s_http_job_busy = true;
        fish_ui_t *ui = job.ui;
        fish_job_result_t *res = calloc(1, sizeof(*res));
        if (!res) {
            if (job.type == FISH_JOB_FEED) {
                s_feed_inflight = false;
            }
            s_http_job_busy = false;
            continue;
        }
        res->ui = ui;
        if (!ui || !ui->cfg || !ui->cfg->tank_id[0]) {
            if (ui && ui->cfg && fish_api_fetch_first_tank_id(ui->cfg->tank_id, sizeof(ui->cfg->tank_id)) == ESP_OK) {
                fish_config_save(ui->cfg);
            } else {
                strncpy(res->toast, "未绑定鱼缸", sizeof(res->toast) - 1);
                lv_async_call(job_result_apply, res);
                if (job.type == FISH_JOB_FEED) {
                    s_feed_inflight = false;
                }
                s_http_job_busy = false;
                continue;
            }
        }
        switch (job.type) {
        case FISH_JOB_FEED:
            if (!fish_wifi_is_connected()) {
                strncpy(res->toast, "未联网，稍后再试", sizeof(res->toast) - 1);
                break;
            }
            if (!fish_sntp_is_authoritative()) {
                strncpy(res->toast, "时间未同步，稍后再试", sizeof(res->toast) - 1);
                break;
            }
            /* SDIO RX asserts when DMA/internal is fragmented during TLS. */
            if (heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA) < 8192 ||
                heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) < 20480) {
                ESP_LOGW("fish_ui", "feed skipped: low internal heap (dma=%u int=%u)",
                         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
                         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
                strncpy(res->toast, "内存紧张，稍后再试", sizeof(res->toast) - 1);
                break;
            }
            if (fish_api_feed(ui->cfg->tank_id, &res->interaction) == ESP_OK) {
                res->has_interaction = true;
                strncpy(res->toast, "喂食成功", sizeof(res->toast) - 1);
            } else {
                strncpy(res->toast, "喂食失败", sizeof(res->toast) - 1);
            }
            /* Let hosted SDIO reclaim RX buffers before the next HTTPS. */
            vTaskDelay(pdMS_TO_TICKS(200));
            break;
#define FISH_CANVAS_H ANIM_VIEW_H

        case FISH_JOB_SYNC:
            if (ui->refresh_cb) {
                ui->refresh_cb(ui->refresh_arg);
                res->sync_ok = true;
                strncpy(res->toast, "鱼缸已更新", sizeof(res->toast) - 1);
            } else if (ui->tank && fish_cache_sync_tank(ui->cfg->tank_id, ui->tank) == ESP_OK) {
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
            vTaskDelay(pdMS_TO_TICKS(100));
            break;
        default:
            break;
        }
        lv_async_call(job_result_apply, res);
        if (job.type == FISH_JOB_FEED) {
            s_feed_inflight = false;
        }
        s_http_job_busy = false;
    }
}

static void ensure_worker(void)
{
    if (s_worker_started) {
        return;
    }
    s_job_q = xQueueCreate(2, sizeof(fish_job_t));
    if (!s_job_q) {
        return;
    }
    /* Keep stack modest — HTTPS buffers are heap/PSRAM; a 96KB stack starved SDIO DMA. */
    if (xTaskCreatePinnedToCore(api_worker_task, "fish_api", 10240, NULL, 4, NULL, 0) == pdPASS) {
        s_worker_started = true;
    } else {
        ESP_LOGE("fish_ui", "fish_api task create failed");
        vQueueDelete(s_job_q);
        s_job_q = NULL;
    }
}

static bool enqueue_job(fish_ui_t *ui, fish_job_type_t type, const char *region)
{
    if (!ui) {
        return false;
    }
    ensure_worker();
    if (!s_job_q) {
        return false;
    }
    if (type == FISH_JOB_FEED) {
        if (s_feed_inflight) {
            ESP_LOGI("fish_ui", "feed job skipped (inflight)");
            return false;
        }
    }
    fish_job_t job = {
        .type = type,
        .ui = ui,
    };
    if (region) {
        strncpy(job.region, region, sizeof(job.region) - 1);
    }
    if (xQueueSend(s_job_q, &job, 0) != pdTRUE) {
        ESP_LOGW("fish_ui", "job queue full, drop type=%d", (int)type);
        return false;
    }
    if (type == FISH_JOB_FEED) {
        s_feed_inflight = true;
    }
    return true;
}

static void loading_bar_anim_exec(void *bar, int32_t v)
{
    lv_bar_set_value(bar, v, LV_ANIM_OFF);
}

static void loading_bar_start(lv_obj_t *bar)
{
    if (!bar) {
        return;
    }
    lv_anim_del(bar, loading_bar_anim_exec);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 12, LV_ANIM_OFF);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, bar);
    lv_anim_set_values(&a, 12, 88);
    lv_anim_set_time(&a, 1000);
    lv_anim_set_playback_time(&a, 1000);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&a, loading_bar_anim_exec);
    lv_anim_start(&a);
}

static void loading_bar_stop(lv_obj_t *bar)
{
    if (!bar) {
        return;
    }
    lv_anim_del(bar, loading_bar_anim_exec);
}

static lv_obj_t *make_loading_bar(lv_obj_t *parent, lv_coord_t w)
{
    lv_obj_t *bar = lv_bar_create(parent);
    lv_obj_set_size(bar, w, 14);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x334155), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 7, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x38bdf8), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 7, LV_PART_INDICATOR);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 12, LV_ANIM_OFF);
    return bar;
}

static void btn_wifi_refresh(fish_ui_t *ui)
{
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
    /* Async job so the loading popup can paint; never block LVGL on HTTPS. */
    fish_ui_show_update_loading(ui, "页面更新加载中");
    if (!enqueue_job(ui, FISH_JOB_SYNC, NULL)) {
        fish_ui_hide_update_loading(ui);
        fish_ui_show_toast(ui, "繁忙，稍后再试");
    }
}

static void close_ble_panel(fish_ui_t *ui)
{
    if (ui && ui->ble_panel) {
        lv_obj_add_flag(ui->ble_panel, LV_OBJ_FLAG_HIDDEN);
    }
}

static void wifi_resume_anim(fish_ui_t *ui)
{
    if (ui && ui->anim) {
        anim_engine_set_paused(ui->anim, false);
    }
    if (ui) {
        fish_ui_update_wifi_status(ui);
    }
}

static void on_ble_close(lv_event_t *e)
{
    fish_ui_t *ui = lv_event_get_user_data(e);
    close_ble_panel(ui);
    wifi_resume_anim(ui);
}

void fish_ui_show_ble_provision(fish_ui_t *ui)
{
    if (!ui) {
        return;
    }
    fish_ble_refresh_pin();
    const char *pin = fish_ble_get_pin();
    if (pin && pin[0]) {
        fish_ui_set_pin(ui, pin);
    }
    if (ui->ble_panel) {
        if (ui->lbl_ble_pin) {
            char buf[32];
            snprintf(buf, sizeof(buf), "PIN: %s", pin ? pin : "------");
            lv_label_set_text(ui->lbl_ble_pin, buf);
        }
        lv_obj_clear_flag(ui->ble_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(ui->ble_panel);
        return;
    }

    ui->ble_panel = lv_obj_create(ui->screen);
    lv_obj_set_size(ui->ble_panel, CONFIG_FISH_LOGICAL_WIDTH - 48, 360);
    lv_obj_align(ui->ble_panel, LV_ALIGN_CENTER, 0, -40);
    lv_obj_set_style_bg_color(ui->ble_panel, lv_color_hex(0x1e293b), 0);
    lv_obj_set_style_border_color(ui->ble_panel, lv_color_hex(0x38bdf8), 0);
    lv_obj_set_style_border_width(ui->ble_panel, 2, 0);
    lv_obj_set_style_radius(ui->ble_panel, 12, 0);
    lv_obj_clear_flag(ui->ble_panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *btn_close = lv_btn_create(ui->ble_panel);
    lv_obj_set_size(btn_close, 44, 44);
    lv_obj_align(btn_close, LV_ALIGN_TOP_RIGHT, -10, 10);
    lv_obj_set_style_bg_opa(btn_close, LV_OPA_20, 0);
    lv_obj_set_style_bg_color(btn_close, lv_color_hex(0x64748b), 0);
    lv_obj_set_style_border_width(btn_close, 0, 0);
    lv_obj_set_style_radius(btn_close, 22, 0);
    lv_obj_set_style_shadow_width(btn_close, 0, 0);
    lv_obj_t *lb = lv_label_create(btn_close);
    lv_label_set_text(lb, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(lb, lv_color_hex(0xe2e8f0), 0);
    lv_obj_center(lb);
    lv_obj_add_event_cb(btn_close, on_ble_close, LV_EVENT_CLICKED, ui);

    lv_obj_t *body = lv_obj_create(ui->ble_panel);
    lv_obj_remove_style_all(body);
    lv_obj_set_pos(body, 28, 28);
    lv_obj_set_size(body, CONFIG_FISH_LOGICAL_WIDTH - 48 - 56, 360 - 56);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(body, 22, 0);

    lv_obj_t *title = lv_label_create(body);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, &fish_font_24, 0);
    lv_label_set_text(title, "小程序 BLE 配网");
    lv_obj_set_width(title, LV_PCT(100));
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *hint = lv_label_create(body);
    lv_obj_set_style_text_color(hint, lv_color_hex(0xcbd5e1), 0);
    lv_label_set_text(hint, "打开海水鱼检疫助手 - BLE配网\n输入 PIN 与 WiFi 后设备将重启");
    lv_obj_set_width(hint, LV_PCT(100));
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(hint, 12, 0);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);

    ui->lbl_ble_pin = lv_label_create(body);
    lv_obj_set_style_text_color(ui->lbl_ble_pin, lv_color_hex(0xf8fafc), 0);
    lv_obj_set_style_text_font(ui->lbl_ble_pin, &fish_font_36, 0);
    lv_obj_set_style_pad_top(ui->lbl_ble_pin, 10, 0);
    lv_obj_set_style_pad_bottom(ui->lbl_ble_pin, 6, 0);
    if (pin && pin[0]) {
        char buf[32];
        snprintf(buf, sizeof(buf), "PIN: %s", pin);
        lv_label_set_text(ui->lbl_ble_pin, buf);
    } else {
        lv_label_set_text(ui->lbl_ble_pin, "PIN: ------");
    }

    if (ui->cfg && ui->cfg->device_id[0]) {
        lv_obj_t *serial = lv_label_create(body);
        lv_obj_set_style_text_color(serial, lv_color_hex(0x94a3b8), 0);
        lv_label_set_text_fmt(serial, "设备: %s", ui->cfg->device_id);
        lv_obj_set_style_pad_bottom(serial, 4, 0);
    }

    lv_obj_move_foreground(btn_close);
    lv_obj_move_foreground(ui->ble_panel);
}

static void btn_wifi_long_cb(lv_event_t *e)
{
    fish_ui_t *ui = lv_event_get_user_data(e);
    if (!ui) {
        return;
    }
    ui->wifi_long_pressed = true;
    if (ui->anim) {
        anim_engine_set_paused(ui->anim, true);
    }
    fish_ui_show_ble_provision(ui);
}

static void btn_wifi_click_cb(lv_event_t *e)
{
    fish_ui_t *ui = lv_event_get_user_data(e);
    if (!ui) {
        return;
    }
    if (ui->wifi_long_pressed) {
        ui->wifi_long_pressed = false;
        return;
    }
    btn_wifi_refresh(ui);
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
    if (ui->anim) {
        lv_obj_t *anim_root = anim_engine_get_root(ui->anim);
        if (anim_root) {
            lv_obj_clear_flag(anim_root, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (ui->loading_panel) {
        loading_bar_stop(ui->loading_bar);
        lv_obj_add_flag(ui->loading_panel, LV_OBJ_FLAG_HIDDEN);
    }
}

static void update_tank_header_locked(fish_ui_t *ui, fish_tank_state_t *tank)
{
    if (!ui || !tank || !ui->lbl_title) {
        return;
    }
    const char *name = tank->tank.name[0] ? tank->tank.name
                                          : (ui->cfg && ui->cfg->device_name[0] ? ui->cfg->device_name : "桌面鱼缸");
    char buf[256];
    int pos = snprintf(buf, sizeof(buf), "%s", name);
    const fish_tank_info_t *t = &tank->tank;

    if (t->length_cm > 0 && t->width_cm > 0 && t->height_cm > 0) {
        pos += snprintf(buf + pos, sizeof(buf) - pos, " - %dx%dx%dcm", t->length_cm, t->width_cm, t->height_cm);
    } else if (t->length_cm > 0) {
        pos += snprintf(buf + pos, sizeof(buf) - pos, " - %dcm缸", t->length_cm);
    }
    if (t->run_days > 0) {
        pos += snprintf(buf + pos, sizeof(buf) - pos, " - 运行%d天", t->run_days);
    }
    if (tank->fish_count > 0) {
        pos += snprintf(buf + pos, sizeof(buf) - pos, " - %d条鱼", tank->fish_count);
    }
    if (t->total_value > 0) {
        pos += snprintf(buf + pos, sizeof(buf) - pos, "(%d元)", t->total_value);
    }
    lv_label_set_text(ui->lbl_title, buf);

    if (ui->lbl_nickname) {
        if (tank->tank.owner_nickname[0]) {
            lv_label_set_text(ui->lbl_nickname, tank->tank.owner_nickname);
            lv_obj_clear_flag(ui->lbl_nickname, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_label_set_text(ui->lbl_nickname, "");
            lv_obj_add_flag(ui->lbl_nickname, LV_OBJ_FLAG_HIDDEN);
        }
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
        if (s_feed_inflight) {
            return;
        }
        /* Match anim API cooldown — avoid stacking HTTPS while SDIO is busy. */
        if (s_last_feed_job_ms > 0 && (now - s_last_feed_job_ms) < 10000) {
            return;
        }
        s_last_feed_job_ms = now;
        enqueue_job(ui, FISH_JOB_FEED, NULL);
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
    lv_obj_set_width(ui->lbl_title, CONFIG_FISH_LOGICAL_WIDTH - 130);
    lv_label_set_long_mode(ui->lbl_title, LV_LABEL_LONG_DOT);
    lv_obj_align(ui->lbl_title, LV_ALIGN_TOP_LEFT, 16, 10);
    lv_label_set_text(ui->lbl_title, cfg->device_name[0] ? cfg->device_name : "桌面鱼缸");

    ui->lbl_nickname = lv_label_create(ui->screen);
    lv_obj_set_style_text_color(ui->lbl_nickname, lv_color_hex(0xe2e8f0), 0);
    lv_obj_align(ui->lbl_nickname, LV_ALIGN_TOP_RIGHT, -56, 12);
    lv_label_set_text(ui->lbl_nickname, "");
    lv_obj_add_flag(ui->lbl_nickname, LV_OBJ_FLAG_HIDDEN);

    ui->btn_wifi = lv_btn_create(ui->screen);
    lv_obj_set_size(ui->btn_wifi, 40, 40);
    lv_obj_align(ui->btn_wifi, LV_ALIGN_TOP_RIGHT, -10, 8);
    ui->lbl_wifi = lv_label_create(ui->btn_wifi);
    lv_label_set_text(ui->lbl_wifi, LV_SYMBOL_WIFI);
    lv_obj_center(ui->lbl_wifi);
    lv_obj_add_event_cb(ui->btn_wifi, btn_wifi_click_cb, LV_EVENT_CLICKED, ui);
    lv_obj_add_event_cb(ui->btn_wifi, btn_wifi_long_cb, LV_EVENT_LONG_PRESSED, ui);
    fish_ui_update_wifi_status(ui);

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
        lv_label_set_text(t1, "请用小程序 BLE 配网(长按右上角 WiFi 图标)");
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
        /* Hide tank layer until assets ready — prevents sky/water flash under overlay. */
        if (ui->anim) {
            lv_obj_t *anim_root = anim_engine_get_root(ui->anim);
            if (anim_root) {
                lv_obj_add_flag(anim_root, LV_OBJ_FLAG_HIDDEN);
            }
        }
        ui->loading_panel = lv_obj_create(ui->screen);
        lv_obj_set_size(ui->loading_panel, CONFIG_FISH_LOGICAL_WIDTH, CONFIG_FISH_LOGICAL_HEIGHT);
        lv_obj_align(ui->loading_panel, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_set_style_bg_color(ui->loading_panel, lv_color_hex(0x0f172a), 0);
        lv_obj_set_style_bg_opa(ui->loading_panel, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(ui->loading_panel, 0, 0);
        lv_obj_set_style_radius(ui->loading_panel, 0, 0);
        lv_obj_clear_flag(ui->loading_panel, LV_OBJ_FLAG_SCROLLABLE);

        ui->loading_lbl = lv_label_create(ui->loading_panel);
        lv_obj_set_style_text_color(ui->loading_lbl, lv_color_hex(0xe2e8f0), 0);
        lv_obj_set_style_text_font(ui->loading_lbl, &fish_font_36, 0);
        lv_label_set_text(ui->loading_lbl, "加载中");
        lv_obj_align(ui->loading_lbl, LV_ALIGN_CENTER, 0, -28);

        ui->loading_bar = make_loading_bar(ui->loading_panel, 360);
        lv_obj_align(ui->loading_bar, LV_ALIGN_CENTER, 0, 28);
        loading_bar_start(ui->loading_bar);
        lv_obj_move_foreground(ui->loading_panel);
    }

    /* Defer fish_api worker until first job — saves stack during boot_cache/TLS sync. */
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
    loading_bar_stop(ui->loading_bar);
    loading_bar_stop(ui->update_bar);
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
    if (lvgl_port_lock(0)) {
        update_tank_header_locked(ui, tank);
        lvgl_port_unlock();
    }
    if (ui->anim) {
        anim_engine_set_tank(ui->anim, tank);
    }
    if (ui->bar_satiety) {
        lv_bar_set_value(ui->bar_satiety, tank->interaction.satiety, LV_ANIM_OFF);
    }
}

void fish_ui_set_refresh_handler(fish_ui_t *ui, fish_ui_refresh_cb_t cb, void *arg)
{
    if (!ui) {
        return;
    }
    ui->refresh_cb = cb;
    ui->refresh_arg = arg;
}

void fish_ui_request_refresh(fish_ui_t *ui)
{
    btn_wifi_refresh(ui);
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
    if (!ui || !pin) {
        return;
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "PIN: %s", pin);
    if (ui->lbl_pin) {
        lv_label_set_text(ui->lbl_pin, buf);
    }
    if (ui->lbl_ble_pin) {
        lv_label_set_text(ui->lbl_ble_pin, buf);
    }
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

void fish_ui_show_update_loading(fish_ui_t *ui, const char *msg)
{
    if (!ui || !ui->screen) {
        return;
    }
    if (!ui->update_panel) {
        ui->update_panel = lv_obj_create(ui->screen);
        lv_obj_set_size(ui->update_panel, CONFIG_FISH_LOGICAL_WIDTH, CONFIG_FISH_LOGICAL_HEIGHT);
        lv_obj_align(ui->update_panel, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_set_style_bg_color(ui->update_panel, lv_color_hex(0x020617), 0);
        lv_obj_set_style_bg_opa(ui->update_panel, LV_OPA_70, 0);
        lv_obj_set_style_border_width(ui->update_panel, 0, 0);
        lv_obj_set_style_radius(ui->update_panel, 0, 0);
        lv_obj_clear_flag(ui->update_panel, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(ui->update_panel, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *card = lv_obj_create(ui->update_panel);
        lv_obj_set_size(card, 520, 180);
        lv_obj_center(card);
        lv_obj_set_style_bg_color(card, lv_color_hex(0x1e293b), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(card, lv_color_hex(0x38bdf8), 0);
        lv_obj_set_style_border_width(card, 2, 0);
        lv_obj_set_style_radius(card, 16, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

        ui->update_lbl = lv_label_create(card);
        lv_obj_set_style_text_color(ui->update_lbl, lv_color_hex(0xf8fafc), 0);
        lv_obj_set_style_text_font(ui->update_lbl, &fish_font_36, 0);
        lv_obj_align(ui->update_lbl, LV_ALIGN_TOP_MID, 0, 36);

        ui->update_bar = make_loading_bar(card, 360);
        lv_obj_align(ui->update_bar, LV_ALIGN_BOTTOM_MID, 0, -36);
    }
    if (ui->update_lbl) {
        lv_label_set_text(ui->update_lbl, msg && msg[0] ? msg : "页面更新加载中");
    }
    loading_bar_start(ui->update_bar);
    lv_obj_clear_flag(ui->update_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(ui->update_panel);
}

void fish_ui_hide_update_loading(fish_ui_t *ui)
{
    if (!ui || !ui->update_panel) {
        return;
    }
    loading_bar_stop(ui->update_bar);
    lv_obj_add_flag(ui->update_panel, LV_OBJ_FLAG_HIDDEN);
}

void fish_ui_show_status(fish_ui_t *ui, const char *msg)
{
    fish_ui_show_toast(ui, msg);
}
