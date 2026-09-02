#include "device_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/md.h"
#include "sdkconfig.h"
#include "sntp_sync.h"

static const char *TAG = "fish_api";
static fish_config_t s_cfg;
static SemaphoreHandle_t s_http_mu;

static esp_err_t hmac_sha256_hex(const char *msg, const char *key, char *hex_out, size_t hex_len)
{
    unsigned char out[32];
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!info) {
        return ESP_FAIL;
    }
    if (mbedtls_md_hmac(info, (const unsigned char *)key, strlen(key),
                        (const unsigned char *)msg, strlen(msg), out) != 0) {
        return ESP_FAIL;
    }
    for (int i = 0; i < 32 && (size_t)(i * 2 + 2) <= hex_len; i++) {
        snprintf(hex_out + i * 2, 3, "%02x", out[i]);
    }
    return ESP_OK;
}

static esp_err_t signed_request(const char *method, const char *sign_path, const char *url_path,
                                const char *body, char *resp, size_t resp_len, int *http_code)
{
#if CONFIG_FISH_MOCK_API
    (void)method;
    (void)sign_path;
    (void)url_path;
    (void)body;
    if (http_code) {
        *http_code = 200;
    }
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (s_http_mu && xSemaphoreTake(s_http_mu, pdMS_TO_TICKS(60000)) != pdTRUE) {
        ESP_LOGW(TAG, "http busy timeout %s", sign_path ? sign_path : "?");
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t ret = ESP_FAIL;
    char url[256];
    snprintf(url, sizeof(url), "%s%s", CONFIG_FISH_API_HOST, url_path);

    int64_t ts = fish_time_ms();
    char sign_str[256];
    snprintf(sign_str, sizeof(sign_str), "%s.%lld.%s.%s", s_cfg.app_key, (long long)ts, method, sign_path);

    char signature[65] = {0};
    esp_err_t herr = hmac_sha256_hex(sign_str, s_cfg.app_secret, signature, sizeof(signature));
    if (herr != ESP_OK) {
        ret = herr;
        goto out_unlock;
    }

    char ts_hdr[24];
    snprintf(ts_hdr, sizeof(ts_hdr), "%lld", (long long)ts);

    esp_http_client_config_t config = {
        .url = url,
        .method = (strcmp(method, "POST") == 0) ? HTTP_METHOD_POST : HTTP_METHOD_GET,
        .timeout_ms = 45000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ret = ESP_FAIL;
        goto out_unlock;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "X-App-Key", s_cfg.app_key);
    esp_http_client_set_header(client, "X-Timestamp", ts_hdr);
    esp_http_client_set_header(client, "X-Signature", signature);

    esp_err_t err;
    const size_t body_len = (body && body[0]) ? strlen(body) : 0;
    if (body_len > 0) {
        err = esp_http_client_open(client, body_len);
        if (err != ESP_OK) {
            esp_http_client_cleanup(client);
            ret = err;
            goto out_unlock;
        }
        int written = esp_http_client_write(client, body, body_len);
        if (written < 0 || (size_t)written != body_len) {
            esp_http_client_cleanup(client);
            ret = ESP_FAIL;
            goto out_unlock;
        }
    } else {
        err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            esp_http_client_cleanup(client);
            ret = err;
            goto out_unlock;
        }
    }

    int content = esp_http_client_fetch_headers(client);
    int code = esp_http_client_get_status_code(client);
    if (http_code) {
        *http_code = code;
    }
    int read_total = 0;
    if (content < 0 && code <= 0) {
        ESP_LOGW(TAG, "HTTP headers/TLS failed %s (content=%d)", sign_path, content);
        esp_http_client_cleanup(client);
        ret = ESP_FAIL;
        goto out_unlock;
    }
    if (content > 0) {
        if ((size_t)content >= resp_len) {
            ESP_LOGW(TAG, "response too large %s %d >= %zu", sign_path, content, resp_len);
            esp_http_client_cleanup(client);
            ret = ESP_ERR_NO_MEM;
            goto out_unlock;
        }
        while (read_total < content) {
            int r = esp_http_client_read(client, resp + read_total, content - read_total);
            if (r <= 0) {
                break;
            }
            read_total += r;
        }
        if (read_total < content) {
            ESP_LOGW(TAG, "HTTP short read %s %d/%d", sign_path, read_total, content);
        }
    } else {
        while (read_total < (int)resp_len - 1) {
            int r = esp_http_client_read(client, resp + read_total, (int)resp_len - 1 - read_total);
            if (r <= 0) {
                break;
            }
            read_total += r;
        }
    }
    resp[read_total] = '\0';
    esp_http_client_cleanup(client);
    if (code == 401) {
        ESP_LOGW(TAG, "HTTP 401 auth failed %s %s", sign_path, resp[0] ? resp : "(empty)");
    } else if (code < 200 || code >= 300) {
        ESP_LOGW(TAG, "HTTP %d %s %s", code, sign_path, resp[0] ? resp : "(empty)");
    } else if (read_total <= 0) {
        ESP_LOGW(TAG, "HTTP %d empty body %s", code, sign_path);
    }
    /* Reject short/empty bodies even with 2xx — truncated JSON must not be applied. */
    ret = (code >= 200 && code < 300 && read_total > 0 && (content <= 0 || read_total >= content))
              ? ESP_OK
              : ESP_FAIL;
out_unlock:
    if (s_http_mu) {
        xSemaphoreGive(s_http_mu);
    }
    return ret;
#endif
}

