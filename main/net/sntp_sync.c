#include "sntp_sync.h"

#include <sys/time.h>
#include <time.h>

#include "esp_log.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "fish_sntp";
static bool s_synced;
static bool s_inited;
static bool s_fallback;

static void time_sync_notification_cb(struct timeval *tv)
{
    (void)tv;
    s_synced = true;
    s_fallback = false;
    ESP_LOGI(TAG, "NTP synced");
}

static void apply_fallback_time(void)
{
    /* TLS cert window only — HMAC still needs real NTP (±5min). */
    struct timeval tv = {.tv_sec = 1788250000, .tv_usec = 0}; /* ~2026-09-01 */
    settimeofday(&tv, NULL);
    s_fallback = true;
    ESP_LOGW(TAG, "temporary clock for TLS only");
}

bool fish_sntp_is_authoritative(void)
{
    return fish_time_ready() && !s_fallback;
}

bool fish_sntp_sync(int timeout_ms)
{
    ESP_LOGI(TAG, "SNTP sync start (timeout=%dms)", timeout_ms);
    if (!s_inited) {
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        /* Prefer numeric hosts — avoids DNS stalls on esp-hosted. */
        esp_sntp_setservername(0, "203.107.6.88");   /* ntp.aliyun.com */
        esp_sntp_setservername(1, "162.159.200.1");  /* time.cloudflare.com */
        esp_sntp_setservername(2, "ntp.tencent.com");
        esp_sntp_set_time_sync_notification_cb(time_sync_notification_cb);
        esp_sntp_init();
        s_inited = true;
    }
    if (fish_sntp_is_authoritative()) {
        return true;
    }
    s_synced = false;
    int waited = 0;
    while (!fish_sntp_is_authoritative() && waited < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(200));
        waited += 200;
    }
    if (fish_sntp_is_authoritative()) {
        return true;
    }
    if (!fish_time_ready()) {
        apply_fallback_time();
    }
    return false;
}

bool fish_sntp_wait_authoritative(int timeout_ms)
{
    if (fish_sntp_is_authoritative()) {
        return true;
    }
    int waited = 0;
    while (!fish_sntp_is_authoritative() && waited < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(500));
        waited += 500;
    }
    if (fish_sntp_is_authoritative()) {
        ESP_LOGI(TAG, "authoritative NTP ready after +%dms", waited);
        return true;
    }
    return false;
}

int64_t fish_time_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000LL + tv.tv_usec / 1000;
}

bool fish_time_ready(void)
{
    return fish_time_ms() > 1600000000000LL;
}
