#include "resource_cache.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cJSON.h"
#include "device_api.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "fish_img.h"
#include "wifi_manager.h"

static const char *TAG = "fish_cache";
static const char *SPIFFS_BASE = "/spiffs";
static const char *META_PATH = "/spiffs/tank_meta.json";

static void parse_tank_state(const char *json, fish_tank_state_t *state, bool local_only);

static const char *json_str_any(cJSON *obj, const char *k1, const char *k2, const char *k3)
{
    if (!obj) {
        return NULL;
    }
    const char *keys[] = {k1, k2, k3};
    for (int i = 0; i < 3; i++) {
        if (!keys[i]) {
            break;
        }
        cJSON *item = cJSON_GetObjectItem(obj, keys[i]);
        if (item && cJSON_IsString(item) && item->valuestring[0]) {
            return item->valuestring;
        }
    }
    return NULL;
}

static int json_int_any(cJSON *obj, int fallback, const char *k1, const char *k2, const char *k3)
{
    if (!obj) {
        return fallback;
    }
    const char *keys[] = {k1, k2, k3};
    for (int i = 0; i < 3; i++) {
        if (!keys[i]) {
            break;
        }
        cJSON *item = cJSON_GetObjectItem(obj, keys[i]);
        if (item && cJSON_IsNumber(item)) {
            return item->valueint;
        }
    }
    return fallback;
}

static void copy_str_field(char *dst, size_t dst_len, const char *src)
{
    if (!dst || dst_len == 0 || !src) {
        return;
    }
    strncpy(dst, src, dst_len - 1);
    dst[dst_len - 1] = '\0';
}
#define FISH_DECO_BIN_MAX 256
#define FISH_CACHE_BUDGET_BYTES (3 * 1024 * 1024)
#define FISH_CACHE_MAX_KEEP 48

static esp_err_t ensure_dir(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0) {
        return ESP_OK;
    }
    return mkdir(path, 0777) == 0 ? ESP_OK : ESP_FAIL;
}

static void cache_path_from_stable_key(const char *prefix, const char *stable_key, char *out, size_t out_len)
{
    uint32_t crc = fish_img_crc32(stable_key ? stable_key : "", stable_key ? strlen(stable_key) : 0);
    snprintf(out, out_len, "/spiffs/%s/%08x", prefix, (unsigned)crc);
}

static void fish_cache_log_spiffs_usage(const char *tag)
{
    size_t total = 0;
    size_t used = 0;
    if (esp_spiffs_info(NULL, &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "SPIFFS %s: used %u / %u bytes", tag ? tag : "status", (unsigned)used, (unsigned)total);
    }
}

static bool path_in_keep_list(const char *path, const char *keep[], int keep_count)
{
    if (!path) {
        return false;
    }
    for (int i = 0; i < keep_count; i++) {
        if (keep[i] && keep[i][0] && strcmp(path, keep[i]) == 0) {
            return true;
        }
    }
    return false;
}

static void fish_cache_gc_dir(const char *dir_path, const char *keep[], int keep_count)
{
    DIR *dir = opendir(dir_path);
    if (!dir) {
        return;
    }
    struct dirent *ent;
    char full[320];
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') {
            continue;
        }
        snprintf(full, sizeof(full), "%s/%s", dir_path, ent->d_name);
        if (!path_in_keep_list(full, keep, keep_count)) {
            if (unlink(full) == 0) {
                ESP_LOGI(TAG, "GC removed %s", full);
            }
        }
    }
    closedir(dir);
}