static bool url_path_char_safe(unsigned char c)
{
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
        return true;
    }
    /* Keep RFC 3986 reserved URL characters intact. In particular, encoding
     * '?' and '&' breaks temporary signed image URLs and their query fields. */
    return c == '-' || c == '_' || c == '.' || c == '~' || c == '/' || c == '?' || c == '&' || c == '=' ||
           c == '%' || c == '#' || c == ':' || c == '@' || c == '!' || c == '$' || c == '\'' || c == '(' ||
           c == ')' || c == '*' || c == '+' || c == ',' || c == ';';
}

static esp_err_t fish_api_url_encode(const char *url, char *out, size_t out_len)
{
    if (!url || !out || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    const char *path = strstr(url, "://");
    if (!path) {
        strncpy(out, url, out_len - 1);
        out[out_len - 1] = '\0';
        return ESP_OK;
    }
    path = strchr(path + 3, '/');
    if (!path) {
        strncpy(out, url, out_len - 1);
        out[out_len - 1] = '\0';
        return ESP_OK;
    }
    size_t prefix_len = (size_t)(path - url);
    if (prefix_len >= out_len) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(out, url, prefix_len);
    size_t pos = prefix_len;
    for (; *path; path++) {
        unsigned char c = (unsigned char)*path;
        if (url_path_char_safe(c)) {
            if (pos + 1 >= out_len) {
                return ESP_ERR_NO_MEM;
            }
            out[pos++] = (char)c;
        } else {
            if (pos + 3 >= out_len) {
                return ESP_ERR_NO_MEM;
            }
            static const char hex[] = "0123456789ABCDEF";
            out[pos++] = '%';
            out[pos++] = hex[c >> 4];
            out[pos++] = hex[c & 0x0F];
        }
    }
    out[pos] = '\0';
    return ESP_OK;
}

static void mock_tank_list(char *resp, size_t len)
{
    snprintf(resp, len,
             "{\"code\":0,\"data\":{\"list\":[{\"_id\":\"mock_tank_01\",\"name\":\"测试海缸\","
             "\"imageUrl\":\"\",\"length\":120,\"width\":60,\"height\":55,"
             "\"interaction\":{\"algaeLevel\":2,\"algaeRegions\":{\"left\":2,\"mid\":1,\"right\":0},"
             "\"satiety\":6,\"waterQuality\":8},\"updatedAt\":\"2026-08-27T10:00:00Z\"}]}}");
}

static void mock_tank_detail(char *resp, size_t len)
{
    snprintf(resp, len,
             "{\"code\":0,\"data\":{\"tank\":{\"_id\":\"mock_tank_01\",\"name\":\"测试海缸\","
             "\"length\":120,\"width\":60,\"height\":55,\"totalValue\":12800,\"runDays\":128,"
             "\"ownerNickName\":\"测试用户\",\"imageUrl\":\"\",\"decorations\":[],"
             "\"interaction\":{\"algaeLevel\":2,\"algaeRegions\":{\"left\":2,\"mid\":1,\"right\":0},"
             "\"satiety\":6,\"waterQuality\":8},\"updatedAt\":\"2026-08-27T10:00:00Z\"},"
             "\"fishList\":[{\"_id\":\"fish_01\",\"name\":\"小丑鱼\",\"iconUrl\":\"\","
             "\"size\":8,\"swimLayer\":\"free\",\"temperament\":\"active\",\"updatedAt\":\"2026-08-27T10:00:00Z\"},"
             "{\"_id\":\"fish_02\",\"name\":\"蓝吊\",\"iconUrl\":\"\",\"size\":12,\"swimLayer\":\"middle_top\","
             "\"temperament\":\"normal\",\"updatedAt\":\"2026-08-27T10:00:00Z\"}]}}");
}

static void mock_interaction_json(char *resp, size_t len, const fish_interaction_t *it)
{
    snprintf(resp, len,
             "{\"code\":0,\"data\":{\"interaction\":{\"satiety\":%d,\"waterQuality\":%d,"
             "\"algaeLevel\":%d,\"algaeRegions\":{\"left\":%d,\"mid\":%d,\"right\":%d}}}}",
             it->satiety, it->water_quality, it->algae_level, it->algae_left, it->algae_mid, it->algae_right);
}

esp_err_t fish_api_init(const fish_config_t *cfg)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    s_cfg = *cfg;
    if (!s_http_mu) {
        s_http_mu = xSemaphoreCreateMutex();
    }
    return ESP_OK;
}

