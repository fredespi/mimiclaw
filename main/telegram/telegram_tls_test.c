#include "telegram_tls_test.h"

#include "mimi_config.h"
#include "proxy/http_proxy.h"
#include "telegram/telegram_http.h"
#include "time/time_sync.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "nvs.h"

static const char *TAG = "telegram_tls_test";

static bool is_time_valid(void)
{
    time_t now = 0;
    struct tm timeinfo = {0};
    time(&now);
    localtime_r(&now, &timeinfo);
    return timeinfo.tm_year >= (2020 - 1900);
}

static esp_err_t load_bot_token(char *out, size_t out_len)
{
    if (!out || out_len == 0) return ESP_ERR_INVALID_ARG;
    out[0] = '\0';

    nvs_handle_t nvs;
    if (nvs_open(MIMI_NVS_TG, NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = out_len;
        if (nvs_get_str(nvs, MIMI_NVS_KEY_TG_TOKEN, out, &len) == ESP_OK && out[0]) {
            nvs_close(nvs);
            return ESP_OK;
        }
        nvs_close(nvs);
    }

    if (MIMI_SECRET_TG_TOKEN[0] != '\0') {
        snprintf(out, out_len, "%s", MIMI_SECRET_TG_TOKEN);
        return ESP_OK;
    }

    return ESP_ERR_INVALID_STATE;
}

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} http_resp_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_resp_t *resp = (http_resp_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        if (resp->len + evt->data_len + 1 > resp->cap) {
            size_t new_cap = resp->cap * 2;
            if (new_cap < resp->len + evt->data_len + 1) {
                new_cap = resp->len + evt->data_len + 1;
            }
            char *tmp = realloc(resp->buf, new_cap);
            if (!tmp) return ESP_ERR_NO_MEM;
            resp->buf = tmp;
            resp->cap = new_cap;
        }
        memcpy(resp->buf + resp->len, evt->data, evt->data_len);
        resp->len += evt->data_len;
        resp->buf[resp->len] = '\0';
    }
    return ESP_OK;
}

static esp_err_t http_get_via_proxy(const char *token, const char *path, int timeout_ms)
{
    proxy_conn_t *conn = proxy_conn_open("api.telegram.org", 443, timeout_ms);
    if (!conn) return ESP_ERR_HTTP_CONNECT;

    char header[256];
    int hlen = snprintf(header, sizeof(header),
        "GET /bot%s/%s HTTP/1.1\r\n"
        "Host: api.telegram.org\r\n"
        "Connection: close\r\n\r\n",
        token, path);

    if (proxy_conn_write(conn, header, hlen) < 0) {
        proxy_conn_close(conn);
        return ESP_FAIL;
    }

    size_t cap = 1024;
    char *buf = calloc(1, cap);
    if (!buf) {
        proxy_conn_close(conn);
        return ESP_ERR_NO_MEM;
    }

    size_t len = 0;
    while (1) {
        if (len + 512 >= cap) {
            cap *= 2;
            char *tmp = realloc(buf, cap);
            if (!tmp) break;
            buf = tmp;
        }
        int n = proxy_conn_read(conn, buf + len, cap - len - 1, timeout_ms);
        if (n <= 0) break;
        len += (size_t)n;
    }
    buf[len] = '\0';
    proxy_conn_close(conn);

    char *body = strstr(buf, "\r\n\r\n");
    if (!body) {
        free(buf);
        return ESP_FAIL;
    }
    body += 4;
    bool ok = strstr(body, "\"ok\":true") != NULL;
    free(buf);
    return ok ? ESP_OK : ESP_FAIL;
}

static esp_err_t http_get_direct(const char *url, int timeout_ms)
{
    http_resp_t resp = {
        .buf = calloc(1, 1024),
        .len = 0,
        .cap = 1024,
    };
    if (!resp.buf) return ESP_ERR_NO_MEM;

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = &resp,
        .timeout_ms = timeout_ms,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(resp.buf);
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        free(resp.buf);
        return err;
    }

    bool ok = (status == 200) && (resp.len > 0) && strstr(resp.buf, "\"ok\":true");
    free(resp.buf);
    return ok ? ESP_OK : ESP_FAIL;
}

esp_err_t telegram_tls_self_test(uint32_t timeout_ms)
{
    if (!is_time_valid()) {
        esp_err_t t = time_sync_wait(timeout_ms ? timeout_ms : 15000);
        if (t != ESP_OK) {
            ESP_LOGE(TAG, "Time sync failed; TLS will not work");
            return t;
        }
    }

    char token[128] = {0};
    if (load_bot_token(token, sizeof(token)) != ESP_OK) {
        ESP_LOGE(TAG, "No Telegram bot token configured");
        return ESP_ERR_INVALID_STATE;
    }

    telegram_http_request_t req;
    telegram_http_prepare_request(&req, token, "getMe", NULL);

    int effective_timeout = (timeout_ms > 0) ? (int)timeout_ms : 15000;

    if (http_proxy_is_enabled()) {
        ESP_LOGI(TAG, "Proxy enabled; testing TLS via proxy tunnel");
        return http_get_via_proxy(token, "getMe", effective_timeout);
    }

    ESP_LOGI(TAG, "Testing direct TLS to Telegram");
    return http_get_direct(req.url, effective_timeout);
}