void fish_cache_gc(const fish_tank_state_t *state)
{
    if (!state) {
        return;
    }
    const char *keep[FISH_CACHE_MAX_KEEP];
    int n = 0;
    if (state->tank.bg_path[0] && n < FISH_CACHE_MAX_KEEP) {
        keep[n++] = state->tank.bg_path;
    }
    if (state->tank.bg_bin_path[0] && n < FISH_CACHE_MAX_KEEP) {
        keep[n++] = state->tank.bg_bin_path;
    }
    for (int i = 0; i < state->fish_count && n + 3 < FISH_CACHE_MAX_KEEP; i++) {
        if (state->fish[i].icon_path[0]) {
            keep[n++] = state->fish[i].icon_path;
        }
        if (state->fish[i].icon_bin_path[0]) {
            keep[n++] = state->fish[i].icon_bin_path;
        }
        if (state->fish[i].icon_path_flip[0]) {
            keep[n++] = state->fish[i].icon_path_flip;
        }
    }
    for (int i = 0; i < state->deco_count && n + 2 < FISH_CACHE_MAX_KEEP; i++) {
        if (state->decos[i].png_path[0]) {
            keep[n++] = state->decos[i].png_path;
        }
        if (state->decos[i].bin_path[0]) {
            keep[n++] = state->decos[i].bin_path;
        }
    }
    fish_cache_gc_dir("/spiffs/i", keep, n);
    fish_cache_gc_dir("/spiffs/bg", keep, n);
    fish_cache_gc_dir("/spiffs/d", keep, n);
    fish_cache_log_spiffs_usage("after GC");
}

static const char *url_file_ext(const char *url)
{
    if (!url || !url[0]) {
        return ".png";
    }
    const char *q = strchr(url, '?');
    const char *end = q ? q : url + strlen(url);
    const char *dot = NULL;
    for (const char *p = url; p < end; p++) {
        if (*p == '.') {
            dot = p;
        }
    }
    if (!dot) {
        return ".png";
    }
    if ((size_t)(end - dot) >= 4 && strncasecmp(dot, ".jpg", 4) == 0) {
        return ".jpg";
    }
    if ((size_t)(end - dot) >= 5 && strncasecmp(dot, ".jpeg", 5) == 0) {
        return ".jpg";
    }
    if ((size_t)(end - dot) >= 4 && strncasecmp(dot, ".png", 4) == 0) {
        return ".png";
    }
    return ".png";
}

static void append_src_bin_paths(const char *src_path, char *bin_path, size_t bin_len, char *flip_path, size_t flip_len)
{
    size_t pn = strlen(src_path);
    const char *ext = strrchr(src_path, '.');
    size_t base_len = pn;
    if (ext && ext > src_path) {
        base_len = (size_t)(ext - src_path);
    }
    snprintf(bin_path, bin_len, "%.*s.bin", (int)base_len, src_path);
    if (flip_path && flip_len > 0) {
        snprintf(flip_path, flip_len, "%.*s_r.bin", (int)base_len, src_path);
    }
}

esp_err_t fish_cache_init(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = SPIFFS_BASE,
        .partition_label = "storage",
        .max_files = 48,
        .format_if_mount_failed = true,
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed %s", esp_err_to_name(ret));
        return ret;
    }
    ensure_dir("/spiffs/i");
    ensure_dir("/spiffs/bg");
    ensure_dir("/spiffs/d");

    size_t total = 0;
    size_t used = 0;
    if (esp_spiffs_info(NULL, &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "SPIFFS mounted: used %u / %u bytes", (unsigned)used, (unsigned)total);
    }
    return ESP_OK;
}

esp_err_t fish_cache_load_local(fish_tank_state_t *state)
{
    if (!state) {
        return ESP_ERR_INVALID_ARG;
    }
    FILE *f = fopen(META_PATH, "r");
    if (!f) {
        return ESP_ERR_NOT_FOUND;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return ESP_FAIL;
    }
    long sz = ftell(f);
    if (sz <= 0 || sz >= (long)sizeof(state->tank_json)) {
        fclose(f);
        return ESP_FAIL;
    }
    rewind(f);
    size_t n = fread(state->tank_json, 1, (size_t)sz, f);
    fclose(f);
    if (n != (size_t)sz) {
        return ESP_FAIL;
    }
    state->tank_json[n] = '\0';
    parse_tank_state(state->tank_json, state, true);
    ESP_LOGI(TAG, "loaded local cache fish=%d", state->fish_count);
    return ESP_OK;
}