esp_err_t fish_api_parse_interaction(const char *json, fish_interaction_t *out)
{
    if (!json || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        return ESP_FAIL;
    }
    cJSON *data = cJSON_GetObjectItem(root, "data");
    cJSON *it = data ? cJSON_GetObjectItem(data, "interaction") : cJSON_GetObjectItem(root, "interaction");
    if (!it) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }
    out->satiety = cJSON_GetObjectItem(it, "satiety") ? cJSON_GetObjectItem(it, "satiety")->valueint : 0;
    out->water_quality = cJSON_GetObjectItem(it, "waterQuality") ? cJSON_GetObjectItem(it, "waterQuality")->valueint : 10;
    out->algae_level = cJSON_GetObjectItem(it, "algaeLevel") ? cJSON_GetObjectItem(it, "algaeLevel")->valueint : 0;
    cJSON *reg = cJSON_GetObjectItem(it, "algaeRegions");
    out->algae_left = reg && cJSON_GetObjectItem(reg, "left") ? cJSON_GetObjectItem(reg, "left")->valueint : 0;
    out->algae_mid = reg && cJSON_GetObjectItem(reg, "mid") ? cJSON_GetObjectItem(reg, "mid")->valueint : 0;
    out->algae_right = reg && cJSON_GetObjectItem(reg, "right") ? cJSON_GetObjectItem(reg, "right")->valueint : 0;
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t fish_api_health(void)
{
#if CONFIG_FISH_MOCK_API
    return ESP_OK;
#else
    char resp[128];
    int code = 0;
    return signed_request("GET", "/health", "/health", "", resp, sizeof(resp), &code);
#endif
}

