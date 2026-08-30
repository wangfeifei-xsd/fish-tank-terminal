#include "anim_engine.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "fish_img.h"
#include "fonts/fish_font_24.h"
#include "lvgl.h"
#include "resource_cache.h"

static const char *TAG = "anim_engine";
#define FISH_DECO_BIN_MAX 256
#define FEED_SPAWN_DROP 12.0f

static const float DT = 1.0f / 60.0f;
static const uint32_t ANIM_TIMER_MS = 16; /* 60Hz 同步，仿真步长仍固定 dt=1/60 */

static float randf(void)
{
    return (float)rand() / (float)RAND_MAX;
}

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void free_deco_widgets(anim_engine_t *eng)
{
    if (!eng) {
        return;
    }
    for (int i = 0; i < FISH_MAX_DECO; i++) {
        if (eng->deco_img[i]) {
            lv_obj_del(eng->deco_img[i]);
            eng->deco_img[i] = NULL;
        }
    }
}

static void free_decorations(anim_engine_t *eng)
{
    free_deco_widgets(eng);
    for (int i = 0; i < eng->deco_count; i++) {
        fish_img_free(&eng->decos[i].dsc, &eng->decos[i].buf);
    }
    eng->deco_count = 0;
}

static void sync_deco_widgets(anim_engine_t *eng);

static void rebuild_decorations(anim_engine_t *eng)
{
    free_decorations(eng);
    if (!eng->tank) {
        return;
    }
    float tank_len = eng->tank->tank.length_cm > 0 ? (float)eng->tank->tank.length_cm : 100.0f;
    float px_per_cm = (float)ANIM_W / tank_len;
    int n = eng->tank->deco_count;
    if (n > FISH_MAX_DECO) {
        n = FISH_MAX_DECO;
    }
    eng->deco_count = n;
    for (int i = 0; i < n; i++) {
        anim_deco_t *ad = &eng->decos[i];
        const fish_deco_item_t *src = &eng->tank->decos[i];
        ad->x = src->x;
        ad->y = src->y;
        ad->loaded = false;
        int tw = (int)(src->default_w_cm * src->scale * px_per_cm);
        int th = (int)(src->default_h_cm * src->scale * px_per_cm);
        if (tw < 4) {
            tw = 4;
        }
        if (th < 4) {
            th = 4;
        }
        if (tw > FISH_DECO_BIN_MAX) {
            tw = FISH_DECO_BIN_MAX;
        }
        if (th > FISH_DECO_BIN_MAX) {
            th = FISH_DECO_BIN_MAX;
        }
        ad->w = tw;
        ad->h = th;
        const char *bin = fish_cache_deco_path(eng->tank, i);
        if (bin && fish_img_load_bin(bin, &ad->dsc, &ad->buf) == ESP_OK) {
            ad->loaded = true;
        }
    }
    sync_deco_widgets(eng);
}