static const cJSON *find_deco_meta(cJSON *list, const char *key)
{
    if (!list || !cJSON_IsArray(list) || !key || !key[0]) {
        return NULL;
    }
    cJSON *item;
    cJSON_ArrayForEach(item, list) {
        cJSON *k = cJSON_GetObjectItem(item, "key");
        if (!k) {
            k = cJSON_GetObjectItem(item, "decoKey");
        }
        if (k && cJSON_IsString(k) && strcmp(k->valuestring, key) == 0) {
            return item;
        }
    }
    return NULL;
}

static void parse_decorations(cJSON *tank, fish_tank_state_t *state)
{
    state->deco_count = 0;
    cJSON *deco_arr = tank ? cJSON_GetObjectItem(tank, "decorations") : NULL;
    if (!deco_arr || !cJSON_IsArray(deco_arr)) {
        return;
    }

    cJSON *deco_list = NULL;
    cJSON *deco_root = NULL;
    bool need_meta = false;
    cJSON *item;
    cJSON_ArrayForEach(item, deco_arr) {
        cJSON *url = cJSON_GetObjectItem(item, "imageUrl");
        if (!url || !cJSON_IsString(url) || !url->valuestring[0]) {
            need_meta = true;
            break;
        }
    }
    if (need_meta) {
        char *deco_json = malloc(FISH_API_RESP_MAX);
        if (deco_json && fish_api_deco_list(deco_json, FISH_API_RESP_MAX) == ESP_OK) {
            deco_root = cJSON_Parse(deco_json);
            cJSON *data = deco_root ? cJSON_GetObjectItem(deco_root, "data") : NULL;
            deco_list = data ? cJSON_GetObjectItem(data, "list") : NULL;
        }
        free(deco_json);
    }

    cJSON_ArrayForEach(item, deco_arr) {
        if (state->deco_count >= FISH_MAX_DECO) {
            break;
        }
        fish_deco_item_t *d = &state->decos[state->deco_count];
        memset(d, 0, sizeof(*d));
        d->scale = 1.0f;
        d->default_w_cm = 10.0f;
        d->default_h_cm = 10.0f;

        cJSON *key = cJSON_GetObjectItem(item, "decoKey");
        if (!key) {
            key = cJSON_GetObjectItem(item, "key");
        }
        if (key && cJSON_IsString(key)) {
            strncpy(d->deco_key, key->valuestring, sizeof(d->deco_key) - 1);
        }
        cJSON *url = cJSON_GetObjectItem(item, "imageUrl");
        if (url && cJSON_IsString(url)) {
            strncpy(d->image_url, url->valuestring, sizeof(d->image_url) - 1);
        }
        const cJSON *meta = find_deco_meta(deco_list, d->deco_key);
        if (meta) {
            cJSON *murl = cJSON_GetObjectItem(meta, "imageUrl");
            if ((!d->image_url[0]) && murl && cJSON_IsString(murl)) {
                strncpy(d->image_url, murl->valuestring, sizeof(d->image_url) - 1);
            }
            cJSON *dw = cJSON_GetObjectItem(meta, "defaultW");
            cJSON *dh = cJSON_GetObjectItem(meta, "defaultH");
            if (dw) {
                d->default_w_cm = (float)dw->valuedouble;
            }
            if (dh) {
                d->default_h_cm = (float)dh->valuedouble;
            }
        }
        cJSON *dw = cJSON_GetObjectItem(item, "defaultW");
        cJSON *dh = cJSON_GetObjectItem(item, "defaultH");
        if (dw) {
            d->default_w_cm = (float)dw->valuedouble;
        }
        if (dh) {
            d->default_h_cm = (float)dh->valuedouble;
        }
        d->x = cJSON_GetObjectItem(item, "x") ? (float)cJSON_GetObjectItem(item, "x")->valuedouble : 0.0f;
        d->y = cJSON_GetObjectItem(item, "y") ? (float)cJSON_GetObjectItem(item, "y")->valuedouble : 0.0f;
        d->scale = cJSON_GetObjectItem(item, "scale") ? (float)cJSON_GetObjectItem(item, "scale")->valuedouble : 1.0f;
        d->rotation_deg = cJSON_GetObjectItem(item, "rotation") ? (float)cJSON_GetObjectItem(item, "rotation")->valuedouble : 0.0f;

        if (!d->image_url[0]) {
            continue;
        }
        {
            char stable_key[160];
            snprintf(stable_key, sizeof(stable_key), "%s|%.3f|%.1f|%.1f", d->deco_key, d->scale, d->default_w_cm,
                     d->default_h_cm);
            cache_path_from_stable_key("d", stable_key, d->png_path, sizeof(d->png_path));
        }
        strncat(d->png_path, url_file_ext(d->image_url), sizeof(d->png_path) - strlen(d->png_path) - 1);
        append_src_bin_paths(d->png_path, d->bin_path, sizeof(d->bin_path), NULL, 0);
        fish_img_download_if_needed(d->image_url, d->png_path);
        state->deco_count++;
    }
    cJSON_Delete(deco_root);
}

