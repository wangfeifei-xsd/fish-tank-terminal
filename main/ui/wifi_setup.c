#include "wifi_setup.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "fish_ui_style.h"
#include "fonts/fish_font_24.h"
#include "fonts/fish_font_36.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>
#include "lvgl.h"
#include "nvs_config.h"
#include "sdkconfig.h"
#include "wifi_manager.h"

static const char *TAG = "wifi_setup";

#define WIFI_TOP_H      120
#define WIFI_KB_H       320
#define WIFI_LIST_PAD   16

typedef struct {
    fish_config_t *cfg;
    lv_obj_t *screen;
    lv_obj_t *list;
    lv_obj_t *status;
    lv_obj_t *dlg;
    lv_obj_t *ta;
    lv_obj_t *kb;
    char selected_ssid[33];
    bool done;
    bool success;
    bool pending_scan;
    bool scan_running;
    bool pending_connect;
    char pending_pass[65];
} wifi_setup_t;

static wifi_setup_t s_ws;
static wifi_ap_record_t s_ap_cache[FISH_WIFI_SCAN_MAX];

typedef struct {
    wifi_ap_record_t aps[FISH_WIFI_SCAN_MAX];
    uint16_t n;
    esp_err_t err;
} wifi_scan_result_t;

static void set_status_locked(const char *msg)
{
    if (s_ws.status) {
        lv_label_set_text(s_ws.status, msg);
    }
}

static void set_status(const char *msg)
{
    if (lvgl_port_lock(0)) {
        set_status_locked(msg);
        lvgl_port_unlock();
    }
}