static void sync_deco_widgets(anim_engine_t *eng)
{
    if (!eng || !eng->root) {
        return;
    }
    free_deco_widgets(eng);
    float W = (float)ANIM_W;
    float H = (float)eng->view_h;
    for (int i = 0; i < eng->deco_count; i++) {
        anim_deco_t *ad = &eng->decos[i];
        if (!ad->loaded || !ad->dsc.data) {
            continue;
        }
        eng->deco_img[i] = lv_img_create(eng->root);
        lv_img_set_src(eng->deco_img[i], &ad->dsc);
        lv_obj_clear_flag(eng->deco_img[i], LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        float cx = ad->x * W;
        float cy = ad->y * H;
        lv_obj_set_pos(eng->deco_img[i], (lv_coord_t)(cx - ad->w / 2), (lv_coord_t)(cy - ad->h / 2));
    }
    if (eng->canvas) {
        lv_obj_move_foreground(eng->canvas);
    }
}

static void destroy_fish_widget(anim_fish_t *f)
{
    if (f && f->img) {
        lv_obj_del(f->img);
        f->img = NULL;
    }
}

static void teardown_fish_widgets(anim_engine_t *eng)
{
    if (!eng) {
        return;
    }
    for (int i = 0; i < eng->fish_count; i++) {
        destroy_fish_widget(&eng->fishes[i]);
        fish_img_free(&eng->fishes[i].sprite_dsc, &eng->fishes[i].sprite_buf);
        fish_img_free(&eng->fishes[i].sprite_flip_dsc, &eng->fishes[i].sprite_flip_buf);
    }
}

static void create_fish_widget(anim_engine_t *eng, anim_fish_t *f)
{
    if (!eng || !f || !eng->root || !f->has_sprite || !f->sprite_dsc.data) {
        return;
    }
    destroy_fish_widget(f);
    f->cached_src = NULL;
    f->cached_x = (lv_coord_t)INT16_MIN;
    f->cached_y = (lv_coord_t)INT16_MIN;
    f->img = lv_img_create(eng->root);
    lv_obj_clear_flag(f->img, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_img_set_src(f->img, &f->sprite_dsc);
    lv_img_set_zoom(f->img, 256);
    lv_img_set_angle(f->img, 0);
    if (eng->canvas) {
        lv_obj_move_foreground(eng->canvas);
    }
}

static void sync_fish_widget(anim_engine_t *eng, anim_fish_t *f)
{
    if (!eng || !f || !f->img || !f->has_sprite) {
        return;
    }
    int draw_facing = f->facing;
    if (f->turning) {
        float p = f->turn_t / f->turn_duration;
        if (p > 1.0f) {
            p = 1.0f;
        }
        float cos_p = cosf(p * 3.14159265f);
        draw_facing = (f->facing * cos_p) >= 0.0f ? f->facing : -f->facing;
    }
    const lv_img_dsc_t *dsc = &f->sprite_dsc;
    if (draw_facing < 0 && f->sprite_flip_dsc.data) {
        dsc = &f->sprite_flip_dsc;
    }
    if (dsc != f->cached_src) {
        lv_img_set_src(f->img, dsc);
        f->cached_src = dsc;
    }
    int iw = (int)dsc->header.w;
    int ih = (int)dsc->header.h;
    if (iw <= 0 || ih <= 0) {
        return;
    }
    float display_w = f->size;
    if (display_w < 8.0f) {
        display_w = 8.0f;
    }
    int target_w = (int)(display_w + 0.5f);
    if (abs(iw - target_w) > 2) {
        uint16_t zoom = (uint16_t)(display_w * 256.0f / (float)iw);
        if (zoom < 32) {
            zoom = 32;
        }
        lv_img_set_zoom(f->img, zoom);
    } else {
        lv_img_set_zoom(f->img, 256);
    }
    uint16_t zoom = lv_img_get_zoom(f->img);
    float zw = (float)iw * (float)zoom / 256.0f;
    float zh = (float)ih * (float)zoom / 256.0f;
    lv_coord_t nx = (lv_coord_t)(f->x - zw * 0.5f);
    lv_coord_t ny = (lv_coord_t)(f->y - zh * 0.5f);
    if (nx != f->cached_x || ny != f->cached_y) {
        lv_obj_set_pos(f->img, nx, ny);
        f->cached_x = nx;
        f->cached_y = ny;
    }
}

static void sync_fish_widgets(anim_engine_t *eng)
{
    if (!eng) {
        return;
    }
    for (int i = 0; i < eng->fish_count; i++) {
        sync_fish_widget(eng, &eng->fishes[i]);
    }
}

static void free_sprite_preload(anim_engine_t *eng)
{
    if (!eng) {
        return;
    }
    for (int i = 0; i < eng->sprite_preload_count; i++) {
        anim_sprite_preload_t *p = &eng->sprite_preload[i];
        fish_img_free(&p->dsc, &p->buf);
        fish_img_free(&p->flip_dsc, &p->flip_buf);
        p->ready = false;
    }
    eng->sprite_preload_count = 0;
}

static void preload_fish_sprites(anim_engine_t *eng, fish_tank_state_t *tank)
{
    if (!eng || !tank) {
        return;
    }
    free_sprite_preload(eng);
    int n = tank->fish_count;
    if (n > ANIM_MAX_FISH) {
        n = ANIM_MAX_FISH;
    }
    eng->sprite_preload_count = n;
    float tank_len = tank->tank.length_cm > 0 ? (float)tank->tank.length_cm : 100.0f;
    float px_per_cm = (float)ANIM_W / tank_len;
    for (int i = 0; i < n; i++) {
        anim_sprite_preload_t *p = &eng->sprite_preload[i];
        memset(p, 0, sizeof(*p));
        const fish_item_t *src = &tank->fish[i];
        if (!src->icon_path[0] && !src->icon_bin_path[0]) {
            continue;
        }
        struct stat st;
        bool have_png = src->icon_path[0] && stat(src->icon_path, &st) == 0 && st.st_size > 0;
        bool have_bin = src->icon_bin_path[0] && stat(src->icon_bin_path, &st) == 0 && st.st_size > 0;
        if (!have_png && !have_bin) {
            continue;
        }
        int tw = fish_sprite_target_px(src->size_cm * px_per_cm);
        const char *png = src->icon_path[0] ? src->icon_path : src->icon_bin_path;
        if (fish_img_load_sprite_fit_width(png, src->icon_bin_path, tw, &p->dsc, &p->buf, false) == ESP_OK) {
            p->ready = true;
        } else {
            ESP_LOGW(TAG, "preload sprite failed %s", png);
        }
        fish_img_load_sprite_fit_width(png, src->icon_path_flip, tw, &p->flip_dsc, &p->flip_buf, true);
    }
    int ready = 0;
    for (int i = 0; i < n; i++) {
        if (eng->sprite_preload[i].ready) {
            ready++;
        }
    }
    ESP_LOGI(TAG, "preloaded %d/%d fish sprites", ready, n);
}

static void rebuild_fishes(anim_engine_t *eng)
{
    teardown_fish_widgets(eng);
    if (!eng->tank) {
        eng->fish_count = 0;
        return;
    }
    int n = eng->tank->fish_count;
    if (n > ANIM_MAX_FISH) {
        n = ANIM_MAX_FISH;
    }
    eng->fish_count = n;
    int lanes = n < 6 ? n : 6;
    float W = (float)ANIM_W;
    float H = (float)eng->view_h;
    float tank_len = eng->tank->tank.length_cm > 0 ? (float)eng->tank->tank.length_cm : 100.0f;
    eng->px_per_cm = W / tank_len;
    float layer_top = H * 0.12f;
    float layer_bottom = H * 0.85f;
    float layer_h = layer_bottom - layer_top;
    float lane_step = layer_h * 0.6f / (lanes > 0 ? lanes : 1);

    for (int i = 0; i < n; i++) {
        anim_fish_t *f = &eng->fishes[i];
        memset(f, 0, sizeof(*f));
        const fish_item_t *src = &eng->tank->fish[i];
        strncpy(f->name, src->name, sizeof(f->name) - 1);
        f->dir = randf() > 0.5f ? 1 : -1;
        f->facing = -f->dir;
        float speed_mul = 1.0f, amp_mul = 1.0f;
        f->pause_prob = 0.0015f;
        if (strcmp(src->temperament, "calm") == 0) {
            speed_mul = 0.55f;
            amp_mul = 0.5f;
            f->pause_prob = 0.004f;
        } else if (strcmp(src->temperament, "active") == 0) {
            speed_mul = 1.3f;
            amp_mul = 1.4f;
            f->pause_prob = 0.0006f;
        }
        f->vx = (14.0f + randf() * 20.0f) * speed_mul;
        f->amp = (3.0f + randf() * 5.0f) * amp_mul;
        f->freq = 0.5f + randf() * 0.6f;
        f->phase = randf() * 6.283f;
        f->size = src->size_cm * eng->px_per_cm;
        if (f->size < 4) {
            f->size = 4;
        }
        if (f->size > FISH_SPRITE_WIDTH) {
            f->size = FISH_SPRITE_WIDTH;
        }
        float rt = layer_top, rb = layer_bottom;
        if (strcmp(src->swim_layer, "middle_top") == 0) {
            rb = layer_top + layer_h * 0.7f;
        } else if (strcmp(src->swim_layer, "bottom") == 0) {
            rt = layer_top + layer_h * 0.7f;
        }
        f->range_top = rt;
        f->range_bottom = rb;
        int lane = i % lanes;
        f->base_y = rt + (rb - rt) * ((float)lane / (float)(lanes > 1 ? lanes - 1 : 1)) + randf() * lane_step * 0.3f;
        f->target_base_y = f->base_y;
        float margin = f->size / 2 + 4;
        f->x = margin + randf() * (W - margin * 2);
        f->y = f->base_y;
        f->turn_duration = 0.22f + randf() * 0.12f;
        f->turn_sign = randf() > 0.5f ? 1 : -1;
        f->amp_mul = 1.0f;
        {
            static const uint32_t palette[] = {0xf97316, 0x38bdf8, 0xfacc15, 0xfb7185, 0x4ade80, 0xa78bfa};
            f->color = palette[i % (sizeof(palette) / sizeof(palette[0]))];
        }
        f->bubble_cooldown = randf() * 6000.0f;
        f->has_sprite = false;
        memset(&f->sprite_dsc, 0, sizeof(f->sprite_dsc));
        memset(&f->sprite_flip_dsc, 0, sizeof(f->sprite_flip_dsc));
        f->sprite_buf = NULL;
        f->sprite_flip_buf = NULL;
        int target_w = fish_sprite_target_px(f->size);
        if (i < eng->sprite_preload_count && eng->sprite_preload[i].ready &&
            (int)eng->sprite_preload[i].dsc.header.w == target_w) {
            anim_sprite_preload_t *p = &eng->sprite_preload[i];
            f->sprite_dsc = p->dsc;
            f->sprite_flip_dsc = p->flip_dsc;
            f->sprite_buf = p->buf;
            f->sprite_flip_buf = p->flip_buf;
            p->buf = NULL;
            p->flip_buf = NULL;
            p->ready = false;
            f->has_sprite = f->sprite_dsc.data != NULL;
        } else if (src->icon_bin_path[0] || src->icon_path[0]) {
            struct stat st;
            bool have_asset = (src->icon_bin_path[0] && stat(src->icon_bin_path, &st) == 0 && st.st_size > 0) ||
                              (src->icon_path[0] && stat(src->icon_path, &st) == 0 && st.st_size > 0);
            if (have_asset) {
                const char *png = src->icon_path[0] ? src->icon_path : src->icon_bin_path;
                if (fish_img_load_sprite_fit_width(png, src->icon_bin_path, target_w, &f->sprite_dsc, &f->sprite_buf,
                                                   false) == ESP_OK) {
                    f->has_sprite = true;
                }
                fish_img_load_sprite_fit_width(png, src->icon_path_flip, target_w, &f->sprite_flip_dsc,
                                               &f->sprite_flip_buf, true);
            }
        }
        f->prev_x = f->x;
        f->prev_y = f->y;
        if (f->has_sprite) {
            create_fish_widget(eng, f);
        }
    }
    if (eng->canvas) {
        lv_obj_move_foreground(eng->canvas);
    }
}

static void init_bubble_widgets(anim_engine_t *eng);
static void sync_bubble_widgets(anim_engine_t *eng);
static void anim_update_algae_bands(anim_engine_t *eng);

static void init_bubbles(anim_engine_t *eng)
{
    float H = (float)eng->view_h;
    for (int i = 0; i < ANIM_MAX_BUBBLES; i++) {
        eng->bubbles[i].x = randf() * ANIM_W;
        eng->bubbles[i].y = H * (0.4f + randf() * 0.6f);
        eng->bubbles[i].r = 1.5f + randf() * 3.0f;
        eng->bubbles[i].speed = 8.0f + randf() * 16.0f;
        eng->bubbles[i].prev_x = eng->bubbles[i].x;
        eng->bubbles[i].prev_y = eng->bubbles[i].y;
        eng->bubbles[i].dot = NULL;
    }
    init_bubble_widgets(eng);
}

static void init_bubble_widgets(anim_engine_t *eng)
{
    if (!eng || !eng->root) {
        return;
    }
    for (int i = 0; i < ANIM_MAX_BUBBLES; i++) {
        anim_bubble_t *b = &eng->bubbles[i];
        if (b->dot) {
            lv_obj_del(b->dot);
        }
        b->dot = lv_obj_create(eng->root);
        lv_obj_remove_style_all(b->dot);
        lv_coord_t d = (lv_coord_t)(b->r * 2.0f);
        if (d < 2) {
            d = 2;
        }
        lv_obj_set_size(b->dot, d, d);
        lv_obj_set_style_radius(b->dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(b->dot, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(b->dot, LV_OPA_60, 0);
        lv_obj_clear_flag(b->dot, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(b->dot, (lv_coord_t)(b->x - b->r), (lv_coord_t)(b->y - b->r));
    }
    if (eng->canvas) {
        lv_obj_move_foreground(eng->canvas);
    }
}

static void sync_bubble_widgets(anim_engine_t *eng)
{
    if (!eng) {
        return;
    }
    for (int i = 0; i < ANIM_MAX_BUBBLES; i++) {
        anim_bubble_t *b = &eng->bubbles[i];
        if (!b->dot) {
            continue;
        }
        lv_coord_t d = (lv_coord_t)(b->r * 2.0f);
        if (d < 2) {
            d = 2;
        }
        lv_obj_set_size(b->dot, d, d);
        lv_coord_t nx = (lv_coord_t)(b->x - b->r);
        lv_coord_t ny = (lv_coord_t)(b->y - b->r);
        if (lv_obj_get_x(b->dot) != nx || lv_obj_get_y(b->dot) != ny) {
            lv_obj_set_pos(b->dot, nx, ny);
        }
    }
}

static void anim_update_algae_bands(anim_engine_t *eng)
{
    if (!eng || !eng->root) {
        return;
    }
    int levels[3] = {0, 0, 0};
    if (eng->has_interaction) {
        levels[0] = eng->interaction.algae_left;
        levels[1] = eng->interaction.algae_mid;
        levels[2] = eng->interaction.algae_right;
    }
    lv_coord_t third_w = (lv_coord_t)(ANIM_W / 3);
    for (int i = 0; i < 3; i++) {
        if (!eng->algae_band[i]) {
            eng->algae_band[i] = lv_obj_create(eng->root);
            lv_obj_remove_style_all(eng->algae_band[i]);
            lv_obj_clear_flag(eng->algae_band[i], LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_style_bg_color(eng->algae_band[i], lv_color_hex(0x22783c), 0);
        }
        if (levels[i] <= 0) {
            lv_obj_add_flag(eng->algae_band[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_opa_t opa = (lv_opa_t)fminf(160.0f, 24.0f + (float)levels[i] * 18.0f);
        lv_obj_set_size(eng->algae_band[i], third_w, eng->view_h);
        lv_obj_set_pos(eng->algae_band[i], i * third_w, 0);
        lv_obj_set_style_bg_opa(eng->algae_band[i], opa, 0);
        lv_obj_clear_flag(eng->algae_band[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (eng->canvas) {
        lv_obj_move_foreground(eng->canvas);
    }
}

static float water_speed_mul(const anim_engine_t *eng)
{
    if (eng->interaction.water_quality <= 3) {
        return 0.5f;
    }
    return 1.0f;
}

static inline lv_coord_t ax(const anim_engine_t *eng, float x)
{
    return eng->draw_ox + (lv_coord_t)x;
}

static inline lv_coord_t ay(const anim_engine_t *eng, float y)
{
    return eng->draw_oy + (lv_coord_t)y;
}

static void anim_update_water_overlay(anim_engine_t *eng)
{
    if (!eng || !eng->overlay_water) {
        return;
    }
    if (!eng->has_interaction) {
        lv_obj_add_flag(eng->overlay_water, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    int q = eng->interaction.water_quality;
    if (q >= 7) {
        lv_obj_add_flag(eng->overlay_water, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (q < 0) {
        q = 0;
    }
    lv_opa_t alpha = (lv_opa_t)((7.0f - (float)q) / 7.0f * 89.0f);
    lv_obj_set_style_bg_color(eng->overlay_water, lv_color_hex(0xb49628), 0);
    lv_obj_set_style_bg_opa(eng->overlay_water, alpha, 0);
    lv_obj_clear_flag(eng->overlay_water, LV_OBJ_FLAG_HIDDEN);
}

static int x_to_third(float x)
{
    int t = (int)(x / ((float)ANIM_W / 3.0f));
    if (t < 0) {
        t = 0;
    }
    if (t > 2) {
        t = 2;
    }
    return t;
}

static void mark_thirds(bool dirty[3], float cx, float cy, float half, lv_coord_t view_h)
{
    (void)cy;
    (void)view_h;
    int lx = (int)(cx - half);
    int hx = (int)(cx + half);
    if (lx < 0) {
        lx = 0;
    }
    if (hx >= ANIM_W) {
        hx = ANIM_W - 1;
    }
    int t0 = x_to_third((float)lx);
    int t1 = x_to_third((float)hx);
    for (int t = t0; t <= t1; t++) {
        dirty[t] = true;
    }
}

static void anim_invalidate_thirds(anim_engine_t *eng, const bool dirty[3])
{
    lv_coord_t third_w = (lv_coord_t)(ANIM_W / 3);
    for (int t = 0; t < 3; t++) {
        if (!dirty[t]) {
            continue;
        }
        lv_area_t area = {(lv_coord_t)(t * third_w), 0, (lv_coord_t)((t + 1) * third_w - 1),
                          (lv_coord_t)(eng->view_h - 1)};
        lv_obj_invalidate_area(eng->canvas, &area);
    }
}

static void anim_invalidate_feed(anim_engine_t *eng)
{
    bool dirty[3] = {false, false, false};
    const float pad = 48.0f;
    for (int i = 0; i < ANIM_MAX_PARTICLES; i++) {
        anim_particle_t *p = &eng->particles[i];
        if (!p->eaten) {
            mark_thirds(dirty, p->x, p->y, p->r + pad, eng->view_h);
        }
    }
    anim_invalidate_thirds(eng, dirty);
}

static void anim_invalidate_dirty(anim_engine_t *eng)
{
    if (!eng || !eng->canvas) {
        return;
    }
    if (eng->feed_active) {
        anim_invalidate_feed(eng);
    } else if (eng->clean.active) {
        lv_obj_invalidate(eng->canvas);
    }
}

static void anim_handle_screen_tap(anim_engine_t *eng, lv_coord_t sx, lv_coord_t sy)
{
    if (!eng || !eng->root) {
        return;
    }
    int64_t now = now_ms();
    if (eng->last_tap_ms > 0 && (now - eng->last_tap_ms) < 350) {
        return;
    }
    eng->last_tap_ms = now;

    lv_area_t a;
    lv_obj_get_coords(eng->root, &a);
    if (sx < a.x1 || sx > a.x2 || sy < a.y1 || sy > a.y2) {
        return;
    }
    int lx = (int)(sx - a.x1);
    int ly = (int)(sy - a.y1);
    ESP_LOGI(TAG, "tank tap (%d,%d)", lx, ly);
    anim_engine_handle_tap(eng, lx, ly);
}

static void anim_canvas_tap_cb(lv_event_t *e)
{
    anim_engine_t *eng = lv_event_get_user_data(e);
    if (!eng || lv_event_get_code(e) != LV_EVENT_PRESSED) {
        return;
    }
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) {
        return;
    }
    lv_point_t pt;
    lv_indev_get_point(indev, &pt);
    anim_handle_screen_tap(eng, pt.x, pt.y);
}


static void anim_update_bg_img(anim_engine_t *eng)
{
    if (!eng || !eng->bg_img) {
        return;
    }
    if (eng->bg_dsc.data) {
        lv_img_set_src(eng->bg_img, &eng->bg_dsc);
        lv_obj_clear_flag(eng->bg_img, LV_OBJ_FLAG_HIDDEN);
        if (eng->root) {
            lv_obj_set_style_bg_opa(eng->root, LV_OPA_TRANSP, 0);
        }
    } else {
        lv_obj_add_flag(eng->bg_img, LV_OBJ_FLAG_HIDDEN);
        if (eng->root) {
            lv_obj_set_style_bg_color(eng->root, lv_color_hex(0x0ea5e9), 0);
            lv_obj_set_style_bg_opa(eng->root, LV_OPA_COVER, 0);
        }
    }
}

static void draw_decorations(anim_engine_t *eng, lv_draw_ctx_t *ctx)
{
    float W = (float)ANIM_W;
    float H = (float)eng->view_h;
    for (int i = 0; i < eng->deco_count; i++) {
        anim_deco_t *ad = &eng->decos[i];
        if (!ad->loaded || !ad->dsc.data) {
            continue;
        }
        float cx = ad->x * W;
        float cy = ad->y * H;
        float half_w = (float)ad->w / 2.0f;
        float half_h = (float)ad->h / 2.0f;
        lv_draw_img_dsc_t img_dsc;
        lv_draw_img_dsc_init(&img_dsc);
        lv_area_t a = {
            ax(eng, cx - half_w),
            ay(eng, cy - half_h),
            ax(eng, cx + half_w),
            ay(eng, cy + half_h),
        };
        lv_draw_img(ctx, &img_dsc, &a, &ad->dsc);
    }
}

static void draw_algae(anim_engine_t *eng, lv_draw_ctx_t *ctx)
{
    const fish_interaction_t *it = &eng->interaction;
    int levels[3] = {it->algae_left, it->algae_mid, it->algae_right};
    int max_lv = levels[0];
    if (levels[1] > max_lv) {
        max_lv = levels[1];
    }
    if (levels[2] > max_lv) {
        max_lv = levels[2];
    }
    if (max_lv <= 0) {
        return;
    }

    float third = (float)ANIM_W / 3.0f;
    float view_h = (float)eng->view_h;
    uint32_t seed = 12345;
    int count = max_lv * 3;
    if (count > 18) {
        count = 18;
    }
    for (int i = 0; i < count; i++) {
        seed = seed * 9301 + 49297;
        float rx = (float)(seed % 233280) / 233280.0f;
        seed = seed * 9301 + 49297;
        float ry = (float)(seed % 233280) / 233280.0f;
        float x = rx * ANIM_W;
        float y = view_h * (0.55f + ry * 0.4f);
        float lv = x < third ? (float)levels[0] : (x < third * 2 ? (float)levels[1] : (float)levels[2]);
        if (lv <= 0.01f) {
            continue;
        }
        seed = seed * 9301 + 49297;
        float rr = 6.0f + (float)(seed % 233280) / 233280.0f * 10.0f;
        lv_draw_rect_dsc_t dsc;
        lv_draw_rect_dsc_init(&dsc);
        dsc.radius = (lv_coord_t)rr;
        dsc.bg_color = lv_color_hex(0x22783c);
        dsc.bg_opa = (lv_opa_t)fminf(160, 24 + lv * 18);
        lv_area_t a = {ax(eng, x - rr), ay(eng, y - rr), ax(eng, x + rr), ay(eng, y + rr)};
        lv_draw_rect(ctx, &dsc, &a);
    }
}

static void draw_bubble_text(anim_engine_t *eng, lv_draw_ctx_t *ctx, anim_fish_t *f)
{
    if (!f->bubble_text[0] || now_ms() > (int64_t)f->bubble_until) {
        return;
    }
    lv_draw_label_dsc_t dsc;
    lv_draw_label_dsc_init(&dsc);
    dsc.color = lv_color_hex(0x334155);
    dsc.font = &fish_font_24;
    lv_coord_t px = ax(eng, f->x);
    lv_coord_t py = ay(eng, f->y - f->size / 2 - 16);
    lv_area_t coords = {px, py, (lv_coord_t)(px + 120), (lv_coord_t)(py + 20)};
    lv_draw_label(ctx, &dsc, &coords, f->bubble_text, NULL);
}

static void update_status_bubble(anim_engine_t *eng, anim_fish_t *f)
{
    const fish_interaction_t *it = &eng->interaction;
    int64_t now = now_ms();
    if (f->bubble_thanks || f->bubble_feed) {
        return;
    }
    int sev_h = it->satiety <= 1 ? 2 : (it->satiety <= 3 ? 1 : 0);
    int sev_w = it->water_quality <= 3 ? 2 : (it->water_quality <= 5 ? 1 : 0);
    int sev_a = it->algae_level >= 4 ? 2 : (it->algae_level >= 2 ? 1 : 0);
    int max_sev = sev_h > sev_w ? sev_h : sev_w;
    if (sev_a > max_sev) {
        max_sev = sev_a;
    }
    if (max_sev == 0) {
        return;
    }
    if (now < (int64_t)f->bubble_until || now < (int64_t)f->bubble_cooldown) {
        return;
    }
    float rate = max_sev == 2 ? 0.05f : 0.015f;
    if (randf() > rate) {
        return;
    }
    if (sev_h >= max_sev) {
        strncpy(f->bubble_text, it->satiety <= 1 ? "求投喂!" : "有点饿了…", sizeof(f->bubble_text) - 1);
    } else if (sev_w >= max_sev) {
        strncpy(f->bubble_text, it->water_quality <= 3 ? "好臭啊!" : "水有点浑浊…", sizeof(f->bubble_text) - 1);
    } else {
        strncpy(f->bubble_text, it->algae_level >= 4 ? "快刮藻!!" : "有点看不清", sizeof(f->bubble_text) - 1);
    }
    f->bubble_until = (float)(now + 1200 + rand() % 1800);
    f->bubble_cooldown = (float)(now + (max_sev == 2 ? 2500 : 3500) + rand() % 3500);
}

static void draw_fish_sprite(anim_engine_t *eng, lv_draw_ctx_t *ctx, anim_fish_t *f)
{
    const lv_img_dsc_t *img = NULL;
    if (f->has_sprite) {
        if (f->facing < 0 && f->sprite_flip_dsc.data) {
            img = &f->sprite_flip_dsc;
        } else if (f->sprite_dsc.data) {
            img = &f->sprite_dsc;
        }
    }
    if (img && img->data) {
        lv_draw_img_dsc_t dsc;
        lv_draw_img_dsc_init(&dsc);
        if (fabsf(f->tilt) > 0.01f) {
            dsc.angle = (int16_t)(f->tilt * 10);
            dsc.pivot.x = img->header.w / 2;
            dsc.pivot.y = img->header.h / 2;
        }
        lv_coord_t iw = img->header.w;
        lv_coord_t ih = img->header.h;
        lv_area_t a = {
            ax(eng, f->x - iw / 2),
            ay(eng, f->y - ih / 2),
            ax(eng, f->x - iw / 2 + iw - 1),
            ay(eng, f->y - ih / 2 + ih - 1),
        };
        lv_draw_img(ctx, &dsc, &a, img);
    } else {
        /* Offline/mock data may not include iconUrl. Keep a recognizable fish
         * silhouette instead of the old capsule placeholder. */
        float w = f->size;
        float h = f->size * 0.42f;
        if (h < 6.0f) {
            h = 6.0f;
        }
        float tilt_rad = f->tilt * 0.017453292f;
        float skew = sinf(tilt_rad) * h * 0.25f;
        lv_coord_t cx = ax(eng, f->x);
        lv_coord_t cy = ay(eng, f->y + skew);
        int dir = f->facing >= 0 ? 1 : -1;
        uint32_t col = f->color ? f->color : 0xf97316u;

        float wag = sinf((float)eng->frame * 0.22f + f->phase) * h * 0.14f;
        lv_draw_rect_dsc_t body;
        lv_draw_rect_dsc_init(&body);
        body.radius = (lv_coord_t)(h * 0.45f);
        body.bg_color = lv_color_hex(col);
        body.bg_opa = LV_OPA_COVER;
        lv_area_t body_a = {
            cx - (lv_coord_t)(w * 0.34f),
            cy - (lv_coord_t)(h / 2.0f),
            cx + (lv_coord_t)(w * 0.34f),
            cy + (lv_coord_t)(h / 2.0f),
        };
        lv_draw_rect(ctx, &body, &body_a);

        lv_draw_rect_dsc_t tail;
        lv_draw_rect_dsc_init(&tail);
        tail.bg_color = lv_color_hex(col);
        tail.bg_opa = LV_OPA_70;
        lv_point_t tail_points[3] = {
            {cx - (lv_coord_t)(dir * w * 0.25f), cy},
            {cx - (lv_coord_t)(dir * w * 0.57f), cy - (lv_coord_t)(h * 0.56f) + (lv_coord_t)wag},
            {cx - (lv_coord_t)(dir * w * 0.57f), cy + (lv_coord_t)(h * 0.56f) + (lv_coord_t)wag},
        };
        lv_draw_triangle(ctx, &tail, tail_points);

        lv_draw_rect_dsc_t fin;
        lv_draw_rect_dsc_init(&fin);
        fin.bg_color = lv_color_hex(col);
        fin.bg_opa = LV_OPA_80;
        lv_point_t dorsal[3] = {
            {cx - (lv_coord_t)(w * 0.05f), cy - (lv_coord_t)(h * 0.38f)},
            {cx + (lv_coord_t)(w * 0.10f), cy - (lv_coord_t)(h * 0.78f)},
            {cx + (lv_coord_t)(w * 0.20f), cy - (lv_coord_t)(h * 0.30f)},
        };
        lv_draw_triangle(ctx, &fin, dorsal);
        lv_point_t ventral[3] = {
            {cx - (lv_coord_t)(w * 0.02f), cy + (lv_coord_t)(h * 0.36f)},
            {cx + (lv_coord_t)(w * 0.13f), cy + (lv_coord_t)(h * 0.70f)},
            {cx + (lv_coord_t)(w * 0.23f), cy + (lv_coord_t)(h * 0.27f)},
        };
        lv_draw_triangle(ctx, &fin, ventral);

        lv_draw_line_dsc_t gill;
        lv_draw_line_dsc_init(&gill);
        gill.color = lv_color_hex(0xffffff);
        gill.opa = LV_OPA_50;
        gill.width = 2;
        lv_point_t gill_points[2] = {
            {cx + (lv_coord_t)(dir * w * 0.03f), cy - (lv_coord_t)(h * 0.28f)},
            {cx + (lv_coord_t)(dir * w * 0.03f), cy + (lv_coord_t)(h * 0.28f)},
        };
        lv_draw_line(ctx, &gill, &gill_points[0], &gill_points[1]);

        lv_draw_rect_dsc_t eye;
        lv_draw_rect_dsc_init(&eye);
        eye.radius = LV_RADIUS_CIRCLE;
        eye.bg_color = lv_color_white();
        eye.bg_opa = LV_OPA_COVER;
        lv_coord_t er = (lv_coord_t)fmaxf(2.0f, h * 0.14f);
        lv_coord_t ex = cx + (lv_coord_t)(dir * w * 0.24f);
        lv_coord_t ey = cy - (lv_coord_t)(h * 0.1f);
        lv_area_t eye_a = {ex - er, ey - er, ex + er, ey + er};
        lv_draw_rect(ctx, &eye, &eye_a);
    }
    draw_bubble_text(eng, ctx, f);
}

static float clamp_base_y(const anim_fish_t *f, float y)
{
    return fmaxf(f->range_top, fminf(f->range_bottom, y));
}

static bool particle_alive(const anim_engine_t *eng, const anim_particle_t *p)
{
    if (!p || p->eaten) {
        return false;
    }
    for (int i = 0; i < ANIM_MAX_PARTICLES; i++) {
        if (&eng->particles[i] == p) {
            return true;
        }
    }
    return false;
}

static void trigger_feed_bubble(anim_fish_t *f)
{
    static const char *lines[] = {"好吃！", "好香！", "谢谢主人！", "再来一颗！", "yummy～", "满足～"};
    int n = (int)(sizeof(lines) / sizeof(lines[0]));
    strncpy(f->bubble_text, lines[rand() % n], sizeof(f->bubble_text) - 1);
    f->bubble_text[sizeof(f->bubble_text) - 1] = '\0';
    f->bubble_until = (float)(now_ms() + 1200 + (int)(randf() * 600.0f));
    f->bubble_cooldown = (float)(now_ms() + 4000 + (int)(randf() * 2000.0f));
    f->bubble_color = 0xf97316;
    f->bubble_feed = true;
    f->bubble_level = 1;
}

static bool update_seeking(anim_engine_t *eng, anim_fish_t *f, int fi, float dt)
{
    float W = (float)ANIM_W;
    float H = (float)eng->view_h;
    anim_particle_t *target = (anim_particle_t *)f->seek;

    if (target && (target->eaten || !particle_alive(eng, target))) {
        target = NULL;
        f->seek = NULL;
    }

    if (!target) {
        anim_particle_t *best = NULL;
        float best_d = 1e30f;
        for (int i = 0; i < ANIM_MAX_PARTICLES; i++) {
            anim_particle_t *p = &eng->particles[i];
            if (p->eaten || p->taken_by >= 0) {
                continue;
            }
            float dx = p->x - f->x;
            float dy = p->y - f->y;
            float d = dx * dx + dy * dy;
            if (d < best_d) {
                best_d = d;
                best = p;
            }
        }
        if (!best) {
            return false;
        }
        best->taken_by = fi;
        f->seek = best;
        target = best;
        f->turning = false;
        f->turn_t = 0;
    }

    float dx = target->x - f->x;
    float hyst = fmaxf(3.0f, f->size * 0.12f);
    int dir_to = dx > hyst ? 1 : (dx < -hyst ? -1 : f->dir);
    if (f->dir != dir_to) {
        f->dir = dir_to;
        f->facing = -dir_to;
    }

    float head_x = f->x + (float)f->dir * f->size * 0.5f;
    float head_y = f->y;
    float dist_head = hypotf(target->x - head_x, target->y - head_y);

    float mul;
    if (dist_head < f->size * 0.5f) {
        mul = 1.2f;
    } else if (dist_head < f->size * 1.2f) {
        mul = 1.8f;
    } else {
        mul = 3.0f;
    }

    float step_x = (float)f->dir * f->vx * mul * dt;
    float max_step_x = fmaxf(f->size * 0.3f, target->r + 2.0f);
    if (step_x > max_step_x) {
        step_x = max_step_x;
    } else if (step_x < -max_step_x) {
        step_x = -max_step_x;
    }
    f->x += step_x;

    float dy = target->y - f->base_y;
    float max_vy = fmaxf(f->size * 0.5f, fabsf(f->vx) * 2.0f) * dt;
    float step_y = dy * 0.2f;
    if (step_y > max_vy) {
        step_y = max_vy;
    } else if (step_y < -max_vy) {
        step_y = -max_vy;
    }
    f->base_y += step_y;
    f->target_base_y = f->base_y;

    float m = f->size / 2 + 2;
    f->x = fmaxf(m, fminf(W - m, f->x));
    f->base_y = fmaxf(m, fminf(H - m, f->base_y));

    bool edge_hit = dist_head < target->r + 1.5f;
    bool vert_overlap =
        fabsf(target->x - f->x) < f->size * 0.5f + target->r && fabsf(target->y - f->y) < f->size * 0.5f;

    if (edge_hit || vert_overlap) {
        f->peck = 1;
        target->eaten = true;
        target->taken_by = -1;
        f->seek = NULL;
        f->fed_this_round = true;
        f->pause_until = 0;
        trigger_feed_bubble(f);
    }
    return true;
}

static void update_fish(anim_engine_t *eng, anim_fish_t *f, int fi, float t)
{
    const float dt = DT;
    const float W = (float)ANIM_W;
    const float H = (float)eng->view_h;
    const float now = (float)now_ms();
    const bool feeding = eng->feed_active;

    if (feeding && f->feed_delay > 0) {
        f->feed_delay -= dt * 1000.0f;
        if (f->feed_delay < 0) {
            f->feed_delay = 0;
        }
    }

    if (f->peck > 0) {
        f->peck = fmaxf(0, f->peck - dt * 3.2f);
        f->x += (float)f->dir * f->vx * 0.35f * dt;
        float m2 = f->size / 2 + 4;
        f->x = fmaxf(m2, fminf(W - m2, f->x));
    } else if (feeding && !f->fed_this_round && f->feed_delay <= 0 && update_seeking(eng, f, fi, dt)) {
        /* 觅食由 update_seeking 处理位移 */
    } else {
        if (f->turning) {
            f->turn_t += dt;
            if (f->turn_t >= f->turn_duration) {
                f->turning = false;
                f->turn_t = 0;
                f->dir = -f->dir;
                f->facing = -f->facing;
            }
        } else if (now >= f->pause_until) {
            if (randf() < f->pause_prob) {
                f->pause_until = now + 500 + randf() * 1800;
            } else {
                f->x += (float)f->dir * f->vx * water_speed_mul(eng) * dt;
            }
            float margin = f->size / 2 + 4;
            if ((f->x <= margin && f->dir == -1) || (f->x >= W - margin && f->dir == 1)) {
                if (f->x <= margin && f->dir == -1) {
                    f->x = margin;
                } else {
                    f->x = W - margin;
                }
                f->turning = true;
                f->turn_t = 0;
                f->turn_duration = 0.22f + randf() * 0.12f;
                f->turn_sign = randf() > 0.5f ? 1 : -1;
                f->target_base_y = clamp_base_y(f, f->base_y + (randf() - 0.5f) * H * 0.1f);
            }
        }
        if (fabsf(f->base_y - f->target_base_y) >= 0.5f) {
            f->base_y += (f->target_base_y - f->base_y) * 0.08f;
        } else {
            f->base_y = f->target_base_y;
        }
    }

    float target_amp_mul = (f->seek != NULL && f->peck <= 0) ? 0.55f : 1.0f;
    f->amp_mul += (target_amp_mul - f->amp_mul) * 0.08f;

    float phase = t * 0.05f * f->freq + f->phase;
    float target_y = f->peck > 0 ? f->base_y : f->base_y + sinf(phase) * f->amp * f->amp_mul;
    float target_tilt = f->peck > 0 ? 0 : cosf(phase) * f->amp * 0.006f;
    f->y += (target_y - f->y) * 0.25f;
    f->tilt += (target_tilt - f->tilt) * 0.2f;

    update_status_bubble(eng, f);
}

static void update_bubbles(anim_engine_t *eng)
{
    for (int i = 0; i < ANIM_MAX_BUBBLES; i++) {
        anim_bubble_t *b = &eng->bubbles[i];
        b->y -= b->speed * DT;
        b->x += sinf(eng->frame * 0.05f + b->r) * 0.3f;
        if (b->y < -6) {
            b->y = (float)eng->view_h + 6;
            b->x = randf() * ANIM_W;
        }
    }
}

static void update_feed(anim_engine_t *eng)
{
    if (!eng->feed_active) {
        return;
    }

    int remain = 0;
    for (int i = 0; i < ANIM_MAX_PARTICLES; i++) {
        anim_particle_t *p = &eng->particles[i];
        if (p->eaten) {
            continue;
        }
        p->y += p->vy * DT;
        if (p->y > (float)eng->view_h + 8.0f) {
            p->eaten = true;
            p->taken_by = -1;
            continue;
        }
        remain++;
    }

    if (remain == 0) {
        eng->feed_active = false;
        for (int fi = 0; fi < eng->fish_count; fi++) {
            eng->fishes[fi].seek = NULL;
        }
    }
}

static void anim_sim_step(anim_engine_t *eng)
{
    eng->step_dt = DT;
    eng->frame++;
    eng->sim_time = (float)eng->frame * DT;

    for (int i = 0; i < eng->fish_count; i++) {
        eng->fishes[i].prev_x = eng->fishes[i].x;
        eng->fishes[i].prev_y = eng->fishes[i].y;
    }
    for (int i = 0; i < ANIM_MAX_BUBBLES; i++) {
        eng->bubbles[i].prev_x = eng->bubbles[i].x;
        eng->bubbles[i].prev_y = eng->bubbles[i].y;
    }
    update_bubbles(eng);
    update_feed(eng);
    for (int i = 0; i < eng->fish_count; i++) {
        update_fish(eng, &eng->fishes[i], i, (float)eng->frame);
    }
}

static void draw_particles(anim_engine_t *eng, lv_draw_ctx_t *ctx)
{
    for (int i = 0; i < ANIM_MAX_PARTICLES; i++) {
        anim_particle_t *p = &eng->particles[i];
        if (p->eaten) {
            continue;
        }
        lv_draw_rect_dsc_t dsc;
        lv_draw_rect_dsc_init(&dsc);
        dsc.radius = LV_RADIUS_CIRCLE;
        dsc.bg_color = lv_color_hex(0x0f0f0f);
        lv_area_t a = {ax(eng, p->x - p->r), ay(eng, p->y - p->r),
                       ax(eng, p->x + p->r), ay(eng, p->y + p->r)};
        lv_draw_rect(ctx, &dsc, &a);
        lv_draw_rect_dsc_t hi;
        lv_draw_rect_dsc_init(&hi);
        hi.radius = LV_RADIUS_CIRCLE;
        hi.bg_color = lv_color_white();
        hi.bg_opa = LV_OPA_40;
        float hr = p->r * 0.35f;
        lv_area_t ha = {ax(eng, p->x - hr), ay(eng, p->y - hr), ax(eng, p->x + hr), ay(eng, p->y + hr)};
        lv_draw_rect(ctx, &hi, &ha);
    }
}

static void draw_clean_fx(anim_engine_t *eng, lv_draw_ctx_t *ctx)
{
    if (!eng->clean.active) {
        return;
    }
    int64_t elapsed = now_ms() - eng->clean.start_ms;
    float p = (float)elapsed / (float)eng->clean.dur_ms;
    if (p >= 1.0f) {
        eng->clean.active = false;
        return;
    }
    float ease = 1.0f - (1.0f - p) * (1.0f - p);
    float sweep_y = (float)eng->view_h * ease;
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = lv_color_white();
    dsc.bg_opa = (lv_opa_t)(128 * (1.0f - ease));
    lv_area_t a = {ax(eng, eng->clean.x0), ay(eng, 0), ax(eng, eng->clean.x1), ay(eng, sweep_y)};
    lv_draw_rect(ctx, &dsc, &a);
}

static bool area_intersects_clip(const lv_area_t *clip, lv_area_t *box)
{
    lv_area_t overlap;
    return _lv_area_intersect(&overlap, clip, box);
}

static void anim_canvas_event(lv_event_t *e)
{
    anim_engine_t *eng = lv_event_get_user_data(e);
    if (!eng || lv_event_get_code(e) != LV_EVENT_DRAW_MAIN) {
        return;
    }
    lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(e);
    lv_area_t coords;
    lv_obj_get_coords(eng->canvas, &coords);
    eng->draw_ox = coords.x1;
    eng->draw_oy = coords.y1;
    if (eng->feed_active) {
        draw_particles(eng, ctx);
    }
    if (eng->clean.active) {
        draw_clean_fx(eng, ctx);
    }
}

static void anim_lv_sync_cb(void *user)
{
    anim_engine_t *eng = user;
    if (!eng) {
        return;
    }
    eng->lv_sync_pending = false;
    sync_fish_widgets(eng);
    sync_bubble_widgets(eng);
    anim_invalidate_dirty(eng);
}

static void anim_esp_timer_cb(void *arg)
{
    anim_engine_t *eng = arg;
    if (!eng || eng->paused) {
        return;
    }
    int64_t now_us = esp_timer_get_time();
    float real_dt = DT;
    if (eng->last_tick_us > 0) {
        real_dt = (float)(now_us - eng->last_tick_us) / 1000000.0f;
    }
    eng->last_tick_us = now_us;
    if (real_dt < 0.001f) {
        real_dt = 0.001f;
    }
    if (real_dt > 0.25f) {
        real_dt = 0.25f;
    }

    eng->sim_accum += real_dt;
    int steps = 0;
    while (eng->sim_accum >= DT && steps < 5) {
        eng->sim_accum -= DT;
        anim_sim_step(eng);
        steps++;
    }
    if (steps == 0) {
        anim_sim_step(eng);
        steps = 1;
    }

    if (eng->frame == 1 || (eng->frame % 120) == 0) {
        ESP_LOGD(TAG, "anim frame=%d fish=%d steps=%d real=%.0fms", eng->frame, eng->fish_count, steps,
                 real_dt * 1000.0f);
    }

    if (!eng->lv_sync_pending) {
        eng->lv_sync_pending = true;
        lv_async_call(anim_lv_sync_cb, eng);
    }
}

anim_engine_t *anim_engine_create(lv_obj_t *parent)
{
    anim_engine_t *eng = calloc(1, sizeof(*eng));
    if (!eng) {
        return NULL;
    }
    eng->gen = 1;
    eng->step_dt = DT;
    eng->view_h = (lv_coord_t)ANIM_VIEW_H;
    if (eng->view_h < 200) {
        eng->view_h = 200;
    }
    /* Root container: static bg (lv_img) + transparent fish layer. */
    eng->root = lv_obj_create(parent);
    lv_obj_remove_style_all(eng->root);
    lv_obj_set_size(eng->root, ANIM_W, eng->view_h);
    lv_obj_align(eng->root, LV_ALIGN_TOP_MID, 0, ANIM_CANVAS_TOP_Y);
    lv_obj_set_style_bg_color(eng->root, lv_color_hex(0x0ea5e9), 0);
    lv_obj_set_style_bg_opa(eng->root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(eng->root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(eng->root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(eng->root, anim_canvas_tap_cb, LV_EVENT_PRESSED, eng);

    eng->bg_img = lv_img_create(eng->root);
    lv_obj_set_size(eng->bg_img, ANIM_W, eng->view_h);
    lv_obj_align(eng->bg_img, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_add_flag(eng->bg_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(eng->bg_img, LV_OBJ_FLAG_CLICKABLE);

    eng->overlay_vignette = lv_obj_create(eng->root);
    lv_obj_remove_style_all(eng->overlay_vignette);
    lv_obj_set_size(eng->overlay_vignette, ANIM_W, eng->view_h);
    lv_obj_align(eng->overlay_vignette, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_add_flag(eng->overlay_vignette, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(eng->overlay_vignette, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    eng->overlay_water = lv_obj_create(eng->root);
    lv_obj_remove_style_all(eng->overlay_water);
    lv_obj_set_size(eng->overlay_water, ANIM_W, eng->view_h);
    lv_obj_align(eng->overlay_water, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_add_flag(eng->overlay_water, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(eng->overlay_water, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    eng->canvas = lv_obj_create(eng->root);
    lv_obj_remove_style_all(eng->canvas);
    lv_obj_set_size(eng->canvas, ANIM_W, eng->view_h);
    lv_obj_align(eng->canvas, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_opa(eng->canvas, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(eng->canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(eng->canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(eng->canvas, anim_canvas_event, LV_EVENT_DRAW_MAIN, eng);
    lv_obj_add_event_cb(eng->canvas, anim_canvas_tap_cb, LV_EVENT_PRESSED, eng);
    eng->canvas_buf = NULL;
    init_bubbles(eng);
    eng->tap_mode = FISH_TAP_FEED;
    return eng;
}

void anim_engine_destroy(anim_engine_t *eng)
{
    if (!eng) {
        return;
    }
    anim_engine_stop(eng);
    free_sprite_preload(eng);
    if (eng->root) {
        lv_obj_del(eng->root);
        eng->root = NULL;
        eng->bg_img = NULL;
        eng->overlay_vignette = NULL;
        eng->overlay_water = NULL;
        eng->canvas = NULL;
    } else if (eng->canvas) {
        lv_obj_del(eng->canvas);
        eng->canvas = NULL;
    }
    for (int i = 0; i < eng->fish_count; i++) {
        anim_fish_t *f = &eng->fishes[i];
        fish_img_free(&f->sprite_dsc, &f->sprite_buf);
        fish_img_free(&f->sprite_flip_dsc, &f->sprite_flip_buf);
    }
    fish_img_free(&eng->bg_dsc, &eng->bg_buf);
    free_decorations(eng);
    free(eng->canvas_buf);
    free(eng);
}

void anim_engine_set_tank(anim_engine_t *eng, fish_tank_state_t *tank)
{
    if (!eng) {
        return;
    }
    free_decorations(eng);
    eng->tank = tank;
    if (tank) {
        eng->interaction = tank->interaction;
        eng->has_interaction = true;
        anim_update_bg_img(eng);
        anim_update_water_overlay(eng);
        anim_update_algae_bands(eng);
    } else {
        fish_img_free(&eng->bg_dsc, &eng->bg_buf);
        eng->bg_src_path[0] = '\0';
        eng->has_interaction = false;
        anim_update_bg_img(eng);
        anim_update_water_overlay(eng);
        anim_update_algae_bands(eng);
    }
    rebuild_fishes(eng);
    rebuild_decorations(eng);
    int sprites = 0;
    for (int i = 0; i < eng->fish_count; i++) {
        if (eng->fishes[i].has_sprite) {
            sprites++;
        }
    }
    ESP_LOGI(TAG, "set_tank fish=%d sprites=%d deco=%d bg=%s", eng->fish_count, sprites, eng->deco_count,
             eng->bg_dsc.data ? "yes" : "no");
    if (eng->bg_img) {
        lv_obj_invalidate(eng->bg_img);
    }
    if (eng->canvas) {
        lv_obj_invalidate(eng->canvas);
    }
}

esp_err_t anim_engine_prepare_bg(anim_engine_t *eng, fish_tank_state_t *tank)
{
    if (!eng || !tank) {
        return ESP_ERR_INVALID_ARG;
    }
    const char *bg_src = fish_cache_bg_src_path(tank);
    bool same_bg = bg_src && eng->bg_dsc.data && strcmp(eng->bg_src_path, bg_src) == 0;
    if (!same_bg && bg_src) {
        lv_img_dsc_t new_dsc = {0};
        uint8_t *new_buf = NULL;
        esp_err_t err = fish_img_decode_rgb565(bg_src, ANIM_W, eng->view_h, &new_dsc, &new_buf);
        if (err == ESP_OK) {
            fish_img_free(&eng->bg_dsc, &eng->bg_buf);
            eng->bg_dsc = new_dsc;
            eng->bg_buf = new_buf;
            strncpy(eng->bg_src_path, bg_src, sizeof(eng->bg_src_path) - 1);
        } else {
            ESP_LOGW(TAG, "bg decode failed %s, keep previous", bg_src);
            return err;
        }
    } else if (!bg_src && eng->bg_dsc.data) {
        fish_img_free(&eng->bg_dsc, &eng->bg_buf);
        eng->bg_src_path[0] = '\0';
    }
    return ESP_OK;
}

esp_err_t anim_engine_prepare_fish(anim_engine_t *eng, fish_tank_state_t *tank)
{
    if (!eng || !tank) {
        return ESP_ERR_INVALID_ARG;
    }
    preload_fish_sprites(eng, tank);
    return ESP_OK;
}

esp_err_t anim_engine_prepare_assets(anim_engine_t *eng, fish_tank_state_t *tank)
{
    esp_err_t err = anim_engine_prepare_bg(eng, tank);
    anim_engine_prepare_fish(eng, tank);
    return err;
}

void anim_engine_set_interaction(anim_engine_t *eng, const fish_interaction_t *it)
{
    if (eng && it) {
        eng->interaction = *it;
        eng->has_interaction = true;
        if (eng->tank) {
            eng->tank->interaction = *it;
        }
        anim_update_water_overlay(eng);
        anim_update_algae_bands(eng);
    }
}

void anim_engine_set_tap_mode(anim_engine_t *eng, fish_tap_mode_t mode)
{
    if (eng) {
        eng->tap_mode = mode;
    }
}

void anim_engine_start(anim_engine_t *eng)
{
    if (!eng) {
        return;
    }
    if (eng->esp_timer) {
        esp_timer_stop(eng->esp_timer);
        esp_timer_delete(eng->esp_timer);
        eng->esp_timer = NULL;
    }
    eng->last_tick_us = 0;
    eng->sim_accum = 0;
    eng->step_dt = DT;
    eng->lv_sync_pending = false;
    const esp_timer_create_args_t args = {
        .callback = anim_esp_timer_cb,
        .arg = eng,
        .name = "anim_eng",
        .dispatch_method = ESP_TIMER_TASK,
    };
    if (esp_timer_create(&args, &eng->esp_timer) == ESP_OK) {
        esp_timer_start_periodic(eng->esp_timer, (uint64_t)ANIM_TIMER_MS * 1000ULL);
    }
    ESP_LOGI(TAG, "anim started esp_timer=%p fish=%d view_h=%d", (void *)eng->esp_timer, eng->fish_count,
             eng->view_h);
}

void anim_engine_stop(anim_engine_t *eng)
{
    if (eng && eng->esp_timer) {
        esp_timer_stop(eng->esp_timer);
        esp_timer_delete(eng->esp_timer);
        eng->esp_timer = NULL;
    }
    if (eng) {
        eng->gen++;
        eng->lv_sync_pending = false;
        eng->paused = false;
    }
}

void anim_engine_set_paused(anim_engine_t *eng, bool paused)
{
    if (!eng) {
        return;
    }
    eng->paused = paused;
    if (paused) {
        eng->lv_sync_pending = false;
    }
}

void anim_engine_trigger_feed(anim_engine_t *eng, int x, int y)
{
    if (!eng) {
        return;
    }
    if (x < 0) x = 0;
    if (x >= ANIM_W) x = ANIM_W - 1;
    if (y < 0) y = 0;
    if (y >= (int)eng->view_h) y = (int)eng->view_h - 1;
    if (eng->feed_active) {
        ESP_LOGI(TAG, "feed ignored (already active)");
        return;
    }
    ESP_LOGI(TAG, "feed trigger (%d,%d)", x, y);
    eng->feed_active = true;
    float spawn_y = (float)y + FEED_SPAWN_DROP;
    for (int i = 0; i < ANIM_MAX_PARTICLES; i++) {
        anim_particle_t *p = &eng->particles[i];
        p->eaten = false;
        p->taken_by = -1;
        p->x = (float)x + (randf() - 0.5f) * 20.0f;
        p->y = spawn_y + (randf() - 0.5f) * 10.0f;
        p->vy = 10 + randf() * 9;
        p->r = 1.5f + randf();
    }
    for (int i = 0; i < eng->fish_count; i++) {
        eng->fishes[i].fed_this_round = false;
        eng->fishes[i].feed_delay = randf() * 900;
        eng->fishes[i].seek = NULL;
    }
    if (eng->on_interaction) {
        eng->on_interaction("feed", NULL, eng->interaction_user);
    }
    anim_invalidate_feed(eng);
}

void anim_engine_trigger_water(anim_engine_t *eng)
{
    if (!eng) {
        return;
    }
    eng->water.active = true;
    eng->water.start_ms = now_ms();
    eng->water.dur_ms = 1200;
    for (int i = 0; i < eng->fish_count && i < 3; i++) {
        anim_fish_t *f = &eng->fishes[i];
        strncpy(f->bubble_text, "谢谢主人!", sizeof(f->bubble_text) - 1);
        f->bubble_until = (float)(now_ms() + 2000);
        f->bubble_thanks = true;
    }
    if (eng->on_interaction) {
        eng->on_interaction("water", "virtual", eng->interaction_user);
    }
}

void anim_engine_screen_tap(anim_engine_t *eng, int screen_x, int screen_y)
{
    if (!eng || !eng->root || eng->paused) {
        return;
    }
    /* Fixed layout; do not call lvgl_port_lock here (may run inside taskLVGL indev read). */
    lv_coord_t ox = (lv_coord_t)((CONFIG_FISH_LOGICAL_WIDTH - ANIM_W) / 2);
    lv_coord_t oy = ANIM_CANVAS_TOP_Y;
    lv_coord_t view_h = eng->view_h;
    if (screen_x < ox || screen_x >= ox + (lv_coord_t)ANIM_W || screen_y < oy || screen_y >= oy + view_h) {
        return;
    }
    int lx = (int)(screen_x - ox);
    int ly = (int)(screen_y - oy);
    ESP_LOGI(TAG, "tank tap (%d,%d) screen=(%d,%d)", lx, ly, screen_x, screen_y);
    anim_engine_handle_tap(eng, lx, ly);
}

void anim_engine_handle_tap(anim_engine_t *eng, int x, int y)
{
    if (!eng) {
        return;
    }
    if (x < 0) x = 0;
    if (x >= ANIM_W) x = ANIM_W - 1;
    if (y < 0) y = 0;
    if (y >= (int)eng->view_h) y = (int)eng->view_h - 1;
    if (eng->tap_mode == FISH_TAP_CLEAN) {
        const char *region = x < ANIM_W / 3 ? "left" : (x < ANIM_W * 2 / 3 ? "mid" : "right");
        int lv = 0;
        if (strcmp(region, "left") == 0) {
            lv = eng->interaction.algae_left;
        } else if (strcmp(region, "mid") == 0) {
            lv = eng->interaction.algae_mid;
        } else {
            lv = eng->interaction.algae_right;
        }
        if (lv <= 0) {
            return;
        }
        eng->clean.active = true;
        eng->clean.start_ms = now_ms();
        eng->clean.dur_ms = 500;
        strncpy(eng->clean.region, region, sizeof(eng->clean.region) - 1);
        float third = (float)ANIM_W / 3.0f;
        eng->clean.x0 = strcmp(region, "left") == 0 ? 0 : (strcmp(region, "mid") == 0 ? third : third * 2);
        eng->clean.x1 = eng->clean.x0 + third;
        eng->clean.sweep_dir = randf() > 0.5f ? 1 : -1;
        if (eng->on_interaction) {
            eng->on_interaction("clean", region, eng->interaction_user);
        }
    } else {
        anim_engine_trigger_feed(eng, x, y);
    }
}

void anim_engine_set_interaction_cb(anim_engine_t *eng, void (*cb)(const char *, const char *, void *), void *user)
{
    eng->on_interaction = cb;
    eng->interaction_user = user;
}

lv_obj_t *anim_engine_get_root(anim_engine_t *eng)
{
    return eng ? eng->root : NULL;
}