static void parse_tank_state(const char *json, fish_tank_state_t *state, bool local_only)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        return;
    }
    cJSON *data = cJSON_GetObjectItem(root, "data");
    cJSON *tank = data ? cJSON_GetObjectItem(data, "tank") : NULL;
    if (tank) {
        cJSON *id = cJSON_GetObjectItem(tank, "_id");
        if (id && cJSON_IsString(id)) {
            strncpy(state->tank.id, id->valuestring, sizeof(state->tank.id) - 1);
        }
        cJSON *name = cJSON_GetObjectItem(tank, "name");
        if (name && cJSON_IsString(name)) {
            strncpy(state->tank.name, name->valuestring, sizeof(state->tank.name) - 1);
        }
        state->tank.length_cm = cJSON_GetObjectItem(tank, "length") ? cJSON_GetObjectItem(tank, "length")->valueint : 100;
        state->tank.width_cm = cJSON_GetObjectItem(tank, "width") ? cJSON_GetObjectItem(tank, "width")->valueint : 0;
        state->tank.height_cm = cJSON_GetObjectItem(tank, "height") ? cJSON_GetObjectItem(tank, "height")->valueint : 50;
        state->tank.total_value = json_int_any(tank, 0, "totalValue", "totalPrice", "fishTotalValue");
        state->tank.run_days = json_int_any(tank, 0, "runDays", "tankDays", "days");
        const char *created = json_str_any(tank, "createdAt", "createTime", NULL);
        if (created) {
            copy_str_field(state->tank.created_at, sizeof(state->tank.created_at), created);
        }
        const char *nick = json_str_any(tank, "ownerNickName", "nickName", "ownerNickname");
        if (!nick && data) {
            nick = json_str_any(data, "ownerNickName", "nickName", "userNickName");
        }
        if (!nick && data) {
            cJSON *owner = cJSON_GetObjectItem(data, "owner");
            nick = json_str_any(owner, "nickName", "nickname", "name");
        }
        if (nick) {
            copy_str_field(state->tank.owner_nickname, sizeof(state->tank.owner_nickname), nick);
        }
        cJSON *ua = cJSON_GetObjectItem(tank, "updatedAt");
        if (ua && cJSON_IsString(ua)) {
            strncpy(state->tank.updated_at, ua->valuestring, sizeof(state->tank.updated_at) - 1);
        }
        const char *bg_url = NULL;
        cJSON *bg_img = cJSON_GetObjectItem(tank, "bgImageUrl");
        cJSON *img = cJSON_GetObjectItem(tank, "imageUrl");
        if (bg_img && cJSON_IsString(bg_img) && bg_img->valuestring[0]) {
            bg_url = bg_img->valuestring;
        } else if (img && cJSON_IsString(img) && img->valuestring[0]) {
            bg_url = img->valuestring;
        }
        if (bg_url) {
            strncpy(state->tank.image_url, bg_url, sizeof(state->tank.image_url) - 1);
            {
                char stable_key[128];
                snprintf(stable_key, sizeof(stable_key), "%s|%s", state->tank.id, state->tank.updated_at);
                cache_path_from_stable_key("bg", stable_key, state->tank.bg_path, sizeof(state->tank.bg_path));
            }
            strncat(state->tank.bg_path, url_file_ext(bg_url), sizeof(state->tank.bg_path) - strlen(state->tank.bg_path) - 1);
            append_src_bin_paths(state->tank.bg_path, state->tank.bg_bin_path, sizeof(state->tank.bg_bin_path), NULL, 0);
            if (!local_only) {
                fish_img_download_if_needed(state->tank.image_url, state->tank.bg_path);
            }
        }
        cJSON *inter = cJSON_GetObjectItem(tank, "interaction");
        if (inter) {
            fish_interaction_t *it = &state->interaction;
            it->satiety = cJSON_GetObjectItem(inter, "satiety") ? cJSON_GetObjectItem(inter, "satiety")->valueint : 10;
            it->water_quality = cJSON_GetObjectItem(inter, "waterQuality") ? cJSON_GetObjectItem(inter, "waterQuality")->valueint : 10;
            it->algae_level = cJSON_GetObjectItem(inter, "algaeLevel") ? cJSON_GetObjectItem(inter, "algaeLevel")->valueint : 0;
            cJSON *reg = cJSON_GetObjectItem(inter, "algaeRegions");
            it->algae_left = reg && cJSON_GetObjectItem(reg, "left") ? cJSON_GetObjectItem(reg, "left")->valueint : 0;
            it->algae_mid = reg && cJSON_GetObjectItem(reg, "mid") ? cJSON_GetObjectItem(reg, "mid")->valueint : 0;
            it->algae_right = reg && cJSON_GetObjectItem(reg, "right") ? cJSON_GetObjectItem(reg, "right")->valueint : 0;
        }
    }

    state->fish_count = 0;
    cJSON *list = data ? cJSON_GetObjectItem(data, "fishList") : NULL;
    if (list && cJSON_IsArray(list)) {
        cJSON *item;
        cJSON_ArrayForEach(item, list) {
            if (state->fish_count >= FISH_MAX_FISH) {
                break;
            }
            fish_item_t *f = &state->fish[state->fish_count];
            cJSON *fid = cJSON_GetObjectItem(item, "_id");
            if (fid && cJSON_IsString(fid)) {
                strncpy(f->id, fid->valuestring, sizeof(f->id) - 1);
            }
            cJSON *fname = cJSON_GetObjectItem(item, "name");
            if (fname && cJSON_IsString(fname)) {
                strncpy(f->name, fname->valuestring, sizeof(f->name) - 1);
            }
            cJSON *icon = cJSON_GetObjectItem(item, "iconUrl");
            if (!icon) {
                icon = cJSON_GetObjectItem(item, "icon");
            }
            if (!icon) {
                icon = cJSON_GetObjectItem(item, "imageUrl");
            }
            if (icon && cJSON_IsString(icon)) {
                strncpy(f->icon_url, icon->valuestring, sizeof(f->icon_url) - 1);
            }
            cJSON *ua = cJSON_GetObjectItem(item, "updatedAt");
            if (ua && cJSON_IsString(ua)) {
                strncpy(f->updated_at, ua->valuestring, sizeof(f->updated_at) - 1);
            }
            f->size_cm = cJSON_GetObjectItem(item, "size") ? (float)cJSON_GetObjectItem(item, "size")->valuedouble : 10;
            int fish_days = json_int_any(item, 0, "tankDays", "runDays", "days");
            if (fish_days > state->tank.run_days) {
                state->tank.run_days = fish_days;
            }
            cJSON *layer = cJSON_GetObjectItem(item, "swimLayer");
            if (layer && cJSON_IsString(layer)) {
                strncpy(f->swim_layer, layer->valuestring, sizeof(f->swim_layer) - 1);
            } else {
                strncpy(f->swim_layer, "free", sizeof(f->swim_layer) - 1);
            }
            cJSON *temp = cJSON_GetObjectItem(item, "temperament");
            if (temp && cJSON_IsString(temp)) {
                strncpy(f->temperament, temp->valuestring, sizeof(f->temperament) - 1);
            } else {
                strncpy(f->temperament, "normal", sizeof(f->temperament) - 1);
            }
            if (f->icon_url[0]) {
                char stable_key[128];
                snprintf(stable_key, sizeof(stable_key), "%s|%s", f->id, f->updated_at);
                cache_path_from_stable_key("i", stable_key, f->icon_path, sizeof(f->icon_path));
                strncat(f->icon_path, url_file_ext(f->icon_url), sizeof(f->icon_path) - strlen(f->icon_path) - 1);
                append_src_bin_paths(f->icon_path, f->icon_bin_path, sizeof(f->icon_bin_path), f->icon_path_flip,
                                     sizeof(f->icon_path_flip));
                if (!local_only) {
                    fish_img_download_if_needed(f->icon_url, f->icon_path);
                }
            }
            state->fish_count++;
        }
    }
    parse_decorations(tank, state);
    cJSON_Delete(root);
}