static void layout_list_normal(void)
{
    if (!s_ws.list) {
        return;
    }
    lv_obj_clear_flag(s_ws.list, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(s_ws.list, CONFIG_FISH_LOGICAL_WIDTH - 32,
                    CONFIG_FISH_LOGICAL_HEIGHT - WIFI_TOP_H - WIFI_LIST_PAD);
    lv_obj_align(s_ws.list, LV_ALIGN_BOTTOM_MID, 0, -8);
}

static void layout_list_with_keyboard(void)
{
    if (!s_ws.list) {
        return;
    }
    lv_obj_add_flag(s_ws.list, LV_OBJ_FLAG_HIDDEN);
}

static void close_dialog(void)
{
    if (s_ws.kb) {
        lv_obj_del(s_ws.kb);
        s_ws.kb = NULL;
    }
    if (s_ws.dlg) {
        lv_obj_del(s_ws.dlg);
        s_ws.dlg = NULL;
        s_ws.ta = NULL;
    }
    layout_list_normal();
}

static void do_connect(const char *ssid, const char *pass)
{
    strncpy(s_ws.cfg->wifi_ssid, ssid, sizeof(s_ws.cfg->wifi_ssid) - 1);
    strncpy(s_ws.cfg->wifi_pass, pass ? pass : "", sizeof(s_ws.cfg->wifi_pass) - 1);
    s_ws.cfg->provisioned = true;
    strncpy(s_ws.pending_pass, pass ? pass : "", sizeof(s_ws.pending_pass) - 1);
    s_ws.pending_connect = true;
    set_status("正在连接…");
    close_dialog();
}

static void on_connect_btn(lv_event_t *e)
{
    (void)e;
    const char *pass = s_ws.ta ? lv_textarea_get_text(s_ws.ta) : "";
    do_connect(s_ws.selected_ssid, pass);
}

static void on_cancel_btn(lv_event_t *e)
{
    (void)e;
    close_dialog();
}

static void open_password_dialog(const char *ssid, bool open_network)
{
    memset(s_ws.selected_ssid, 0, sizeof(s_ws.selected_ssid));
    memcpy(s_ws.selected_ssid, ssid, sizeof(s_ws.selected_ssid) - 1);
    close_dialog();

    if (open_network) {
        do_connect(ssid, "");
        return;
    }

    layout_list_with_keyboard();

    s_ws.dlg = lv_obj_create(s_ws.screen);
    lv_obj_set_size(s_ws.dlg, 720, 260);
    lv_obj_align(s_ws.dlg, LV_ALIGN_CENTER, 0, -100);
    lv_obj_set_style_bg_color(s_ws.dlg, lv_color_hex(0x1e293b), 0);
    lv_obj_set_style_radius(s_ws.dlg, 12, 0);
    fish_ui_apply_default_font(s_ws.dlg);

    lv_obj_t *title = lv_label_create(s_ws.dlg);
    lv_label_set_text_fmt(title, "密码: %s", ssid);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 16, 12);

    s_ws.ta = lv_textarea_create(s_ws.dlg);
    lv_obj_set_size(s_ws.ta, 680, 64);
    lv_obj_align(s_ws.ta, LV_ALIGN_TOP_MID, 0, 56);
    lv_textarea_set_password_mode(s_ws.ta, true);
    lv_textarea_set_one_line(s_ws.ta, true);
    lv_textarea_set_placeholder_text(s_ws.ta, "输入 WiFi 密码");
    lv_textarea_set_max_length(s_ws.ta, 63);

    lv_obj_t *btn_ok = lv_btn_create(s_ws.dlg);
    lv_obj_set_size(btn_ok, 160, 64);
    lv_obj_align(btn_ok, LV_ALIGN_BOTTOM_RIGHT, -16, -16);
    lv_obj_t *lb_ok = lv_label_create(btn_ok);
    lv_label_set_text(lb_ok, "连接");
    lv_obj_center(lb_ok);
    lv_obj_add_event_cb(btn_ok, on_connect_btn, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_cancel = lv_btn_create(s_ws.dlg);
    lv_obj_set_size(btn_cancel, 160, 64);
    lv_obj_align(btn_cancel, LV_ALIGN_BOTTOM_LEFT, 16, -16);
    lv_obj_set_style_bg_color(btn_cancel, lv_color_hex(0x475569), 0);
    lv_obj_t *lb_c = lv_label_create(btn_cancel);
    lv_label_set_text(lb_c, "取消");
    lv_obj_center(lb_c);
    lv_obj_add_event_cb(btn_cancel, on_cancel_btn, LV_EVENT_CLICKED, NULL);

    s_ws.kb = lv_keyboard_create(s_ws.screen);
    lv_obj_set_size(s_ws.kb, CONFIG_FISH_LOGICAL_WIDTH, WIFI_KB_H);
    lv_obj_align(s_ws.kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_text_font(s_ws.kb, &fish_font_24, LV_PART_ITEMS);
    lv_keyboard_set_textarea(s_ws.kb, s_ws.ta);
}

static void on_ssid_clicked(lv_event_t *e)
{
    wifi_ap_record_t *ap = lv_event_get_user_data(e);
    if (!ap) {
        return;
    }
    bool open_net = (ap->authmode == WIFI_AUTH_OPEN);
    open_password_dialog((const char *)ap->ssid, open_net);
}

static void fill_list(wifi_ap_record_t *aps, uint16_t n)
{
    lv_obj_clean(s_ws.list);
    if (n == 0) {
        lv_list_add_text(s_ws.list, "未发现 WiFi，请点击刷新");
        return;
    }
    if (n > FISH_WIFI_SCAN_MAX) {
        n = FISH_WIFI_SCAN_MAX;
    }
    for (uint16_t i = 0; i < n; i++) {
        char line[80];
        const char *lock = (aps[i].authmode == WIFI_AUTH_OPEN) ? "开" : "锁";
        snprintf(line, sizeof(line), "[%s] %s  %ddBm", lock, aps[i].ssid, aps[i].rssi);
        lv_obj_t *btn = lv_list_add_btn(s_ws.list, NULL, line);
        lv_obj_set_style_text_color(btn, lv_color_white(), LV_PART_MAIN);
        s_ap_cache[i] = aps[i];
        lv_obj_add_event_cb(btn, on_ssid_clicked, LV_EVENT_CLICKED, &s_ap_cache[i]);
    }
}

static void scan_result_apply(void *user_data)
{
    wifi_scan_result_t *r = user_data;
    if (!r) {
        return;
    }
    if (r->err != ESP_OK) {
        set_status_locked("扫描失败，请点刷新重试");
        fill_list(NULL, 0);
    } else if (r->n == 0) {
        set_status_locked("未发现 WiFi，请点击刷新");
        fill_list(NULL, 0);
    } else {
        char buf[48];
        snprintf(buf, sizeof(buf), "找到 %u 个网络，点选连接", r->n);
        set_status_locked(buf);
        fill_list(r->aps, r->n);
    }
    s_ws.scan_running = false;
    free(r);
}

static void scan_task(void *arg)
{
    (void)arg;
    wifi_scan_result_t *r = calloc(1, sizeof(*r));
    if (!r) {
        vTaskDelete(NULL);
        return;
    }
    r->n = FISH_WIFI_SCAN_MAX;
    r->err = fish_wifi_scan(r->aps, &r->n);
    ESP_LOGI(TAG, "scan done err=%s count=%u", esp_err_to_name(r->err), r->n);
    lv_async_call(scan_result_apply, r);
    vTaskDelete(NULL);
}

static void do_scan(void)
{
    if (s_ws.scan_running) {
        return;
    }
    s_ws.scan_running = true;
    set_status("正在扫描 WiFi…");
    BaseType_t ok = xTaskCreate(scan_task, "wifi_scan", 6144, NULL, 4, NULL);
    if (ok != pdPASS) {
        s_ws.scan_running = false;
        set_status("扫描任务创建失败");
    }
}

static void on_refresh(lv_event_t *e)
{
    (void)e;
    s_ws.pending_scan = true;
    set_status("正在扫描 WiFi…");
}

static void on_skip(lv_event_t *e)
{
    (void)e;
    set_status("已跳过屏上配网（仍可用 BLE）");
    s_ws.done = true;
    s_ws.success = false;
}

static void run_pending_connect(void)
{
    if (!s_ws.pending_connect) {
        return;
    }
    s_ws.pending_connect = false;
    if (fish_wifi_connect(s_ws.cfg) != ESP_OK) {
        if (lvgl_port_lock(0)) {
            set_status("连接启动失败，请重试");
            lvgl_port_unlock();
        }
        s_ws.cfg->wifi_ssid[0] = '\0';
        s_ws.cfg->wifi_pass[0] = '\0';
        s_ws.cfg->provisioned = false;
        return;
    }
    fish_wifi_wait_connected(20000);
    if (fish_wifi_is_connected()) {
        fish_config_save(s_ws.cfg);
        if (lvgl_port_lock(0)) {
            set_status("连接成功，正在重启…");
            lvgl_port_unlock();
        }
        s_ws.success = true;
        s_ws.done = true;
        vTaskDelay(pdMS_TO_TICKS(800));
        esp_restart();
    } else {
        if (lvgl_port_lock(0)) {
            set_status("连接失败，请检查密码后重试");
            lvgl_port_unlock();
        }
        s_ws.cfg->wifi_ssid[0] = '\0';
        s_ws.cfg->wifi_pass[0] = '\0';
        s_ws.cfg->provisioned = false;
    }
}

esp_err_t fish_wifi_setup_run(fish_config_t *cfg)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(&s_ws, 0, sizeof(s_ws));
    s_ws.cfg = cfg;

    if (lvgl_port_lock(0)) {
        s_ws.screen = lv_obj_create(NULL);
        fish_ui_apply_default_font(s_ws.screen);
        lv_obj_set_style_bg_color(s_ws.screen, lv_color_hex(0x0f172a), 0);
        lv_scr_load(s_ws.screen);

        lv_obj_t *title = lv_label_create(s_ws.screen);
        lv_obj_set_style_text_color(title, lv_color_white(), 0);
        lv_obj_set_style_text_font(title, &fish_font_36, 0);
        lv_label_set_text(title, "WiFi 配网");
        lv_obj_align(title, LV_ALIGN_TOP_LEFT, 16, 16);

        s_ws.status = lv_label_create(s_ws.screen);
        lv_obj_set_style_text_color(s_ws.status, lv_color_hex(0x94a3b8), 0);
        lv_label_set_text(s_ws.status, "准备扫描…");
        lv_obj_align(s_ws.status, LV_ALIGN_TOP_LEFT, 16, 56);

        lv_obj_t *btn_refresh = lv_btn_create(s_ws.screen);
        lv_obj_set_size(btn_refresh, 120, 64);
        lv_obj_align(btn_refresh, LV_ALIGN_TOP_RIGHT, -16, 12);
        lv_obj_t *lb_r = lv_label_create(btn_refresh);
        lv_label_set_text(lb_r, "刷新");
        lv_obj_center(lb_r);
        lv_obj_add_event_cb(btn_refresh, on_refresh, LV_EVENT_CLICKED, NULL);

        lv_obj_t *btn_skip = lv_btn_create(s_ws.screen);
        lv_obj_set_size(btn_skip, 120, 64);
        lv_obj_align(btn_skip, LV_ALIGN_TOP_RIGHT, -152, 12);
        lv_obj_set_style_bg_color(btn_skip, lv_color_hex(0x475569), 0);
        lv_obj_t *lb_s = lv_label_create(btn_skip);
        lv_label_set_text(lb_s, "跳过");
        lv_obj_center(lb_s);
        lv_obj_add_event_cb(btn_skip, on_skip, LV_EVENT_CLICKED, NULL);

        s_ws.list = lv_list_create(s_ws.screen);
        lv_obj_set_style_bg_color(s_ws.list, lv_color_hex(0x1e293b), 0);
        lv_obj_set_style_text_color(s_ws.list, lv_color_white(), LV_PART_MAIN);
        fish_ui_apply_default_font(s_ws.list);
        layout_list_normal();

        lvgl_port_unlock();
    }

    s_ws.pending_scan = true;
    while (!s_ws.done) {
        if (s_ws.pending_scan) {
            s_ws.pending_scan = false;
            do_scan();
        }
        run_pending_connect();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    ESP_LOGI(TAG, "wifi setup finished success=%d", s_ws.success);
    return s_ws.success ? ESP_OK : ESP_FAIL;
}
