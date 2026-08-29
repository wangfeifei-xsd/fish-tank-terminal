/**
 * 桌面鱼缸终端 — ESP32-P4 CB V3.2 + WKS50HD071 (720x1280)
 */
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "anim_engine.h"
#include "ble/fish_ble.h"
#include "device_api.h"
#include "display_port.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "fish_ui.h"
#include "lvgl.h"
#include "nvs_config.h"
#include "resource_cache.h"
#include "sdkconfig.h"
#include "sntp_sync.h"
#include "wifi_manager.h"
#include "wifi_setup.h"

static const char *TAG = "fish_main";

#define FISH_ONLINE_STACK_WORDS  16384
#define FISH_WIFI_SYNC_STACK     16384
#define FISH_SNTP_QUICK_MS       15000

static fish_config_t s_cfg;
static fish_tank_state_t s_tank;
static fish_ui_t *s_ui;
static TaskHandle_t s_boot_cache_task;
static SemaphoreHandle_t s_apply_mu;

typedef struct {
    bool running;
    SemaphoreHandle_t done;
} anim_async_req_t;

static void pin_display_cb(const char *pin)
{
    if (s_ui) {
        fish_ui_set_pin(s_ui, pin);
    }
}

static esp_err_t ensure_tank_bound(fish_config_t *cfg)
{
    if (!cfg || cfg->tank_id[0]) {
        return ESP_OK;
    }
    esp_err_t err = fish_api_fetch_first_tank_id(cfg->tank_id, sizeof(cfg->tank_id));
    if (err == ESP_OK) {
        fish_config_save(cfg);
    }
    return err;
}

static const int s_canvas_h = ANIM_VIEW_H;

static bool apply_tank_ui_locked(uint32_t wait_ms)
{
    if (!s_ui) {
        return false;
    }
    if (!lvgl_port_lock(wait_ms)) {
        ESP_LOGW(TAG, "lvgl lock timeout (%ums)", (unsigned)wait_ms);
        return false;
    }
    fish_ui_set_tank(s_ui, &s_tank);
    if (s_ui->anim) {
        anim_engine_start(s_ui->anim);
    }
    lvgl_port_unlock();
    return true;
}

static void anim_run_async_cb(void *user_data)
{
    anim_async_req_t *req = user_data;
    if (req && s_ui && s_ui->anim) {
        if (req->running) {
            anim_engine_start(s_ui->anim);
        } else {
            anim_engine_stop(s_ui->anim);
        }
    }
    if (req && req->done) {
        xSemaphoreGive(req->done);
    }
    free(req);
}

static void anim_engine_stop_sync(uint32_t wait_ms)
{
    if (!s_ui || !s_ui->anim) {
        return;
    }
    if (lvgl_port_lock(wait_ms)) {
        anim_engine_stop(s_ui->anim);
        lvgl_port_unlock();
        return;
    }
    anim_async_req_t *req = calloc(1, sizeof(*req));
    SemaphoreHandle_t done = NULL;
    if (req) {
        done = xSemaphoreCreateBinary();
        if (done) {
            req->done = done;
            lv_async_call(anim_run_async_cb, req);
            if (xSemaphoreTake(done, pdMS_TO_TICKS(wait_ms)) != pdTRUE) {
                ESP_LOGW(TAG, "anim stop async timeout (%ums)", (unsigned)wait_ms);
            }
            vSemaphoreDelete(done);
        } else {
            free(req);
        }
    }
}

static void apply_tank_to_ui(void)
{
    if (s_apply_mu && xSemaphoreTake(s_apply_mu, pdMS_TO_TICKS(30000)) != pdTRUE) {
        ESP_LOGW(TAG, "apply_tank busy, skip");
        return;
    }

    anim_engine_stop_sync(15000);

    UBaseType_t prio = uxTaskPriorityGet(NULL);
    vTaskPrioritySet(NULL, 3);
    bool wifi = fish_wifi_is_connected();
    if (wifi) {
        fish_cache_sync_remote_assets(&s_tank);
    }
    fish_cache_prepare_assets(&s_tank, CONFIG_FISH_LOGICAL_WIDTH, s_canvas_h);
    if (s_ui && s_ui->anim) {
        anim_engine_prepare_bg(s_ui->anim, &s_tank);
        anim_engine_prepare_fish(s_ui->anim, &s_tank);
    }
    vTaskPrioritySet(NULL, prio);

    if (apply_tank_ui_locked(15000)) {
        ESP_LOGI(TAG, "tank applied (wifi=%d)", wifi ? 1 : 0);
    }

    if (s_apply_mu) {
        xSemaphoreGive(s_apply_mu);
    }
}

