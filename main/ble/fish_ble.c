#include "fish_ble.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_hosted.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "nvs_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#if __has_include("esp_hosted_bt_host_stack.h")
#include "esp_hosted_bt_host_stack.h"
#define FISH_BLE_HOSTED_V3 1
#else
#define FISH_BLE_HOSTED_V3 0
#endif

static const char *TAG = "fish_ble";

#define SVC_UUID128  0x4b,0x91,0x33,0xc9,0xc5,0x8f,0xcc,0x9e,0x59,0x45,0xb5,0x1f,0x01,0xc2,0xaf,0x4f
#define CHAR_WIFI128 0xa8,0x26,0x1b,0x73,0x61,0xea,0xf5,0xb7,0x88,0x68,0xe1,0x36,0x3e,0x48,0xb5,0xbe
#define CHAR_PARAM128 0x78,0x56,0x34,0x12,0x78,0x56,0x34,0x12,0x78,0x56,0x34,0x12,0xd4,0xc3,0xb2,0xa1

#define RX_BUF_SIZE 512
#define PIN_TIMEOUT_MS 300000
#define PIN_MAX_RETRIES 3
#define PIN_LOCKOUT_MS 1800000
#define BLE_RX_TIMEOUT_MS 5000

static fish_config_t s_cfg;
static bool s_provisioning;
static char s_pin[7];
static int s_pin_retries;
static int64_t s_pin_win_start;
static uint8_t s_own_addr_type;

static uint8_t s_ble_buf[RX_BUF_SIZE];
static uint16_t s_ble_len;
static int64_t s_ble_last_rx;
static portMUX_TYPE s_ble_mux = portMUX_INITIALIZER_UNLOCKED;

static uint16_t s_chr_wifi_handle;
static uint16_t s_chr_param_handle;
static fish_ble_notify_cb_t s_notify_cb;
static void (*s_pin_display_cb)(const char *pin);

static bool is_json_complete(const uint8_t *b, uint16_t l)
{
    if (l < 2 || b[0] != '{') {
        return false;
    }
    int depth = 0;
    bool in_str = false;
    for (uint16_t i = 0; i < l; i++) {
        if (b[i] == '"' && (i == 0 || b[i - 1] != '\\')) {
            in_str = !in_str;
        }
        if (!in_str) {
            if (b[i] == '{') {
                depth++;
            } else if (b[i] == '}') {
                depth--;
            }
        }
    }
    return depth == 0;
}

static void ble_notify_wifi(const char *json)
{
    struct os_mbuf *om = ble_hs_mbuf_from_flat(json, strlen(json));
    if (om) {
        ble_gatts_notify_custom(0, s_chr_wifi_handle, om);
    }
}

static void on_wifi_json(const char *json)
{
    cJSON *doc = cJSON_Parse(json);
    if (!doc) {
        ble_notify_wifi("{\"status\":\"ERROR\",\"msg\":\"Invalid JSON\"}");
        return;
    }

    int64_t now = esp_timer_get_time() / 1000;
    if (now - s_pin_win_start > PIN_TIMEOUT_MS) {
        ble_notify_wifi("{\"status\":\"ERROR\",\"msg\":\"PIN expired, restart device\"}");
        cJSON_Delete(doc);
        return;
    }
    if (s_pin_retries >= PIN_MAX_RETRIES && now - s_pin_win_start < PIN_LOCKOUT_MS) {
        ble_notify_wifi("{\"status\":\"ERROR\",\"msg\":\"Locked 30min\"}");
        cJSON_Delete(doc);
        return;
    }

    cJSON *pin_j = cJSON_GetObjectItem(doc, "pin");
    uint32_t expect = 0;
    for (int i = 0; i < 6; i++) {
        expect = expect * 10 + (s_pin[i] - '0');
    }
    uint32_t input = pin_j ? (uint32_t)pin_j->valuedouble : 0;
    if (input != expect) {
        s_pin_retries++;
        char buf[128];
        snprintf(buf, sizeof(buf), "{\"status\":\"ERROR\",\"msg\":\"PIN mismatch\",\"retriesLeft\":%d}",
                 PIN_MAX_RETRIES - s_pin_retries);
        ble_notify_wifi(buf);
        cJSON_Delete(doc);
        return;
    }

    cJSON *ssid = cJSON_GetObjectItem(doc, "ssid");
    cJSON *pass = cJSON_GetObjectItem(doc, "password");
    if (!ssid || !cJSON_IsString(ssid) || !ssid->valuestring[0]) {
        ble_notify_wifi("{\"status\":\"ERROR\",\"msg\":\"Missing ssid\"}");
        cJSON_Delete(doc);
        return;
    }

    strncpy(s_cfg.wifi_ssid, ssid->valuestring, sizeof(s_cfg.wifi_ssid) - 1);
    if (pass && cJSON_IsString(pass)) {
        strncpy(s_cfg.wifi_pass, pass->valuestring, sizeof(s_cfg.wifi_pass) - 1);
    }

    cJSON *bind_tok = cJSON_GetObjectItem(doc, "bindToken");
    if (bind_tok && cJSON_IsString(bind_tok) && bind_tok->valuestring) {
        const char *tok = bind_tok->valuestring;
        size_t tok_len = strlen(tok);
        bool hex_ok = (tok_len == 32);
        for (size_t i = 0; hex_ok && i < tok_len; i++) {
            char c = tok[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
                hex_ok = false;
            }
        }
        if (hex_ok) {
            strncpy(s_cfg.bind_token, tok, sizeof(s_cfg.bind_token) - 1);
            s_cfg.bind_token[sizeof(s_cfg.bind_token) - 1] = '\0';
            ESP_LOGI(TAG, "bindToken saved");
        } else {
            ESP_LOGW(TAG, "bindToken ignored (expect 32 hex chars)");
        }
    }

    s_cfg.provisioned = true;
    fish_config_save(&s_cfg);
    ble_notify_wifi("{\"status\":\"OK\",\"msg\":\"WiFi saved, restarting...\"}");
    cJSON_Delete(doc);
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
}

