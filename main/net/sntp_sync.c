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

static void time_sync_notification_cb(struct timeval *tv)
{
    s_synced = true;
    ESP_LOGI(TAG, "NTP synced");
}

bool fish_sntp_sync(int timeout_ms)
{
    ESP_LOGI(TAG, "SNTP sync start (timeout=%dms)", timeout_ms);
    if (!s_inited) {
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "ntp.aliyun.com");
        esp_sntp_set_time_sync_notification_cb(time_sync_notification_cb);
        esp_sntp_init();
        s_inited = true;
    }
    if (fish_time_ready()) {
        return true;
    }
    s_synced = false;
    int waited = 0;
    while (!s_synced && !fish_time_ready() && waited < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(200));
        waited += 200;
    }
    return fish_time_ready() || s_synced;
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