static void wait_boot_cache_task(void)
{
    while (s_boot_cache_task != NULL) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static void on_wifi_connected(void *arg)
{
    (void)arg;
    ESP_LOGD(TAG, "WiFi got IP (online_services will sync)");
}

static void sync_tank_from_api(void)
{
    char *detail_json = malloc(FISH_API_RESP_MAX);
    if (!detail_json) {
        ESP_LOGE(TAG, "detail_json alloc failed");
        return;
    }

    if (fish_api_tank_detail(s_cfg.tank_id, detail_json, FISH_API_RESP_MAX) == ESP_OK) {
        bool need_apply = false;
        if (fish_cache_needs_update(&s_tank, detail_json) || !fish_cache_assets_local_ready(&s_tank)) {
            if (fish_cache_apply_detail_json(detail_json, &s_tank) != ESP_OK) {
                ESP_LOGW(TAG, "apply detail json failed");
            } else {
                need_apply = true;
            }
        } else {
            ESP_LOGI(TAG, "tank meta unchanged");
        }
        if (need_apply) {
            apply_tank_to_ui();
        }
    } else if (fish_cache_sync_tank(s_cfg.tank_id, &s_tank) == ESP_OK) {
        apply_tank_to_ui();
    } else {
        ESP_LOGW(TAG, "tank detail/sync failed");
    }
    free(detail_json);
}

static void poll_task(void *arg)
{
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000));
        if (!fish_wifi_is_connected()) {
            continue;
        }
        if (!s_cfg.tank_id[0]) {
            if (ensure_tank_bound(&s_cfg) == ESP_OK && s_ui) {
                if (fish_cache_sync_tank(s_cfg.tank_id, &s_tank) == ESP_OK) {
                    apply_tank_to_ui();
                }
            }
            continue;
        }

        fish_temp_info_t temp;
        if (fish_api_temp_latest(&temp) == ESP_OK && s_ui) {
            if (lvgl_port_lock(0)) {
                fish_ui_set_temp(s_ui, temp.temperature1, temp.data_stale);
                lvgl_port_unlock();
            }
        }
#if CONFIG_FISH_AUTO_POLL
        char *detail_json = malloc(FISH_API_RESP_MAX);
        if (!detail_json) {
            continue;
        }
        if (fish_api_tank_detail(s_cfg.tank_id, detail_json, FISH_API_RESP_MAX) == ESP_OK &&
            fish_cache_needs_update(&s_tank, detail_json) && s_ui) {
            if (fish_cache_apply_detail_json(detail_json, &s_tank) == ESP_OK) {
                apply_tank_to_ui();
            }
        }
        free(detail_json);
#endif
    }
}

static void boot_cache_task(void *arg)
{
    (void)arg;
    if (fish_cache_load_local(&s_tank) == ESP_OK) {
        ESP_LOGI(TAG, "showing cached tank while network sync runs");
        apply_tank_to_ui();
    }
    s_boot_cache_task = NULL;
    vTaskDelete(NULL);
}

static void online_services_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "online_services: start");

    fish_wifi_wait_connected(30000);
    if (!fish_wifi_is_connected()) {
        ESP_LOGW(TAG, "online_services: WiFi timeout");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "online_services: WiFi up");

    wait_boot_cache_task();
    if (fish_wifi_is_connected() && !fish_cache_assets_local_ready(&s_tank)) {
        ESP_LOGI(TAG, "online_services: sync missing assets");
        apply_tank_to_ui();
    } else {
        ESP_LOGI(TAG, "online_services: local assets ready, skip redundant apply");
    }

    if (!fish_sntp_sync(FISH_SNTP_QUICK_MS)) {
        ESP_LOGW(TAG, "online_services: time not synced, API auth may fail");
    } else {
        ESP_LOGI(TAG, "online_services: time ready");
    }

    ESP_LOGI(TAG, "online_services: API bind");
    esp_err_t bind_err = fish_api_bind(s_cfg.device_id, s_cfg.device_name);
    if (bind_err != ESP_OK) {
        ESP_LOGW(TAG, "online_services: bind failed %s", esp_err_to_name(bind_err));
    }

    if (ensure_tank_bound(&s_cfg) != ESP_OK) {
        ESP_LOGW(TAG, "online_services: no tank bound");
    }

    if (s_cfg.tank_id[0]) {
        ESP_LOGI(TAG, "online_services: sync tank %s", s_cfg.tank_id);
        sync_tank_from_api();
    } else if (s_ui) {
        if (lvgl_port_lock(0)) {
            fish_ui_show_status(s_ui, "未获取鱼缸数据");
            lvgl_port_unlock();
        }
    }

    ESP_LOGI(TAG, "online_services: start poll task");
    if (xTaskCreate(poll_task, "poll", 8192, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "online_services: poll task create failed");
    }
    ESP_LOGI(TAG, "online_services: done");
    vTaskDelete(NULL);
}