static void on_param_json(const char *json)
{
    cJSON *doc = cJSON_Parse(json);
    if (!doc) {
        if (s_notify_cb) {
            s_notify_cb("{\"status\":\"ERROR\",\"msg\":\"Invalid JSON\"}");
        }
        return;
    }

    cJSON *reset = cJSON_GetObjectItem(doc, "resetWiFi");
    if (reset && cJSON_IsTrue(reset)) {
        cJSON *token = cJSON_GetObjectItem(doc, "token");
        if (!token || !cJSON_IsString(token) || strcmp(token->valuestring, s_cfg.device_id) != 0) {
            if (s_notify_cb) {
                s_notify_cb("{\"status\":\"ERROR\",\"msg\":\"Unauthorized: invalid token\"}");
            }
            cJSON_Delete(doc);
            return;
        }
        s_cfg.wifi_ssid[0] = '\0';
        s_cfg.wifi_pass[0] = '\0';
        fish_config_save(&s_cfg);
        if (s_notify_cb) {
            s_notify_cb("{\"status\":\"OK\",\"msg\":\"Resetting WiFi...\"}");
        }
        cJSON_Delete(doc);
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
        return;
    }

    cJSON *name = cJSON_GetObjectItem(doc, "deviceName");
    if (name && cJSON_IsString(name)) {
        strncpy(s_cfg.device_name, name->valuestring, sizeof(s_cfg.device_name) - 1);
        fish_config_save(&s_cfg);
    }
    if (s_notify_cb) {
        s_notify_cb("{\"status\":\"OK\",\"msg\":\"Parameters saved\"}");
    }
    cJSON_Delete(doc);
}

static void accumulate_rx(uint16_t attr_handle, struct os_mbuf *om)
{
    char tmp[RX_BUF_SIZE];
    bool done = false;

    portENTER_CRITICAL(&s_ble_mux);
    uint16_t len = OS_MBUF_PKTLEN(om);
    if (s_ble_len + len <= RX_BUF_SIZE) {
        os_mbuf_copydata(om, 0, len, s_ble_buf + s_ble_len);
        s_ble_len += len;
        s_ble_last_rx = esp_timer_get_time() / 1000;
        done = is_json_complete(s_ble_buf, s_ble_len);
        if (done) {
            s_ble_buf[s_ble_len] = 0;
            memcpy(tmp, s_ble_buf, s_ble_len + 1);
            s_ble_len = 0;
        }
    } else {
        s_ble_len = 0;
    }
    portEXIT_CRITICAL(&s_ble_mux);

    if (done) {
        if (attr_handle == s_chr_wifi_handle) {
            on_wifi_json(tmp);
        } else if (attr_handle == s_chr_param_handle) {
            on_param_json(tmp);
        }
    }
}

static int gatt_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)arg;
    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_READ_CHR:
        if (attr_handle == s_chr_param_handle) {
            char json[256];
            snprintf(json, sizeof(json),
                     "{\"serialNo\":\"%s\",\"deviceName\":\"%s\",\"tempHigh\":%.1f,\"tempLow\":%.1f}",
                     s_cfg.device_id, s_cfg.device_name, s_cfg.temp_high, s_cfg.temp_low);
            os_mbuf_append(ctxt->om, json, strlen(json));
        }
        return 0;
    case BLE_GATT_ACCESS_OP_WRITE_CHR:
        accumulate_rx(attr_handle, ctxt->om);
        return 0;
    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
}

