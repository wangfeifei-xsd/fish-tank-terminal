#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"
#include "device_api.h"
#include "esp_err.h"
#include "resource_cache.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ANIM_W CONFIG_FISH_LOGICAL_WIDTH
#define ANIM_H CONFIG_FISH_LOGICAL_HEIGHT
#define ANIM_CANVAS_TOP_Y     56
#define ANIM_BOTTOM_RESERVE   48
#define ANIM_VIEW_H           (ANIM_H - ANIM_CANVAS_TOP_Y - ANIM_BOTTOM_RESERVE)
#define ANIM_MAX_FISH FISH_MAX_FISH
#define ANIM_MAX_BUBBLES 12
#define ANIM_MAX_PARTICLES 3

typedef struct {
    float x, y, base_y, target_base_y;
    float prev_x, prev_y;
    float size, vx, swim_vx, swim_angle, wander_t, amp, freq, phase, tilt;
    float range_top, range_bottom;
    int dir, facing;
    bool turning;
    float turn_t, turn_duration;
    int turn_sign;
    float pause_until;
    float pause_prob, amp_mul;
    float feed_delay, peck;
    bool fed_this_round;
    void *seek;
    bool eaten;
    int bubble_level;
    float bubble_until, bubble_cooldown;
    char bubble_text[48];
    uint32_t bubble_color;
    bool bubble_thanks, bubble_feed;
    uint32_t color;
    char name[32];
    lv_img_dsc_t sprite_dsc;
    lv_img_dsc_t sprite_flip_dsc;
    uint8_t *sprite_buf;
    uint8_t *sprite_flip_buf;
    bool has_sprite;
    lv_obj_t *img;
    const lv_img_dsc_t *cached_src;
    lv_coord_t cached_x;
    lv_coord_t cached_y;
} anim_fish_t;

typedef struct {
    lv_img_dsc_t dsc;
    uint8_t *buf;
    float x;
    float y;
    int w;
    int h;
    bool loaded;
} anim_deco_t;

typedef struct {
    float x, y, r, speed;
    float prev_x, prev_y;
    lv_obj_t *dot;
} anim_bubble_t;

typedef struct {
    bool active;
    float x, y, vy, r;
    bool eaten;
    int taken_by;
} anim_particle_t;

typedef struct {
    bool active;
    float x, y, t;
    int64_t start_ms;
} anim_hand_t;

typedef struct {
    bool ready;
    lv_img_dsc_t dsc;
    lv_img_dsc_t flip_dsc;
    uint8_t *buf;
    uint8_t *flip_buf;
} anim_sprite_preload_t;

typedef struct {
    int gen;
    anim_fish_t fishes[ANIM_MAX_FISH];
    int fish_count;
    anim_deco_t decos[FISH_MAX_DECO];
    int deco_count;
    anim_bubble_t bubbles[ANIM_MAX_BUBBLES];
    anim_particle_t particles[ANIM_MAX_PARTICLES];
    anim_hand_t hand;
    bool feed_active;
    int64_t feed_api_cool_until_ms;
    fish_interaction_t interaction;
    fish_tank_state_t *tank;
    float px_per_cm;
    float sprite_norm;
    int frame;
    float step_dt;
    float sim_time;
    int64_t last_tap_ms;
    int64_t last_inv_ms;
    uint16_t *canvas_buf;
    lv_obj_t *canvas;
    lv_coord_t draw_ox;
    lv_coord_t draw_oy;
    lv_coord_t view_h;
    bool has_interaction;
    lv_obj_t *root;
    lv_obj_t *bg_img;
    lv_obj_t *deco_img[FISH_MAX_DECO];
    lv_img_dsc_t bg_dsc;
    uint8_t *bg_buf;
    char bg_src_path[64];
    TaskHandle_t sim_task;
    volatile bool sim_running;
    volatile bool lv_sync_pending;
    bool paused;
    void (*on_interaction)(const char *action, const char *region, void *user);
    void *interaction_user;
    anim_sprite_preload_t sprite_preload[ANIM_MAX_FISH];
    int sprite_preload_count;
} anim_engine_t;

anim_engine_t *anim_engine_create(lv_obj_t *parent);
void anim_engine_destroy(anim_engine_t *eng);
esp_err_t anim_engine_prepare_bg(anim_engine_t *eng, fish_tank_state_t *tank);
esp_err_t anim_engine_prepare_fish(anim_engine_t *eng, fish_tank_state_t *tank);
esp_err_t anim_engine_prepare_assets(anim_engine_t *eng, fish_tank_state_t *tank);
void anim_engine_set_tank(anim_engine_t *eng, fish_tank_state_t *tank);
void anim_engine_set_interaction(anim_engine_t *eng, const fish_interaction_t *it);
void anim_engine_start(anim_engine_t *eng);
void anim_engine_stop(anim_engine_t *eng);
void anim_engine_set_paused(anim_engine_t *eng, bool paused);
void anim_engine_handle_tap(anim_engine_t *eng, int x, int y);
/** Screen-space tap (works even when LVGL indev is stalled). */
void anim_engine_screen_tap(anim_engine_t *eng, int screen_x, int screen_y);
void anim_engine_trigger_feed(anim_engine_t *eng, int x, int y);
void anim_engine_set_interaction_cb(anim_engine_t *eng, void (*cb)(const char *, const char *, void *), void *user);

/** Root LVGL object (for z-order / layout). */
lv_obj_t *anim_engine_get_root(anim_engine_t *eng);

#ifdef __cplusplus
}
#endif