esp_err_t fish_api_tank_list(char *json_out, size_t out_len)
{
#if CONFIG_FISH_MOCK_API
    mock_tank_list(json_out, out_len);
    return ESP_OK;
#else
    int code = 0;
    return signed_request("GET", "/api/tank/list", "/api/tank/list", "", json_out, out_len, &code);
#endif
}

bool fish_api_pick_first_tank_id(const char *list_json, char *tank_id, size_t tank_id_len)
{
    if (!list_json || !tank_id || tank_id_len == 0) {
        return false;
    }
    cJSON *root = cJSON_Parse(list_json);
    if (!root) {
        return false;
    }
    cJSON *code = cJSON_GetObjectItem(root, "code");
    if (code && cJSON_IsNumber(code) && code->valueint != 0 && code->valueint != 200) {
        cJSON_Delete(root);
        return false;
    }
    cJSON *data = cJSON_GetObjectItem(root, "data");
    cJSON *list = data ? cJSON_GetObjectItem(data, "list") : NULL;
    if (!list || !cJSON_IsArray(list) || cJSON_GetArraySize(list) == 0) {
        cJSON_Delete(root);
        return false;
    }
    cJSON *first = cJSON_GetArrayItem(list, 0);
    cJSON *id = first ? cJSON_GetObjectItem(first, "_id") : NULL;
    if (!id || !cJSON_IsString(id) || !id->valuestring[0]) {
        id = first ? cJSON_GetObjectItem(first, "tankId") : NULL;
    }
    bool ok = id && cJSON_IsString(id) && id->valuestring[0];
    if (ok) {
        strncpy(tank_id, id->valuestring, tank_id_len - 1);
        tank_id[tank_id_len - 1] = '\0';
    }
    cJSON_Delete(root);
    return ok;
}

