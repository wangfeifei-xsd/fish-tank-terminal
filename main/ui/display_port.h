#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

void fish_display_init(void);
lv_disp_t *fish_display_get(void);
lv_indev_t *fish_display_get_touch(void);
void fish_touch_transform(int raw_x, int raw_y, int *out_x, int *out_y);

/** Bypass LVGL hit-test when the UI thread is busy (screen coordinates). */
typedef void (*fish_tank_tap_fn)(int screen_x, int screen_y, void *user);
void fish_display_set_tank_tap_handler(fish_tank_tap_fn fn, void *user);

#ifdef __cplusplus
}
#endif
