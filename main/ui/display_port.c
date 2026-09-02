#include "display_port.h"

#include <stdlib.h>

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_lvgl_port_disp.h"
#include "fonts/fish_font_24.h"
#include "fonts/fish_font_36.h"
#include "lcd.h"
#include "sdkconfig.h"
#include "touch.h"

static const char *TAG = "fish_disp";
static lv_disp_t *s_disp;
static fish_tank_tap_fn s_tank_tap_fn;
static void *s_tank_tap_user;
static bool s_touch_was_down;

static void boot_bar_anim_exec(void *bar, int32_t v)
{
    lv_bar_set_value(bar, v, LV_ANIM_OFF);
}

typedef struct {
    fish_tank_tap_fn fn;
    void *user;
    int x;
    int y;
} fish_tap_async_t;

static void fish_tap_async_cb(void *user_data)
{
    fish_tap_async_t *req = user_data;
    if (req && req->fn) {
        req->fn(req->x, req->y, req->user);
    }
    free(req);
}

void fish_touch_transform(int raw_x, int raw_y, int *out_x, int *out_y)
{
    if (lcddev.width > lcddev.height) {
        /* Native landscape panel (e.g. 7" 1024x600). */
        if (out_x) {
            *out_x = raw_x;
        }
        if (out_y) {
            *out_y = raw_y;
        }
        return;
    }
    /* Portrait touch panel -> landscape LVGL (matches PPA ROTATION_ANGLE_270). */
    if (out_x) {
        *out_x = raw_y;
    }
    if (out_y) {
        *out_y = lcddev.width - 1 - raw_x;
    }
}

void fish_display_set_tank_tap_handler(fish_tank_tap_fn fn, void *user)
{
    s_tank_tap_fn = fn;
    s_tank_tap_user = user;
}

static void touchpad_read(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    (void)drv;
    static lv_coord_t last_x;
    static lv_coord_t last_y;

    tp_dev.scan(0);
    if (tp_dev.sta & TP_PRES_DOWN) {
        int lx, ly;
        fish_touch_transform(tp_dev.x[0], tp_dev.y[0], &lx, &ly);
        last_x = (lv_coord_t)lx;
        last_y = (lv_coord_t)ly;
        data->point.x = last_x;
        data->point.y = last_y;
        data->state = LV_INDEV_STATE_PRESSED;
        if (!s_touch_was_down) {
            ESP_LOGD(TAG, "touch raw=(%d,%d) mapped=(%d,%d)", tp_dev.x[0], tp_dev.y[0], lx, ly);
            if (s_tank_tap_fn) {
                /* Defer: do not invalidate UI from inside taskLVGL indev read. */
                fish_tap_async_t *req = malloc(sizeof(*req));
                if (req) {
                    req->fn = s_tank_tap_fn;
                    req->user = s_tank_tap_user;
                    req->x = lx;
                    req->y = ly;
                    lv_async_call(fish_tap_async_cb, req);
                }
            }
        }
        s_touch_was_down = true;
    } else {
        data->point.x = last_x;
        data->point.y = last_y;
        data->state = LV_INDEV_STATE_RELEASED;
        s_touch_was_down = false;
    }
}

static lv_indev_t *s_touch_indev;
static lv_obj_t *s_boot_scr;

lv_indev_t *fish_display_get_touch(void)
{
    return s_touch_indev;
}

