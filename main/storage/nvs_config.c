#include "nvs_config.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "nvs_cfg";
static const char *NS = "fishtank";

esp_err_t fish_config_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    return ret;
}

void fish_config_generate_serial(char *out, size_t out_len)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_EFUSE_FACTORY);
    snprintf(out, out_len, "FISH-%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

esp_err_t fish_config_load(fish_config_t *cfg)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(cfg, 0, sizeof(*cfg));

    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        fish_config_generate_serial(cfg->device_id, sizeof(cfg->device_id));
        strncpy(cfg->device_name, "桌面鱼缸终端", sizeof(cfg->device_name) - 1);
#ifdef CONFIG_FISH_APP_KEY
        strncpy(cfg->app_key, CONFIG_FISH_APP_KEY, sizeof(cfg->app_key) - 1);
#endif
#ifdef CONFIG_FISH_APP_SECRET
        strncpy(cfg->app_secret, CONFIG_FISH_APP_SECRET, sizeof(cfg->app_secret) - 1);
#endif
        cfg->temp_high = 26.5f;
        cfg->temp_low = 23.5f;
        return ESP_OK;
    }

    size_t sz;
    sz = sizeof(cfg->wifi_ssid);
    nvs_get_str(h, "wifi_ssid", cfg->wifi_ssid, &sz);
    sz = sizeof(cfg->wifi_pass);
    nvs_get_str(h, "wifi_pass", cfg->wifi_pass, &sz);
    sz = sizeof(cfg->device_id);
    if (nvs_get_str(h, "device_id", cfg->device_id, &sz) != ESP_OK) {
        fish_config_generate_serial(cfg->device_id, sizeof(cfg->device_id));
    }
    sz = sizeof(cfg->device_name);
    nvs_get_str(h, "device_name", cfg->device_name, &sz);
    sz = sizeof(cfg->tank_id);
    nvs_get_str(h, "tank_id", cfg->tank_id, &sz);
    sz = sizeof(cfg->app_key);
    if (nvs_get_str(h, "app_key", cfg->app_key, &sz) != ESP_OK) {
        strncpy(cfg->app_key, CONFIG_FISH_APP_KEY, sizeof(cfg->app_key) - 1);
    }
    sz = sizeof(cfg->app_secret);
    if (nvs_get_str(h, "app_secret", cfg->app_secret, &sz) != ESP_OK) {
        strncpy(cfg->app_secret, CONFIG_FISH_APP_SECRET, sizeof(cfg->app_secret) - 1);
    }
    nvs_get_u8(h, "provisioned", (uint8_t *)&cfg->provisioned);
    size_t blob = sizeof(float);
    if (nvs_get_blob(h, "temp_high", &cfg->temp_high, &blob) != ESP_OK) {
        cfg->temp_high = 26.5f;
    }
    blob = sizeof(float);
    if (nvs_get_blob(h, "temp_low", &cfg->temp_low, &blob) != ESP_OK) {
        cfg->temp_low = 23.5f;
    }
    if (cfg->temp_high <= 0.0f) {
        cfg->temp_high = 26.5f;
    }
    if (cfg->temp_low <= 0.0f) {
        cfg->temp_low = 23.5f;
    }
    if (cfg->device_name[0] == '\0') {
        strncpy(cfg->device_name, "桌面鱼缸终端", sizeof(cfg->device_name) - 1);
    }
    nvs_close(h);
    return ESP_OK;
}

esp_err_t fish_config_save(const fish_config_t *cfg)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    nvs_set_str(h, "wifi_ssid", cfg->wifi_ssid);
    nvs_set_str(h, "wifi_pass", cfg->wifi_pass);
    nvs_set_str(h, "device_id", cfg->device_id);
    nvs_set_str(h, "device_name", cfg->device_name);
    nvs_set_str(h, "tank_id", cfg->tank_id);
    nvs_set_str(h, "app_key", cfg->app_key);
    nvs_set_str(h, "app_secret", cfg->app_secret);
    nvs_set_u8(h, "provisioned", cfg->provisioned ? 1 : 0);
    nvs_set_blob(h, "temp_high", &cfg->temp_high, sizeof(float));
    nvs_set_blob(h, "temp_low", &cfg->temp_low, sizeof(float));
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

bool fish_config_has_wifi(const fish_config_t *cfg)
{
    return cfg && cfg->wifi_ssid[0] != '\0';
}
