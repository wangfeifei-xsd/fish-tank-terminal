#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Max cached fish sprite width; actual size follows on-screen fish size (up to this cap). */
#define FISH_SPRITE_WIDTH 96
#define FISH_SPRITE_MIN_WIDTH 16

static inline int fish_sprite_target_px(float size_px)
{
    int w = (int)(size_px + 0.5f);
    if (w < FISH_SPRITE_MIN_WIDTH) {
        w = FISH_SPRITE_MIN_WIDTH;
    }
    if (w > FISH_SPRITE_WIDTH) {
        w = FISH_SPRITE_WIDTH;
    }
    return w;
}

uint32_t fish_img_crc32(const char *data, size_t len);

void fish_img_spiffs_to_lv(const char *spiffs_path, char *lv_path, size_t lv_len);

esp_err_t fish_img_download_if_needed(const char *url, const char *dest_path);

esp_err_t fish_img_prepare_rgba565a8(const char *src_png, const char *dst_bin, int target_w, int target_h,
                                     bool mirror_x);

esp_err_t fish_img_decode_rgb565(const char *src_path, int tw, int th, lv_img_dsc_t *dsc, uint8_t **buf_out);

esp_err_t fish_img_load_bin(const char *bin_path, lv_img_dsc_t *dsc, uint8_t **buf_out);

esp_err_t fish_img_load_sprite(const char *png_path, const char *bin_path, int tw, int th, lv_img_dsc_t *dsc,
                               uint8_t **buf_out, bool mirror_x);

/** Scale to target_w pixels wide; height preserves aspect ratio (requires PNG if no matching bin). */
void fish_img_size_fit_width(unsigned src_w, unsigned src_h, int target_w, int *out_w, int *out_h);

esp_err_t fish_img_load_sprite_fit_width(const char *png_path, const char *bin_path, int target_w,
                                         lv_img_dsc_t *dsc, uint8_t **buf_out, bool mirror_x);

esp_err_t fish_img_prepare_rgba565a8_fit_width(const char *src_png, const char *dst_bin, int target_w,
                                               bool mirror_x);

void fish_img_free(lv_img_dsc_t *dsc, uint8_t **buf);

#ifdef __cplusplus
}
#endif