bool fish_cache_needs_update(const fish_tank_state_t *state, const char *new_json)
{
    if (!state || !new_json) {
        return true;
    }
    cJSON *root = cJSON_Parse(new_json);
    if (!root) {
        return true;
    }
    cJSON *data = cJSON_GetObjectItem(root, "data");
    cJSON *tank = data ? cJSON_GetObjectItem(data, "tank") : NULL;
    cJSON *ua = tank ? cJSON_GetObjectItem(tank, "updatedAt") : NULL;
    bool changed = true;
    if (ua && cJSON_IsString(ua) && state->tank.updated_at[0]) {
        changed = strcmp(state->tank.updated_at, ua->valuestring) != 0;
    }
    cJSON *list = data ? cJSON_GetObjectItem(data, "fishList") : NULL;
    if (!changed && list && cJSON_IsArray(list)) {
        int count = cJSON_GetArraySize(list);
        if (count != state->fish_count) {
            changed = true;
        } else {
            int i = 0;
            cJSON *item;
            cJSON_ArrayForEach(item, list) {
                if (i >= state->fish_count) {
                    changed = true;
                    break;
                }
                cJSON *id = cJSON_GetObjectItem(item, "_id");
                cJSON *ua_fish = cJSON_GetObjectItem(item, "updatedAt");
                const char *id_s = id && cJSON_IsString(id) ? id->valuestring : "";
                const char *ua_s = ua_fish && cJSON_IsString(ua_fish) ? ua_fish->valuestring : "";
                if (strcmp(state->fish[i].id, id_s) != 0 || strcmp(state->fish[i].updated_at, ua_s) != 0) {
                    changed = true;
                    break;
                }
                i++;
            }
        }
    }
    cJSON_Delete(root);
    return changed;
}

