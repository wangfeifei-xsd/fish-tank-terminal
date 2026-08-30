#include "wifi_manager.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "fish_wifi";
static EventGroupHandle_t s_wifi_event_group;
static bool s_connected;
static bool s_inited;
static bool s_want_connect;
static bool s_started;

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static fish_wifi_connected_cb_t s_connected_cb;
static void *s_connected_cb_arg;

void fish_wifi_set_connected_cb(fish_wifi_connected_cb_t cb, void *arg)
{
    s_connected_cb = cb;
    s_connected_cb_arg = arg;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        s_started = true;
        if (s_want_connect) {
            esp_wifi_connect();
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        if (s_want_connect) {
            esp_wifi_connect();
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_connected = true;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "WiFi connected");
        if (s_connected_cb) {
            s_connected_cb(s_connected_cb_arg);
        }
    }
}

esp_err_t fish_wifi_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s (C6 not ready?)", esp_err_to_name(err));
        return err;
    }

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(err));
        return err;
    }
    s_inited = true;
    return ESP_OK;
}

esp_err_t fish_wifi_scan(wifi_ap_record_t *out, uint16_t *count)
{
    if (!out || !count || *count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_inited) {
        esp_err_t e = fish_wifi_init();
        if (e != ESP_OK) {
            return e;
        }
    }

    /* Pause auto-reconnect while scanning. */
    bool prev = s_want_connect;
    s_want_connect = false;
    esp_wifi_disconnect();

    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 120,
        .scan_time.active.max = 400,
    };
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        s_want_connect = prev;
        return err;
    }

    uint16_t ap_num = 0;
    err = esp_wifi_scan_get_ap_num(&ap_num);
    if (err != ESP_OK) {
        s_want_connect = prev;
        return err;
    }
    if (ap_num == 0) {
        *count = 0;
        s_want_connect = prev;
        return ESP_OK;
    }

    wifi_ap_record_t *tmp = calloc(ap_num, sizeof(wifi_ap_record_t));
    if (!tmp) {
        s_want_connect = prev;
        return ESP_ERR_NO_MEM;
    }
    uint16_t got = ap_num;
    err = esp_wifi_scan_get_ap_records(&got, tmp);
    if (err != ESP_OK) {
        free(tmp);
        s_want_connect = prev;
        return err;
    }

    /* Deduplicate by SSID, keep strongest RSSI. */
    uint16_t n = 0;
    for (uint16_t i = 0; i < got; i++) {
        if (tmp[i].ssid[0] == '\0') {
            continue;
        }
        int found = -1;
        for (uint16_t j = 0; j < n; j++) {
            if (strncmp((char *)out[j].ssid, (char *)tmp[i].ssid, sizeof(tmp[i].ssid)) == 0) {
                found = (int)j;
                break;
            }
        }
        if (found >= 0) {
            if (tmp[i].rssi > out[found].rssi) {
                out[found] = tmp[i];
            }
        } else if (n < *count) {
            out[n++] = tmp[i];
        }
    }

    /* Sort by RSSI descending (simple insertion). */
    for (uint16_t i = 1; i < n; i++) {
        wifi_ap_record_t key = out[i];
        int j = (int)i - 1;
        while (j >= 0 && out[j].rssi < key.rssi) {
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = key;
    }

    *count = n;
    free(tmp);
    s_want_connect = prev;
    if (prev) {
        esp_err_t rc = esp_wifi_connect();
        if (rc != ESP_OK) {
            ESP_LOGW(TAG, "reconnect after scan failed: %s", esp_err_to_name(rc));
        }
    }
    ESP_LOGI(TAG, "scan found %u unique SSIDs", n);
    return ESP_OK;
}

esp_err_t fish_wifi_connect(const fish_config_t *cfg)
{
    if (!cfg || cfg->wifi_ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_inited) {
        esp_err_t e = fish_wifi_init();
        if (e != ESP_OK) {
            return e;
        }
    }

    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    s_want_connect = true;
    s_connected = false;

    wifi_config_t wcfg = {0};
    memcpy(wcfg.sta.ssid, cfg->wifi_ssid, sizeof(wcfg.sta.ssid) - 1);
    memcpy(wcfg.sta.password, cfg->wifi_pass, sizeof(wcfg.sta.password) - 1);
    wcfg.sta.threshold.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wcfg));
    if (s_started) {
        esp_wifi_disconnect();
        esp_wifi_connect();
    } else {
        ESP_ERROR_CHECK(esp_wifi_start());
    }
    return ESP_OK;
}

bool fish_wifi_is_connected(void)
{
    return s_connected;
}

void fish_wifi_wait_connected(int timeout_ms)
{
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
}