void fish_display_init(void)
{
    lcd_init();

    if (CONFIG_FISH_LOGICAL_WIDTH != (int)lcddev.width ||
        CONFIG_FISH_LOGICAL_HEIGHT != (int)lcddev.height) {
        ESP_LOGW(TAG,
                 "UI canvas %dx%d != panel %lux%lu — set FISH_LOGICAL_* in menuconfig to match screen",
                 CONFIG_FISH_LOGICAL_WIDTH, CONFIG_FISH_LOGICAL_HEIGHT,
                 (unsigned long)lcddev.width, (unsigned long)lcddev.height);
    }

    lvgl_port_cfg_t lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_port_cfg.task_priority = 7;
    lvgl_port_cfg.task_max_sleep_ms = 8;
    lvgl_port_cfg.task_stack = 32768;
    lvgl_port_cfg.task_affinity = 1;
    lvgl_port_init(&lvgl_port_cfg);

    const bool native_landscape = lcddev.width > lcddev.height;
    const uint32_t lv_hres = native_landscape ? lcddev.width : lcddev.height;
    const uint32_t lv_vres = native_landscape ? lcddev.height : lcddev.width;

    if (lcddev.id <= 0x7084) {
        const lvgl_port_display_cfg_t rgb_disp_cfg = {
            .panel_handle = lcddev.lcd_panel_handle,
            .buffer_size = lv_hres * 40,
            .double_buffer = true,
            .hres = lv_hres,
            .vres = lv_vres,
            .monochrome = false,
            .rotation = {.swap_xy = false, .mirror_x = false, .mirror_y = false},
            .flags = {
                .buff_dma = false,
                .buff_spiram = true,
                .full_refresh = false,
                .direct_mode = false,
                .sw_rotate = false,
            },
        };
        const lvgl_port_display_rgb_cfg_t rgb_cfg = {.flags = {.bb_mode = false, .avoid_tearing = false}};
        s_disp = lvgl_port_add_disp_rgb(&rgb_disp_cfg, &rgb_cfg);
    } else {
        const lvgl_port_display_cfg_t disp_cfg = {
            .io_handle = lcddev.lcd_dbi_io,
            .panel_handle = lcddev.lcd_panel_handle,
            .control_handle = NULL,
            .buffer_size = lv_hres * lv_vres,
            .double_buffer = true,
            .hres = lv_hres,
            .vres = lv_vres,
            .monochrome = false,
            .rotation = {.swap_xy = false, .mirror_x = false, .mirror_y = false},
            .flags = {
                .buff_dma = false,
                .buff_spiram = true,
                .sw_rotate = false,
                /* 横屏 EK79007 须 avoid_tearing：LVGL 直接写入 DPI 硬件 FB；
                 * SPIRAM 绘图缓冲无法被 MIPI DPI 扫描，会出现“日志正常但全黑”。 */
                .full_refresh = false,
                .direct_mode = true,
            },
        };
        const lvgl_port_display_dsi_cfg_t dpi_cfg = {
            .flags = {
                .avoid_tearing = native_landscape,
                .native_landscape = native_landscape,
            },
        };
        s_disp = lvgl_port_add_disp_dsi(&disp_cfg, &dpi_cfg);
    }

    tp_dev.init();
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = touchpad_read;
    indev_drv.disp = s_disp;
    s_touch_indev = lv_indev_drv_register(&indev_drv);

    if (lvgl_port_lock(0)) {
        lv_theme_t *th = lv_theme_default_init(s_disp, lv_palette_main(LV_PALETTE_BLUE),
                                               lv_palette_main(LV_PALETTE_CYAN), true, &fish_font_24);
        lv_disp_set_theme(s_disp, th);
        s_boot_scr = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(s_boot_scr, lv_color_hex(0x0f172a), 0);
        lv_obj_set_style_bg_opa(s_boot_scr, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s_boot_scr, 0, 0);
        lv_obj_t *boot_lbl = lv_label_create(s_boot_scr);
        lv_obj_set_style_text_color(boot_lbl, lv_color_hex(0xe2e8f0), 0);
        lv_obj_set_style_text_font(boot_lbl, &fish_font_36, 0);
        lv_label_set_text(boot_lbl, "加载中");
        lv_obj_align(boot_lbl, LV_ALIGN_CENTER, 0, -28);

        lv_obj_t *boot_bar = lv_bar_create(s_boot_scr);
        lv_obj_set_size(boot_bar, 360, 14);
        lv_obj_align(boot_bar, LV_ALIGN_CENTER, 0, 28);
        lv_obj_set_style_bg_color(boot_bar, lv_color_hex(0x334155), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(boot_bar, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(boot_bar, 7, LV_PART_MAIN);
        lv_obj_set_style_bg_color(boot_bar, lv_color_hex(0x38bdf8), LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(boot_bar, LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_set_style_radius(boot_bar, 7, LV_PART_INDICATOR);
        lv_bar_set_range(boot_bar, 0, 100);
        lv_bar_set_value(boot_bar, 12, LV_ANIM_OFF);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, boot_bar);
        lv_anim_set_values(&a, 12, 88);
        lv_anim_set_time(&a, 1000);
        lv_anim_set_playback_time(&a, 1000);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
        lv_anim_set_exec_cb(&a, boot_bar_anim_exec);
        lv_anim_start(&a);

        lv_scr_load(s_boot_scr);
        lvgl_port_unlock();
    }
}

lv_disp_t *fish_display_get(void)
{
    return s_disp;
}