static bool path_has_payload(const char *path, size_t min_size)
{
    if (!path || !path[0]) {
        return false;
    }
    struct stat st;
    return stat(path, &st) == 0 && (size_t)st.st_size >= min_size;
}

bool fish_cache_assets_local_ready(const fish_tank_state_t *state)
{
    if (!state || state->fish_count <= 0) {
        return false;
    }
    for (int i = 0; i < state->fish_count; i++) {
        const fish_item_t *f = &state->fish[i];
        if (!f->icon_url[0]) {
            continue;
        }
        if (path_has_payload(f->icon_bin_path, 16) || path_has_payload(f->icon_path, 16)) {
            continue;
        }
        ESP_LOGW(TAG, "missing fish asset %s / %s", f->icon_path, f->icon_bin_path);
        return false;
    }
    if (state->tank.image_url[0]) {
        if (!path_has_payload(state->tank.bg_bin_path, 16) && !path_has_payload(state->tank.bg_path, 16)) {
            ESP_LOGW(TAG, "missing bg asset %s", state->tank.bg_path);
            return false;
        }
    }
    return true;
}

void fish_cache_sync_remote_assets(fish_tank_state_t *state)
{
    if (!state) {
        return;
    }
    if (state->tank.image_url[0] && state->tank.bg_path[0]) {
        esp_err_t err = fish_img_download_if_needed(state->tank.image_url, state->tank.bg_path);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "bg download failed %s -> %s", state->tank.image_url, state->tank.bg_path);
        }
    }
    for (int i = 0; i < state->fish_count; i++) {
        fish_item_t *f = &state->fish[i];
        if (!f->icon_url[0] || !f->icon_path[0]) {
            continue;
        }
        esp_err_t err = fish_img_download_if_needed(f->icon_url, f->icon_path);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "fish download failed %s -> %s", f->icon_url, f->icon_path);
        }
    }
}

