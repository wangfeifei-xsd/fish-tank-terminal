#include "fish_img.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"

#define STBI_MALLOC(sz) heap_caps_malloc((sz), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define STBI_FREE(p) free(p)
#define STBI_REALLOC(p, sz) heap_caps_realloc((p), (sz), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "device_api.h"
#include "esp_cache.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "wifi_manager.h"

static const char *TAG = "fish_img";

static void *img_alloc(size_t size)
{
    void *p = heap_caps_aligned_alloc(64, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        p = heap_caps_aligned_alloc(64, size, MALLOC_CAP_8BIT);
    }
    if (!p) {
        p = malloc(size);
    }
    return p;
}

static void img_cache_msync(const void *buf, size_t len)
{
    if (!buf || len == 0) {
        return;
    }
    uintptr_t addr = (uintptr_t)buf;
    const size_t align = 64;
    uintptr_t start = addr & ~(align - 1);
    uintptr_t end = addr + len;
    size_t sync_len = (size_t)((end + align - 1) & ~(align - 1)) - start;
    esp_cache_msync((void *)start, sync_len, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
}

uint32_t fish_img_crc32(const char *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint32_t)data[i];
        for (int b = 0; b < 8; b++) {
            uint32_t mask = -(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

void fish_img_spiffs_to_lv(const char *spiffs_path, char *lv_path, size_t lv_len)
{
    const char *prefix = "/spiffs";
    const char *rel = spiffs_path;
    if (strncmp(spiffs_path, prefix, strlen(prefix)) == 0) {
        rel = spiffs_path + strlen(prefix);
        if (rel[0] == '/') {
            rel++;
        }
    }
    snprintf(lv_path, lv_len, "S:/%s", rel);
}

void fish_sprite_bin_paths(const char *icon_path, int target_w, char *bin_path, size_t bin_len, char *flip_path,
                           size_t flip_len)
{
    if (!icon_path || target_w <= 0) {
        if (bin_path && bin_len > 0) {
            bin_path[0] = '\0';
        }
        if (flip_path && flip_len > 0) {
            flip_path[0] = '\0';
        }
        return;
    }
    size_t pn = strlen(icon_path);
    const char *ext = strrchr(icon_path, '.');
    size_t base_len = pn;
    if (ext && ext > icon_path) {
        base_len = (size_t)(ext - icon_path);
    }
    if (bin_path && bin_len > 0) {
        snprintf(bin_path, bin_len, "%.*s_%dw.bin", (int)base_len, icon_path, target_w);
    }
    if (flip_path && flip_len > 0) {
        snprintf(flip_path, flip_len, "%.*s_%dwr.bin", (int)base_len, icon_path, target_w);
    }
}

static void remove_path(const char *path)
{
    if (path && path[0]) {
        unlink(path);
    }
}

static bool file_is_image(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    uint8_t hdr[8] = {0};
    size_t n = fread(hdr, 1, sizeof(hdr), f);
    fclose(f);
    if (n < 3) {
        return false;
    }
    if (hdr[0] == 0xFF && hdr[1] == 0xD8 && hdr[2] == 0xFF) {
        return true;
    }
    if (n >= 4 && hdr[0] == 0x89 && hdr[1] == 'P' && hdr[2] == 'N' && hdr[3] == 'G') {
        return true;
    }
    return false;
}

static bool bin_is_valid(const char *path, int expect_w, int expect_h)
{
    if (!path || expect_w <= 0 || expect_h <= 0) {
        return false;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    uint16_t hdr[3];
    if (fread(hdr, sizeof(hdr), 1, f) != 1) {
        fclose(f);
        return false;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return false;
    }
    long sz = ftell(f);
    fclose(f);
    if (hdr[0] != (uint16_t)expect_w || hdr[1] != (uint16_t)expect_h) {
        return false;
    }
    size_t need = 6 + (size_t)hdr[0] * (size_t)hdr[1] * 3;
    return sz >= (long)need;
}

esp_err_t fish_img_download_if_needed(const char *url, const char *dest_path)
{
    if (!url || !url[0] || !dest_path) {
        return ESP_ERR_INVALID_ARG;
    }
    struct stat st;
    if (stat(dest_path, &st) == 0 && st.st_size > 0) {
        if (file_is_image(dest_path)) {
            return ESP_OK;
        }
        ESP_LOGW(TAG, "invalid cached file, re-download %s", dest_path);
        remove_path(dest_path);
    }
    if (!fish_wifi_is_connected()) {
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t err = fish_api_download_url(url, dest_path);
    if (err != ESP_OK) {
        return err;
    }
    if (!file_is_image(dest_path)) {
        ESP_LOGW(TAG, "downloaded payload is not an image %s", dest_path);
        remove_path(dest_path);
        return ESP_FAIL;
    }
    if (stat(dest_path, &st) == 0) {
        ESP_LOGI(TAG, "saved %s (%ld bytes)", dest_path, (long)st.st_size);
    }
    return ESP_OK;
}

static esp_err_t decode_image_file(const char *spiffs_path, uint8_t **rgba, unsigned *w, unsigned *h)
{
    struct stat st;
    if (stat(spiffs_path, &st) != 0) {
        ESP_LOGW(TAG, "image missing %s", spiffs_path);
        return ESP_ERR_NOT_FOUND;
    }
    if (st.st_size < 16) {
        ESP_LOGW(TAG, "image too small %s (%ld bytes)", spiffs_path, (long)st.st_size);
        return ESP_FAIL;
    }
    int iw = 0;
    int ih = 0;
    int comp = 0;
    unsigned char *rgb = stbi_load(spiffs_path, &iw, &ih, &comp, 4);
    if (!rgb || iw <= 0 || ih <= 0) {
        if (rgb) {
            STBI_FREE(rgb);
        }
        ESP_LOGE(TAG, "stbi decode failed %s", spiffs_path);
        return ESP_FAIL;
    }
    *w = (unsigned)iw;
    *h = (unsigned)ih;
    *rgba = rgb;
    ESP_LOGI(TAG, "decoded %s %ux%u", spiffs_path, *w, *h);
    return ESP_OK;
}

static esp_err_t spiffs_check_space(size_t need_bytes)
{
    size_t total = 0;
    size_t used = 0;
    if (esp_spiffs_info(NULL, &total, &used) != ESP_OK) {
        return ESP_FAIL;
    }
    if (used + need_bytes > total) {
        ESP_LOGE(TAG, "SPIFFS full: need %u, free %u", (unsigned)need_bytes, (unsigned)(total - used));
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t fwrite_row_checked(FILE *f, const void *row, size_t row_bytes, const char *path)
{
    if (fwrite(row, 1, row_bytes, f) != row_bytes) {
        ESP_LOGE(TAG, "fwrite failed %s", path);
        fclose(f);
        unlink(path);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t write_rgba565a8_bin(const char *path, int w, int h, const uint8_t *rgba, bool mirror_x)
{
    size_t row_bytes = (size_t)w * 3;
    esp_err_t space_err = spiffs_check_space(6 + row_bytes * (size_t)h);
    if (space_err != ESP_OK) {
        return space_err;
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        return ESP_FAIL;
    }
    uint16_t hdr[3] = {(uint16_t)w, (uint16_t)h, (uint16_t)LV_IMG_CF_TRUE_COLOR_ALPHA};
    if (fwrite(hdr, sizeof(hdr), 1, f) != 1) {
        fclose(f);
        unlink(path);
        return ESP_FAIL;
    }
    uint8_t *row = img_alloc(row_bytes);
    if (!row) {
        fclose(f);
        unlink(path);
        return ESP_ERR_NO_MEM;
    }
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int sx = mirror_x ? (w - 1 - x) : x;
            const uint8_t *p = &rgba[(size_t)(y * w + sx) * 4];
            lv_color_t c = lv_color_make(p[0], p[1], p[2]);
            row[x * 3 + 0] = c.full & 0xFF;
            row[x * 3 + 1] = (c.full >> 8) & 0xFF;
            row[x * 3 + 2] = p[3];
        }
        esp_err_t err = fwrite_row_checked(f, row, row_bytes, path);
        if (err != ESP_OK) {
            free(row);
            return err;
        }
        if ((y & 63) == 0) {
            vTaskDelay(1);
        }
    }
    free(row);
    fclose(f);
    return ESP_OK;
}

static esp_err_t rgba_to_dsc_buf(uint8_t *dst, int tw, int th, const uint8_t *src, unsigned sw, unsigned sh,
                                 bool mirror_x)
{
    for (int y = 0; y < th; y++) {
        int sy = (int)((int64_t)y * (int64_t)sh / (int64_t)th);
        if (sy >= (int)sh) {
            sy = (int)sh - 1;
        }
        for (int x = 0; x < tw; x++) {
            int sx = (int)((int64_t)x * (int64_t)sw / (int64_t)tw);
            if (sx >= (int)sw) {
                sx = (int)sw - 1;
            }
            if (mirror_x) {
                sx = (int)sw - 1 - sx;
            }
            const uint8_t *p = &src[(size_t)(sy * sw + sx) * 4];
            lv_color_t c = lv_color_make(p[0], p[1], p[2]);
            dst[(size_t)(y * tw + x) * 3 + 0] = c.full & 0xFF;
            dst[(size_t)(y * tw + x) * 3 + 1] = (c.full >> 8) & 0xFF;
            dst[(size_t)(y * tw + x) * 3 + 2] = p[3];
        }
    }
    return ESP_OK;
}

static esp_err_t write_rgba565a8_bin_resampled(const char *path, int tw, int th, const uint8_t *src, unsigned sw,
                                               unsigned sh, bool mirror_x)
{
    size_t row_bytes = (size_t)tw * 3;
    esp_err_t space_err = spiffs_check_space(6 + row_bytes * (size_t)th);
    if (space_err != ESP_OK) {
        return space_err;
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "bin fopen failed %s", path);
        return ESP_FAIL;
    }
    uint16_t hdr[3] = {(uint16_t)tw, (uint16_t)th, (uint16_t)LV_IMG_CF_TRUE_COLOR_ALPHA};
    if (fwrite(hdr, sizeof(hdr), 1, f) != 1) {
        fclose(f);
        unlink(path);
        return ESP_FAIL;
    }
    uint8_t *row = img_alloc(row_bytes);
    if (!row) {
        fclose(f);
        unlink(path);
        return ESP_ERR_NO_MEM;
    }
    for (int y = 0; y < th; y++) {
        for (int x = 0; x < tw; x++) {
            int sy = (int)((int64_t)y * (int64_t)sh / (int64_t)th);
            if (sy >= (int)sh) {
                sy = (int)sh - 1;
            }
            int sx = (int)((int64_t)x * (int64_t)sw / (int64_t)tw);
            if (sx >= (int)sw) {
                sx = (int)sw - 1;
            }
            if (mirror_x) {
                sx = (int)sw - 1 - sx;
            }
            const uint8_t *p = &src[(size_t)(sy * sw + sx) * 4];
            lv_color_t c = lv_color_make(p[0], p[1], p[2]);
            row[x * 3 + 0] = c.full & 0xFF;
            row[x * 3 + 1] = (c.full >> 8) & 0xFF;
            row[x * 3 + 2] = p[3];
        }
        esp_err_t err = fwrite_row_checked(f, row, row_bytes, path);
        if (err != ESP_OK) {
            free(row);
            return err;
        }
        if ((y & 63) == 0) {
            vTaskDelay(1);
        }
    }
    free(row);
    fclose(f);
    return ESP_OK;
}

void fish_img_size_fit_width(unsigned src_w, unsigned src_h, int target_w, int *out_w, int *out_h)
{
    if (!out_w || !out_h || target_w <= 0) {
        return;
    }
    *out_w = target_w;
    if (src_w == 0 || src_h == 0) {
        *out_h = target_w;
        return;
    }
    *out_h = (int)((int64_t)target_w * (int64_t)src_h / (int64_t)src_w);
    if (*out_h < 1) {
        *out_h = 1;
    }
}

static esp_err_t load_sprite_resampled(const char *png_path, int tw, int th, lv_img_dsc_t *dsc, uint8_t **buf_out,
                                       bool mirror_x)
{
    uint8_t *rgba = NULL;
    unsigned sw = 0;
    unsigned sh = 0;
    esp_err_t err = decode_image_file(png_path, &rgba, &sw, &sh);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "sprite decode failed %s", png_path);
        return err;
    }
    size_t data_len = (size_t)tw * (size_t)th * 3;
    uint8_t *buf = img_alloc(data_len);
    if (!buf) {
        STBI_FREE(rgba);
        return ESP_ERR_NO_MEM;
    }
    rgba_to_dsc_buf(buf, tw, th, rgba, sw, sh, mirror_x);
    STBI_FREE(rgba);
    if (data_len > 0) {
        img_cache_msync(buf, data_len);
    }
    dsc->header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
    dsc->header.w = (uint16_t)tw;
    dsc->header.h = (uint16_t)th;
    dsc->data_size = data_len;
    dsc->data = buf;
    *buf_out = buf;
    ESP_LOGI(TAG, "sprite PSRAM %s %dx%d", png_path, tw, th);
    return ESP_OK;
}

esp_err_t fish_img_load_sprite(const char *png_path, const char *bin_path, int tw, int th, lv_img_dsc_t *dsc,
                               uint8_t **buf_out, bool mirror_x)
{
    if (!png_path || !dsc || !buf_out || tw <= 0 || th <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (bin_path && bin_path[0] && fish_img_load_bin(bin_path, dsc, buf_out) == ESP_OK) {
        return ESP_OK;
    }
    return load_sprite_resampled(png_path, tw, th, dsc, buf_out, mirror_x);
}

esp_err_t fish_img_load_sprite_fit_width(const char *png_path, const char *bin_path, int target_w, lv_img_dsc_t *dsc,
                                         uint8_t **buf_out, bool mirror_x)
{
    if (!png_path || !dsc || !buf_out || target_w <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (bin_path && bin_path[0] && fish_img_load_bin(bin_path, dsc, buf_out) == ESP_OK) {
        int cached_w = (int)dsc->header.w;
        if (abs(cached_w - target_w) <= 2) {
            return ESP_OK;
        }
        fish_img_free(dsc, buf_out);
    }
    uint8_t *rgba = NULL;
    unsigned sw = 0;
    unsigned sh = 0;
    esp_err_t err = decode_image_file(png_path, &rgba, &sw, &sh);
    if (err != ESP_OK) {
        return err;
    }
    int tw = 0;
    int th = 0;
    fish_img_size_fit_width(sw, sh, target_w, &tw, &th);
    size_t data_len = (size_t)tw * (size_t)th * 3;
    uint8_t *buf = img_alloc(data_len);
    if (!buf) {
        STBI_FREE(rgba);
        return ESP_ERR_NO_MEM;
    }
    rgba_to_dsc_buf(buf, tw, th, rgba, sw, sh, mirror_x);
    STBI_FREE(rgba);
    if (data_len > 0) {
        img_cache_msync(buf, data_len);
    }
    dsc->header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
    dsc->header.w = (uint16_t)tw;
    dsc->header.h = (uint16_t)th;
    dsc->data_size = data_len;
    dsc->data = buf;
    *buf_out = buf;
    ESP_LOGI(TAG, "sprite PSRAM %s %dx%d", png_path, tw, th);
    return ESP_OK;
}

esp_err_t fish_img_prepare_rgba565a8_fit_width(const char *src_png, const char *dst_bin, int target_w, bool mirror_x)
{
    if (!src_png || !dst_bin || target_w <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t *rgba = NULL;
    unsigned sw = 0;
    unsigned sh = 0;
    esp_err_t err = decode_image_file(src_png, &rgba, &sw, &sh);
    if (err != ESP_OK) {
        return err;
    }
    int tw = 0;
    int th = 0;
    fish_img_size_fit_width(sw, sh, target_w, &tw, &th);
    if (bin_is_valid(dst_bin, tw, th) && file_is_image(src_png)) {
        STBI_FREE(rgba);
        return ESP_OK;
    }
    remove_path(dst_bin);
    ESP_LOGI(TAG, "prepare %s -> %dx%d", src_png, tw, th);
    if ((unsigned)tw == sw && (unsigned)th == sh) {
        err = write_rgba565a8_bin(dst_bin, tw, th, rgba, mirror_x);
    } else {
        err = write_rgba565a8_bin_resampled(dst_bin, tw, th, rgba, sw, sh, mirror_x);
    }
    STBI_FREE(rgba);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "bin write failed %s", dst_bin);
        remove_path(dst_bin);
        return err;
    }
    ESP_LOGI(TAG, "prepared bin %s (%dx%d)", dst_bin, tw, th);
    return ESP_OK;
}

esp_err_t fish_img_decode_rgb565(const char *src_path, int tw, int th, lv_img_dsc_t *dsc, uint8_t **buf_out)
{
    if (!src_path || !dsc || !buf_out || tw <= 0 || th <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t *rgba = NULL;
    unsigned sw = 0;
    unsigned sh = 0;
    ESP_LOGI(TAG, "decode rgb565 %s -> %dx%d", src_path, tw, th);
    esp_err_t err = decode_image_file(src_path, &rgba, &sw, &sh);
    if (err != ESP_OK) {
        return err;
    }

    size_t data_len = (size_t)tw * (size_t)th * 2;
    uint8_t *buf = img_alloc(data_len);
    if (!buf) {
        STBI_FREE(rgba);
        return ESP_ERR_NO_MEM;
    }

    for (int y = 0; y < th; y++) {
        int sy = (int)((int64_t)y * (int64_t)sh / (int64_t)th);
        if (sy >= (int)sh) {
            sy = (int)sh - 1;
        }
        lv_color_t *row = (lv_color_t *)(buf + (size_t)y * (size_t)tw * 2);
        for (int x = 0; x < tw; x++) {
            int sx = (int)((int64_t)x * (int64_t)sw / (int64_t)tw);
            if (sx >= (int)sw) {
                sx = (int)sw - 1;
            }
            const uint8_t *p = &rgba[(size_t)(sy * sw + sx) * 4];
            row[x] = lv_color_make(p[0], p[1], p[2]);
        }
        if ((y & 63) == 0) {
            vTaskDelay(1);
        }
    }
    STBI_FREE(rgba);

    if (data_len > 0) {
        img_cache_msync(buf, data_len);
    }
    dsc->header.cf = LV_IMG_CF_TRUE_COLOR;
    dsc->header.w = (uint16_t)tw;
    dsc->header.h = (uint16_t)th;
    dsc->data_size = data_len;
    dsc->data = buf;
    *buf_out = buf;
    ESP_LOGI(TAG, "rgb565 ready %dx%d (%u bytes PSRAM)", tw, th, (unsigned)data_len);
    return ESP_OK;
}

esp_err_t fish_img_prepare_rgba565a8(const char *src_png, const char *dst_bin, int target_w, int target_h,
                                     bool mirror_x)
{
    if (!src_png || !dst_bin || target_w <= 0 || target_h <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (bin_is_valid(dst_bin, target_w, target_h) && file_is_image(src_png)) {
        return ESP_OK;
    }
    remove_path(dst_bin);
    uint8_t *rgba = NULL;
    unsigned sw = 0, sh = 0;
    ESP_LOGI(TAG, "prepare %s -> %dx%d", src_png, target_w, target_h);
    esp_err_t err = decode_image_file(src_png, &rgba, &sw, &sh);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "decode failed %s", src_png);
        remove_path(src_png);
        return err;
    }
    if ((unsigned)target_w == sw && (unsigned)target_h == sh) {
        err = write_rgba565a8_bin(dst_bin, target_w, target_h, rgba, mirror_x);
    } else {
        err = write_rgba565a8_bin_resampled(dst_bin, target_w, target_h, rgba, sw, sh, mirror_x);
    }
    STBI_FREE(rgba);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "bin write failed %s", dst_bin);
        remove_path(dst_bin);
        return err;
    }
    ESP_LOGI(TAG, "prepared bin %s (%dx%d)", dst_bin, target_w, target_h);
    return ESP_OK;
}

esp_err_t fish_img_load_bin(const char *bin_path, lv_img_dsc_t *dsc, uint8_t **buf_out)
{
    if (!bin_path || !dsc || !buf_out) {
        return ESP_ERR_INVALID_ARG;
    }
    FILE *f = fopen(bin_path, "rb");
    if (!f) {
        return ESP_FAIL;
    }
    uint16_t hdr[3];
    if (fread(hdr, sizeof(hdr), 1, f) != 1) {
        fclose(f);
        return ESP_FAIL;
    }
    int w = hdr[0];
    int h = hdr[1];
    size_t data_len = (size_t)w * (size_t)h * 3;
    uint8_t *buf = img_alloc(data_len);
    if (!buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    if (fread(buf, 1, data_len, f) != data_len) {
        free(buf);
        fclose(f);
        return ESP_FAIL;
    }
    fclose(f);
    if (data_len > 0) {
        img_cache_msync(buf, data_len);
    }
    dsc->header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
    dsc->header.w = (uint16_t)w;
    dsc->header.h = (uint16_t)h;
    dsc->data_size = data_len;
    dsc->data = buf;
    *buf_out = buf;
    return ESP_OK;
}

void fish_img_free(lv_img_dsc_t *dsc, uint8_t **buf)
{
    if (buf && *buf) {
        free(*buf);
        *buf = NULL;
    }
    if (dsc) {
        dsc->data = NULL;
        dsc->data_size = 0;
    }
}
