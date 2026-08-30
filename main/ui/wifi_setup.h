#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "lvgl.h"
#include "nvs_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool allow_skip;          /**< Show "跳过" (first-time provisioning) */
    bool restart_on_success;  /**< Reboot after successful connect */
    const char *title;
} fish_wifi_setup_opts_t;

/**
 * Show on-screen WiFi setup (scan list + soft keyboard).
 * Blocks until credentials are saved and connection succeeds, then returns ESP_OK.
 * On cancel/fail returns an error; caller may keep BLE provisioning available.
 */
esp_err_t fish_wifi_setup_run(fish_config_t *cfg);

/**
 * Blocking WiFi picker. When @p return_screen is set, reloads it on cancel/failure.
 */
esp_err_t fish_wifi_setup_run_opts(fish_config_t *cfg, const fish_wifi_setup_opts_t *opts,
                                   lv_obj_t *return_screen);

/** Open WiFi picker from main UI (runs in background task). */
void fish_wifi_setup_open_from_ui(fish_config_t *cfg, lv_obj_t *return_screen,
                                  void (*on_resume)(void *arg), void *resume_arg);

#ifdef __cplusplus
}
#endif