esp_err_t fish_cache_sync_tank(const char *tank_id, fish_tank_state_t *state)
{
    if (!state) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(state, 0, sizeof(*state));
    esp_err_t err = fish_api_tank_detail(tank_id, state->tank_json, sizeof(state->tank_json));
    if (err != ESP_OK) {
        return err;
    }
    return fish_cache_apply_detail_json(state->tank_json, state);
}

esp_err_t fish_cache_apply_detail_json(const char *json, fish_tank_state_t *state)
{
    if (!state || !json) {
        return ESP_ERR_INVALID_ARG;
    }
    strncpy(state->tank_json, json, sizeof(state->tank_json) - 1);
    state->tank_json[sizeof(state->tank_json) - 1] = '\0';
    parse_tank_state(state->tank_json, state, false);
    fish_cache_sync_remote_assets(state);
    if (fish_cache_assets_local_ready(state)) {
        fish_cache_gc(state);
    } else {
        ESP_LOGW(TAG, "skip GC until fish/bg assets are cached locally");
    }
    FILE *f = fopen(META_PATH, "w");
    if (f) {
        fwrite(state->tank_json, 1, strlen(state->tank_json), f);
        fclose(f);
    }
    return ESP_OK;
}

const char *fish_cache_icon_path(const fish_tank_state_t *state, int fish_idx)
{
    if (!state || fish_idx < 0 || fish_idx >= state->fish_count) {
        return NULL;
    }
    struct stat st;
    if (state->fish[fish_idx].icon_bin_path[0] && stat(state->fish[fish_idx].icon_bin_path, &st) == 0 &&
        st.st_size > 0) {
        return state->fish[fish_idx].icon_bin_path;
    }
    return NULL;
}

const char *fish_cache_icon_path_flip(const fish_tank_state_t *state, int fish_idx)
{
    if (!state || fish_idx < 0 || fish_idx >= state->fish_count) {
        return NULL;
    }
    struct stat st;
    if (stat(state->fish[fish_idx].icon_path_flip, &st) == 0) {
        return state->fish[fish_idx].icon_path_flip;
    }
    return NULL;
}

const char *fish_cache_bg_path(const fish_tank_state_t *state)
{
    return fish_cache_bg_src_path(state);
}

const char *fish_cache_bg_src_path(const fish_tank_state_t *state)
{
    if (!state || !state->tank.bg_path[0]) {
        return NULL;
    }
    struct stat st;
    if (stat(state->tank.bg_path, &st) == 0 && st.st_size > 0) {
        return state->tank.bg_path;
    }
    return NULL;
}

esp_err_t fish_cache_prepare_fish_sprite(const fish_item_t *fish, int target_w)
{
    if (!fish || target_w < 4) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!fish->icon_path[0]) {
        return ESP_ERR_NOT_FOUND;
    }
    struct stat st;
    bool have_png = stat(fish->icon_path, &st) == 0 && st.st_size > 0;
    if (!have_png && !fish_wifi_is_connected()) {
        return ESP_ERR_NOT_FOUND;
    }
    if (fish->icon_url[0] && fish_wifi_is_connected()) {
        fish_img_download_if_needed(fish->icon_url, fish->icon_path);
    }
    esp_err_t err = fish_img_prepare_rgba565a8_fit_width(fish->icon_path, fish->icon_bin_path, target_w, false);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "sprite bin failed %s (%s)", fish->icon_path, esp_err_to_name(err));
        return err;
    }
    err = fish_img_prepare_rgba565a8_fit_width(fish->icon_path, fish->icon_path_flip, target_w, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "sprite flip bin failed %s (%s)", fish->icon_path, esp_err_to_name(err));
    }
    return ESP_OK;
}