static void start_online_services(void)
{
    if (xTaskCreate(online_services_task, "fish_online", FISH_ONLINE_STACK_WORDS, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "fish_online task create failed (stack=%u words)",
                 (unsigned)FISH_ONLINE_STACK_WORDS);
    }
}

static void app_task(void *arg)
{
    (void)arg;
    s_apply_mu = xSemaphoreCreateMutex();
    fish_wifi_set_connected_cb(on_wifi_connected, NULL);

    ESP_ERROR_CHECK(fish_config_init());
    fish_config_load(&s_cfg);
    if (strcmp(s_cfg.tank_id, "mock_tank_01") == 0) {
        s_cfg.tank_id[0] = '\0';
        fish_config_save(&s_cfg);
        ESP_LOGI(TAG, "cleared mock tank_id, will refresh from API");
    }
    if (s_cfg.device_id[0] == '\0') {
        fish_config_generate_serial(s_cfg.device_id, sizeof(s_cfg.device_id));
        fish_config_save(&s_cfg);
    }

    ESP_ERROR_CHECK(fish_cache_init());
    fish_api_init(&s_cfg);

#ifdef CONFIG_FISH_DEV_WIFI_SSID
    if (CONFIG_FISH_DEV_WIFI_SSID[0] != '\0' && s_cfg.wifi_ssid[0] == '\0') {
        strncpy(s_cfg.wifi_ssid, CONFIG_FISH_DEV_WIFI_SSID, sizeof(s_cfg.wifi_ssid) - 1);
#ifdef CONFIG_FISH_DEV_WIFI_PASS
        strncpy(s_cfg.wifi_pass, CONFIG_FISH_DEV_WIFI_PASS, sizeof(s_cfg.wifi_pass) - 1);
#endif
        s_cfg.provisioned = true;
        fish_config_save(&s_cfg);
        ESP_LOGI(TAG, "Loaded DEV WiFi SSID=%s", s_cfg.wifi_ssid);
    }
#endif

    fish_display_init();
    if (fish_wifi_init() != ESP_OK) {
        ESP_LOGW(TAG, "fish_wifi_init failed (C6 may be unavailable)");
    } else {
        ESP_LOGI(TAG, "If BLE/RPC timeouts persist, run: scripts/flash_c6_slave.sh");
    }

    bool provisioning = !fish_config_has_wifi(&s_cfg);

    fish_ble_set_pin_display_cb(pin_display_cb);
    fish_ble_init(&s_cfg, provisioning);

    if (provisioning) {
        ESP_LOGI(TAG, "entering on-screen WiFi setup (BLE PIN=%s)", fish_ble_get_pin());
        fish_wifi_setup_run(&s_cfg);
        provisioning = !fish_config_has_wifi(&s_cfg);
    }

    if (lvgl_port_lock(0)) {
        s_ui = fish_ui_create(&s_cfg, provisioning);
        lvgl_port_unlock();
    }
    if (provisioning && s_ui) {
        const char *pin = fish_ble_get_pin();
        if (pin && pin[0]) {
            fish_ui_set_pin(s_ui, pin);
        }
    }

#if CONFIG_FISH_MOCK_API
    if (s_ui && (provisioning || s_cfg.tank_id[0] == '\0')) {
        if (s_cfg.tank_id[0] == '\0') {
            strncpy(s_cfg.tank_id, "mock_tank_01", sizeof(s_cfg.tank_id) - 1);
        }
        if (fish_cache_sync_tank(s_cfg.tank_id, &s_tank) == ESP_OK) {
            fish_cache_prepare_assets(&s_tank, CONFIG_FISH_LOGICAL_WIDTH, s_canvas_h);
            if (s_ui && s_ui->anim) {
                anim_engine_prepare_assets(s_ui->anim, &s_tank);
            }
            if (lvgl_port_lock(0)) {
                fish_ui_set_tank(s_ui, &s_tank);
                lvgl_port_unlock();
            }
        }
    }
#endif

    if (!provisioning) {
        if (xTaskCreate(boot_cache_task, "boot_cache", FISH_WIFI_SYNC_STACK, NULL, 4, &s_boot_cache_task) != pdPASS) {
            ESP_LOGE(TAG, "boot_cache task create failed");
            s_boot_cache_task = NULL;
        }
        fish_wifi_connect(&s_cfg);
        start_online_services();
    }

    ESP_LOGI(TAG, "Fish tank terminal ready (provision=%d)", provisioning);
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_main(void)
{
    xTaskCreate(app_task, "fish_app", 24576, NULL, 5, NULL);
}