static const ble_uuid128_t svc_uuid = BLE_UUID128_INIT(SVC_UUID128);
static const ble_uuid128_t chr_wifi_uuid = BLE_UUID128_INIT(CHAR_WIFI128);
static const ble_uuid128_t chr_param_uuid = BLE_UUID128_INIT(CHAR_PARAM128);

static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = &chr_wifi_uuid.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_chr_wifi_handle,
            },
            {
                .uuid = &chr_param_uuid.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_chr_param_handle,
            },
            {0},
        },
    },
    {0},
};

static void gen_pin(void)
{
    uint32_t r = esp_random() % 1000000;
    snprintf(s_pin, sizeof(s_pin), "%06lu", (unsigned long)r);
    s_pin_retries = 0;
    s_pin_win_start = esp_timer_get_time() / 1000;
    if (s_pin_display_cb) {
        s_pin_display_cb(s_pin);
    }
}

static void advertise(void)
{
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)s_cfg.device_id;
    fields.name_len = strlen(s_cfg.device_id);
    fields.name_is_complete = 1;
    ble_gap_adv_set_fields(&fields);

    struct ble_hs_adv_fields sr = {0};
    sr.name = (uint8_t *)s_cfg.device_id;
    sr.name_len = strlen(s_cfg.device_id);
    sr.name_is_complete = 1;
    ble_gap_adv_rsp_set_fields(&sr);

    struct ble_gap_adv_params adv = {0};
    adv.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv.disc_mode = BLE_GAP_DISC_MODE_GEN;
    ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &adv, NULL, NULL);
}

static void on_sync(void)
{
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    ble_svc_gap_device_name_set(s_cfg.device_id);
    advertise();
}

static void on_reset(int reason)
{
    ESP_LOGE(TAG, "NimBLE reset %d", reason);
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static int gatt_register(void)
{
    ble_svc_gap_init();
    ble_svc_gatt_init();
    return ble_gatts_count_cfg(gatt_svcs) || ble_gatts_add_svcs(gatt_svcs);
}

esp_err_t fish_ble_init(const fish_config_t *cfg, bool provisioning)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    s_cfg = *cfg;
    s_provisioning = provisioning;
    if (provisioning) {
        gen_pin();
    }

    if (esp_hosted_connect_to_slave() != 0) {
        ESP_LOGE(TAG, "SDIO connect C6 failed (PIN still available for display)");
        return ESP_FAIL;
    }
#if FISH_BLE_HOSTED_V3
    esp_hosted_bt_host_stack_cfg_t bt_cfg = ESP_HOSTED_BT_HOST_STACK_CONFIG_DEFAULT();
    if (esp_hosted_bt_host_stack_setup(&bt_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "hosted BT stack setup failed");
        return ESP_FAIL;
    }
#else
    /* esp_hosted 2.x API used by vendor board firmware */
    esp_err_t ret = esp_hosted_bt_controller_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "BT controller init: %s (continue)", esp_err_to_name(ret));
    }
    ret = esp_hosted_bt_controller_enable();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "BT controller enable: %s (continue)", esp_err_to_name(ret));
    }
#endif

    if (nimble_port_init() != 0) {
        return ESP_FAIL;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 0;
    ble_hs_cfg.sm_sc = 0;
    if (gatt_register() != 0) {
        return ESP_FAIL;
    }
    nimble_port_freertos_init(host_task);
    return ESP_OK;
}

void fish_ble_set_pin_display_cb(void (*cb)(const char *pin))
{
    s_pin_display_cb = cb;
}

void fish_ble_set_notify_cb(fish_ble_notify_cb_t cb)
{
    s_notify_cb = cb;
}

const char *fish_ble_get_pin(void)
{
    return s_pin;
}

void fish_ble_refresh_pin(void)
{
    gen_pin();
}

bool fish_ble_is_provisioning(void)
{
    return s_provisioning;
}

esp_err_t fish_ble_notify_param(const char *json)
{
    if (!json) {
        return ESP_ERR_INVALID_ARG;
    }
    struct os_mbuf *om = ble_hs_mbuf_from_flat(json, strlen(json));
    if (!om) {
        return ESP_FAIL;
    }
    ble_gatts_notify_custom(0, s_chr_param_handle, om);
    return ESP_OK;
}