esp_err_t fish_api_fetch_first_tank_id(char *tank_id, size_t tank_id_len)
{
    if (!tank_id || tank_id_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    char *list_json = malloc(FISH_API_RESP_MAX);
    if (!list_json) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < 3; attempt++) {
        if (fish_api_tank_list(list_json, FISH_API_RESP_MAX) == ESP_OK &&
            fish_api_pick_first_tank_id(list_json, tank_id, tank_id_len)) {
            ESP_LOGI(TAG, "tank list ok, tank_id=%s", tank_id);
            err = ESP_OK;
            break;
        }
        ESP_LOGW(TAG, "tank list empty or failed (attempt %d)", attempt + 1);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    free(list_json);
    return err;
}

esp_err_t fish_api_tank_detail(const char *tank_id, char *json_out, size_t out_len)
{
#if CONFIG_FISH_MOCK_API
    (void)tank_id;
    mock_tank_detail(json_out, out_len);
    return ESP_OK;
#else
    char path[128];
    snprintf(path, sizeof(path), "/api/tank/detail?tankId=%s", tank_id ? tank_id : "");
    /* Large JSON (~13KB) + WiFi/SDIO jitter → occasional TLS EOF (-0x0087). Retry. */
    esp_err_t err = ESP_FAIL;
    for (int attempt = 1; attempt <= 3; attempt++) {
        int code = 0;
        err = signed_request("GET", "/api/tank/detail", path, "", json_out, out_len, &code);
        if (err == ESP_OK) {
            if (attempt > 1) {
                ESP_LOGI(TAG, "tank detail OK on try %d", attempt);
            }
            return ESP_OK;
        }
        ESP_LOGW(TAG, "tank detail try %d/3 failed code=%d", attempt, code);
        vTaskDelay(pdMS_TO_TICKS(500 * attempt));
    }
    return err;
#endif
}

/* Interaction JSON is tiny; avoid FISH_API_RESP_MAX (16KB) on task stack. */
#define FISH_INTERACTION_RESP_MAX 2048

esp_err_t fish_api_feed(const char *tank_id, fish_interaction_t *out)
{
    char body[128];
    snprintf(body, sizeof(body), "{\"tankId\":\"%s\"}", tank_id ? tank_id : "");
#if CONFIG_FISH_MOCK_API
    char resp[FISH_INTERACTION_RESP_MAX];
    if (out) {
        out->satiety = 8;
        out->water_quality = 8;
        out->algae_level = 2;
        out->algae_left = 2;
        out->algae_mid = 1;
        out->algae_right = 0;
    }
    mock_interaction_json(resp, sizeof(resp), out);
    return ESP_OK;
#else
    char *resp = malloc(FISH_INTERACTION_RESP_MAX);
    if (!resp) {
        return ESP_ERR_NO_MEM;
    }
    int code = 0;
    esp_err_t err = signed_request("POST", "/api/tank/feed", "/api/tank/feed", body, resp, FISH_INTERACTION_RESP_MAX, &code);
    if (err == ESP_OK && out) {
        fish_api_parse_interaction(resp, out);
    }
    free(resp);
    return err;
#endif
}

esp_err_t fish_api_temp_latest(fish_temp_info_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
#if CONFIG_FISH_MOCK_API
    out->temperature1 = 26.3f;
    out->temperature2 = 25.8f;
    out->sensor_count = 2;
    out->data_stale = false;
    out->stale_minutes = 0;
    return ESP_OK;
#else
    char resp[512];
    int code = 0;
    esp_err_t err = signed_request("GET", "/api/temp/latest", "/api/temp/latest", "", resp, sizeof(resp), &code);
    if (err != ESP_OK) {
        out->temperature1 = 26.3f;
        out->data_stale = true;
        return ESP_OK;
    }
    cJSON *root = cJSON_Parse(resp);
    cJSON *data = root ? cJSON_GetObjectItem(root, "data") : NULL;
    if (data) {
        cJSON *t1 = cJSON_GetObjectItem(data, "temperature1");
        if (!t1) {
            t1 = cJSON_GetObjectItem(data, "temperature");
        }
        out->temperature1 = t1 ? (float)t1->valuedouble : 0;
        out->temperature2 = cJSON_GetObjectItem(data, "temperature2") ? (float)cJSON_GetObjectItem(data, "temperature2")->valuedouble : 0;
        out->sensor_count = cJSON_GetObjectItem(data, "sensorCount") ? cJSON_GetObjectItem(data, "sensorCount")->valueint : 1;
    }
    out->data_stale = cJSON_GetObjectItem(root, "dataStale") ? cJSON_IsTrue(cJSON_GetObjectItem(root, "dataStale")) : false;
    out->stale_minutes = cJSON_GetObjectItem(root, "staleMinutes") ? cJSON_GetObjectItem(root, "staleMinutes")->valueint : 0;
    cJSON_Delete(root);
    return ESP_OK;
#endif
}

esp_err_t fish_api_deco_list(char *json_out, size_t out_len)
{
#if CONFIG_FISH_MOCK_API
    snprintf(json_out, out_len, "{\"code\":0,\"data\":{\"list\":[]}}");
    return ESP_OK;
#else
    int code = 0;
    return signed_request("GET", "/api/deco/list", "/api/deco/list", "", json_out, out_len, &code);
#endif
}

esp_err_t fish_api_bind(const char *device_id, const char *device_name)
{
    char body[160];
    snprintf(body, sizeof(body), "{\"deviceId\":\"%s\",\"deviceName\":\"%s\"}", device_id, device_name);
#if CONFIG_FISH_MOCK_API
    return ESP_OK;
#else
    char resp[256];
    int code = 0;
    return signed_request("POST", "/api/auth/bind", "/api/auth/bind", body, resp, sizeof(resp), &code);
#endif
}

esp_err_t fish_api_device_report(const char *device_id, const char *bind_token)
{
    if (!device_id || !device_id[0] || !bind_token || !bind_token[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    char body[160];
    snprintf(body, sizeof(body), "{\"deviceId\":\"%s\",\"bindToken\":\"%s\"}", device_id, bind_token);
#if CONFIG_FISH_MOCK_API
    (void)body;
    return ESP_OK;
#else
    char resp[256];
    int code = 0;
    esp_err_t err = signed_request("POST", "/api/device/report", "/api/device/report", body, resp, sizeof(resp), &code);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "device/report OK: %s", resp[0] ? resp : "(empty)");
        cJSON *root = cJSON_Parse(resp);
        if (root) {
            cJSON *biz = cJSON_GetObjectItem(root, "code");
            if (biz && cJSON_IsNumber(biz) && biz->valueint != 0 && biz->valueint != 200) {
                ESP_LOGW(TAG, "device/report biz code=%d", biz->valueint);
                err = ESP_FAIL;
            }
            cJSON_Delete(root);
        }
    }
    return err;
#endif
}

esp_err_t fish_api_download_url(const char *url, const char *dest_path)
{
    if (!url || !url[0] || !dest_path) {
        return ESP_ERR_INVALID_ARG;
    }
    char resolved[1024];
    if (strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0) {
        strncpy(resolved, url, sizeof(resolved) - 1);
        resolved[sizeof(resolved) - 1] = '\0';
    } else if (strncmp(url, "//", 2) == 0) {
        snprintf(resolved, sizeof(resolved), "https:%s", url);
    } else if (url[0] == '/') {
        snprintf(resolved, sizeof(resolved), "%s%s", CONFIG_FISH_API_HOST, url);
    } else if (strchr(url, '.')) {
        snprintf(resolved, sizeof(resolved), "https://%s", url);
    } else {
        snprintf(resolved, sizeof(resolved), "%s/%s", CONFIG_FISH_API_HOST, url);
    }
    char *encoded = malloc(1024);
    if (!encoded) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t enc_err = fish_api_url_encode(resolved, encoded, 1024);
    if (enc_err != ESP_OK) {
        free(encoded);
        return enc_err;
    }
    esp_http_client_config_t config = {
        .url = encoded,
        .timeout_ms = 30000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(encoded);
        return ESP_FAIL;
    }
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        free(encoded);
        return err;
    }
    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "download HTTP %d %s", status, encoded);
        esp_http_client_cleanup(client);
        free(encoded);
        return ESP_FAIL;
    }
    int content_length = esp_http_client_get_content_length(client);
    FILE *f = fopen(dest_path, "wb");
    if (!f) {
        esp_http_client_cleanup(client);
        free(encoded);
        return ESP_FAIL;
    }
    char buf[1024];
    int r;
    size_t written = 0;
    bool write_failed = false;
    while ((r = esp_http_client_read(client, buf, sizeof(buf))) > 0) {
        size_t nw = fwrite(buf, 1, (size_t)r, f);
        if (nw != (size_t)r) {
            ESP_LOGW(TAG, "download fwrite short %s", dest_path);
            write_failed = true;
            break;
        }
        written += nw;
    }
    fclose(f);
    esp_http_client_cleanup(client);
    free(encoded);
    if (write_failed || r < 0) {
        unlink(dest_path);
        return ESP_FAIL;
    }
    if (content_length > 0 && (int)written != content_length) {
        ESP_LOGW(TAG, "download size mismatch %s got %u expect %d", dest_path, (unsigned)written, content_length);
        unlink(dest_path);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "saved %s (%u bytes)", dest_path, (unsigned)written);
    FILE *check = fopen(dest_path, "rb");
    if (!check) {
        return ESP_FAIL;
    }
    uint8_t magic[4] = {0};
    size_t n = fread(magic, 1, sizeof(magic), check);
    fclose(check);
    bool ok = n >= 3 && ((magic[0] == 0xFF && magic[1] == 0xD8 && magic[2] == 0xFF) ||
                         (n >= 4 && magic[0] == 0x89 && magic[1] == 'P' && magic[2] == 'N' && magic[3] == 'G'));
    if (!ok) {
        ESP_LOGW(TAG, "download saved non-image %s", dest_path);
        unlink(dest_path);
        return ESP_FAIL;
    }
    return ESP_OK;
}