esp_err_t fish_cache_prepare_bg(fish_tank_info_t *tank, int target_w, int target_h)
{
    (void)target_w;
    (void)target_h;
    if (!tank || !tank->bg_path[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (tank->image_url[0] && fish_wifi_is_connected()) {
        return fish_img_download_if_needed(tank->image_url, tank->bg_path);
    }
    struct stat st;
    if (stat(tank->bg_path, &st) == 0 && st.st_size > 0) {
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t fish_cache_prepare_deco(const fish_deco_item_t *deco, float px_per_cm, int *out_w, int *out_h)
{
    if (!deco || !deco->png_path[0] || px_per_cm <= 0.0f) {
        return ESP_ERR_INVALID_ARG;
    }
    int tw = (int)(deco->default_w_cm * deco->scale * px_per_cm);
    int th = (int)(deco->default_h_cm * deco->scale * px_per_cm);
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
    if (out_w) {
        *out_w = tw;
    }
    if (out_h) {
        *out_h = th;
    }
    return fish_img_prepare_rgba565a8(deco->png_path, deco->bin_path, tw, th, false);
}

void fish_cache_prepare_assets(fish_tank_state_t *state, int canvas_w, int canvas_h)
{
    if (!state || canvas_w <= 0 || canvas_h <= 0) {
        return;
    }
    size_t total = 0;
    size_t used = 0;
    size_t budget = FISH_CACHE_BUDGET_BYTES;
    if (esp_spiffs_info(NULL, &total, &used) == ESP_OK && total > used) {
        budget = total - used;
        if (budget > FISH_CACHE_BUDGET_BYTES) {
            budget = FISH_CACHE_BUDGET_BYTES;
        }
    }

    if (state->tank.bg_path[0]) {
        fish_cache_prepare_bg(&state->tank, canvas_w, canvas_h);
    }
    float tank_len = state->tank.length_cm > 0 ? (float)state->tank.length_cm : 100.0f;
    float px_per_cm = (float)canvas_w / tank_len;
    int fish_ready = 0;
    for (int i = 0; i < state->fish_count; i++) {
        fish_item_t *fish = &state->fish[i];
        if (!fish->icon_url[0] && !fish->icon_path[0]) {
            continue;
        }
        int sprite_px = fish_sprite_target_px(fish->size_cm * px_per_cm);
        size_t need = (size_t)sprite_px * (size_t)sprite_px * 3 * 2;
        if (need > budget) {
            ESP_LOGW(TAG, "skip fish sprite %d, need %u budget %u", i, (unsigned)need, (unsigned)budget);
            continue;
        }
        if (fish_cache_prepare_fish_sprite(fish, sprite_px) == ESP_OK) {
            budget = budget > need ? budget - need : 0;
            fish_ready++;
        }
    }
    ESP_LOGI(TAG, "prepare_assets fish_sprites=%d/%d wifi=%d", fish_ready, state->fish_count,
             fish_wifi_is_connected() ? 1 : 0);
    for (int i = 0; i < state->deco_count; i++) {
        fish_deco_item_t *deco = &state->decos[i];
        if (!deco->png_path[0]) {
            continue;
        }
        int tw = 0;
        int th = 0;
        if (fish_cache_prepare_deco(deco, px_per_cm, &tw, &th) != ESP_OK) {
            continue;
        }
        size_t need = (size_t)tw * (size_t)th * 3 + 6;
        if (need > budget) {
            ESP_LOGW(TAG, "skip deco %d, need %u budget %u", i, (unsigned)need, (unsigned)budget);
            unlink(deco->bin_path);
            continue;
        }
        budget = budget > need ? budget - need : 0;
    }
}

const char *fish_cache_deco_path(const fish_tank_state_t *state, int deco_idx)
{
    if (!state || deco_idx < 0 || deco_idx >= state->deco_count) {
        return NULL;
    }
    struct stat st;
    if (state->decos[deco_idx].bin_path[0] && stat(state->decos[deco_idx].bin_path, &st) == 0 && st.st_size > 0) {
        return state->decos[deco_idx].bin_path;
    }
    return NULL;
}
