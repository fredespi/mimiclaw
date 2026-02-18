#include "tool_web_search.h"
#include "mimi_config.h"
#include "proxy/http_proxy.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "cJSON.h"

static const char *TAG = "web_search";

static char s_search_key[128] = {0};

#define SEARCH_RESULT_COUNT 3
#define SEARCH_TEMP_FILE    "/spiffs/tmp_search.json"
#define SEARCH_READ_BUF     (4 * 1024)

/* ── Write HTTP response directly to SPIFFS temp file ─────────── */

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    FILE **fp = (FILE **)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && *fp) {
        fwrite(evt->data, 1, evt->data_len, *fp);
    }
    return ESP_OK;
}

/* ── Init ─────────────────────────────────────────────────────── */

esp_err_t tool_web_search_init(void)
{
    /* Start with build-time default */
    if (MIMI_SECRET_SEARCH_KEY[0] != '\0') {
        strncpy(s_search_key, MIMI_SECRET_SEARCH_KEY, sizeof(s_search_key) - 1);
    }

    /* NVS overrides take highest priority (set via CLI) */
    nvs_handle_t nvs;
    if (nvs_open(MIMI_NVS_SEARCH, NVS_READONLY, &nvs) == ESP_OK) {
        char tmp[128] = {0};
        size_t len = sizeof(tmp);
        if (nvs_get_str(nvs, MIMI_NVS_KEY_API_KEY, tmp, &len) == ESP_OK && tmp[0]) {
            strncpy(s_search_key, tmp, sizeof(s_search_key) - 1);
        }
        nvs_close(nvs);
    }

    if (s_search_key[0]) {
        ESP_LOGI(TAG, "Web search initialized (key configured)");
    } else {
        ESP_LOGW(TAG, "No search API key. Use CLI: set_search_key <KEY>");
    }
    return ESP_OK;
}

/* ── URL-encode a query string ────────────────────────────────── */

static size_t url_encode(const char *src, char *dst, size_t dst_size)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t pos = 0;

    for (; *src && pos < dst_size - 3; src++) {
        unsigned char c = (unsigned char)*src;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            dst[pos++] = c;
        } else if (c == ' ') {
            dst[pos++] = '+';
        } else {
            dst[pos++] = '%';
            dst[pos++] = hex[c >> 4];
            dst[pos++] = hex[c & 0x0F];
        }
    }
    dst[pos] = '\0';
    return pos;
}

/* ── Format results as readable text ──────────────────────────── */

static void format_results(cJSON *root, char *output, size_t output_size)
{
    cJSON *web = cJSON_GetObjectItem(root, "web");
    if (!web) {
        snprintf(output, output_size, "No web results found.");
        return;
    }

    cJSON *results = cJSON_GetObjectItem(web, "results");
    if (!results || !cJSON_IsArray(results) || cJSON_GetArraySize(results) == 0) {
        snprintf(output, output_size, "No web results found.");
        return;
    }

    size_t off = 0;
    int idx = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, results) {
        if (idx >= SEARCH_RESULT_COUNT) break;

        cJSON *title = cJSON_GetObjectItem(item, "title");
        cJSON *url = cJSON_GetObjectItem(item, "url");
        cJSON *desc = cJSON_GetObjectItem(item, "description");

        off += snprintf(output + off, output_size - off,
            "%d. %s\n   %s\n   %s\n\n",
            idx + 1,
            (title && cJSON_IsString(title)) ? title->valuestring : "(no title)",
            (url && cJSON_IsString(url)) ? url->valuestring : "",
            (desc && cJSON_IsString(desc)) ? desc->valuestring : "");

        if (off >= output_size - 1) break;
        idx++;
    }
}

/* ── Direct HTTPS request → write to SPIFFS temp file ─────────── */

static esp_err_t search_direct(const char *url, FILE *fp)
{
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = &fp,
        .timeout_ms = 15000,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_FAIL;

    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "X-Subscription-Token", s_search_key);

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        return err;
    }
    if (status != 200) {
        ESP_LOGE(TAG, "Search API returned %d", status);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ── Proxy HTTPS request → write to SPIFFS temp file ──────────── */

static esp_err_t search_via_proxy(const char *path, FILE *fp)
{
    proxy_conn_t *conn = proxy_conn_open("api.search.brave.com", 443, 15000);
    if (!conn) return ESP_ERR_HTTP_CONNECT;

    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "GET %s HTTP/1.1\r\n"
        "Host: api.search.brave.com\r\n"
        "Accept: application/json\r\n"
        "X-Subscription-Token: %s\r\n"
        "Connection: close\r\n\r\n",
        path, s_search_key);

    if (proxy_conn_write(conn, header, hlen) < 0) {
        proxy_conn_close(conn);
        return ESP_ERR_HTTP_WRITE_DATA;
    }

    /* Read response into temp buffer, write to file */
    char tmp[1024];
    size_t total = 0;
    bool headers_done = false;
    size_t hdr_buf_len = 0;
    char hdr_buf[2048];  /* small buffer just for HTTP headers */
    int status = 0;

    while (1) {
        int n = proxy_conn_read(conn, tmp, sizeof(tmp), 15000);
        if (n <= 0) break;

        if (!headers_done) {
            /* Accumulate until we find end of headers */
            size_t copy = (size_t)n;
            if (hdr_buf_len + copy > sizeof(hdr_buf) - 1)
                copy = sizeof(hdr_buf) - 1 - hdr_buf_len;
            memcpy(hdr_buf + hdr_buf_len, tmp, copy);
            hdr_buf_len += copy;
            hdr_buf[hdr_buf_len] = '\0';

            char *body = strstr(hdr_buf, "\r\n\r\n");
            if (body) {
                /* Parse status from header */
                if (hdr_buf_len > 5 && strncmp(hdr_buf, "HTTP/", 5) == 0) {
                    const char *sp = strchr(hdr_buf, ' ');
                    if (sp) status = atoi(sp + 1);
                }
                headers_done = true;
                body += 4;
                size_t body_len = hdr_buf_len - (body - hdr_buf);
                if (body_len > 0) {
                    fwrite(body, 1, body_len, fp);
                    total += body_len;
                }
            }
        } else {
            fwrite(tmp, 1, (size_t)n, fp);
            total += (size_t)n;
        }
    }
    proxy_conn_close(conn);

    ESP_LOGI(TAG, "Proxy: received %d body bytes, status=%d", (int)total, status);

    if (status != 200) {
        ESP_LOGE(TAG, "Search API returned %d via proxy", status);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ── Execute ──────────────────────────────────────────────────── */

esp_err_t tool_web_search_execute(const char *input_json, char *output, size_t output_size)
{
    if (s_search_key[0] == '\0') {
        snprintf(output, output_size, "Error: No search API key configured. Set MIMI_SECRET_SEARCH_KEY in mimi_secrets.h");
        return ESP_ERR_INVALID_STATE;
    }

    /* Parse input to get query */
    cJSON *input = cJSON_Parse(input_json);
    if (!input) {
        snprintf(output, output_size, "Error: Invalid input JSON");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *query = cJSON_GetObjectItem(input, "query");
    if (!query || !cJSON_IsString(query) || query->valuestring[0] == '\0') {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: Missing 'query' field");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Searching: %s", query->valuestring);
    ESP_LOGI(TAG, "Free internal heap: %d", (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    /* Build URL */
    char encoded_query[256];
    url_encode(query->valuestring, encoded_query, sizeof(encoded_query));
    cJSON_Delete(input);

    char path[384];
    snprintf(path, sizeof(path),
             "/res/v1/web/search?q=%s&count=%d&result_filter=web&text_decorations=false&extra_snippets=false",
             encoded_query, SEARCH_RESULT_COUNT);

    /* Open temp file for writing — HTTP response goes to flash, not RAM */
    FILE *fp = fopen(SEARCH_TEMP_FILE, "w");
    if (!fp) {
        snprintf(output, output_size, "Error: Cannot create temp file");
        return ESP_FAIL;
    }

    /* Make HTTP request — response written to SPIFFS temp file */
    esp_err_t err;
    if (http_proxy_is_enabled()) {
        err = search_via_proxy(path, fp);
    } else {
        char url[512];
        snprintf(url, sizeof(url), "https://api.search.brave.com%s", path);
        err = search_direct(url, fp);
    }
    fclose(fp);

    if (err != ESP_OK) {
        remove(SEARCH_TEMP_FILE);
        snprintf(output, output_size, "Error: Search request failed (%s)", esp_err_to_name(err));
        return err;
    }

    /* TLS is now disconnected — heap is free for parsing.
     * Read the temp file back into a malloc'd buffer. */
    fp = fopen(SEARCH_TEMP_FILE, "r");
    if (!fp) {
        snprintf(output, output_size, "Error: Cannot read temp file");
        return ESP_FAIL;
    }

    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    ESP_LOGI(TAG, "Search response: %ld bytes on disk, free heap: %d",
             fsize, (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    if (fsize <= 0 || fsize > 32 * 1024) {
        fclose(fp);
        remove(SEARCH_TEMP_FILE);
        snprintf(output, output_size, "Error: Invalid response size (%ld)", fsize);
        return ESP_FAIL;
    }

    char *json_buf = malloc((size_t)fsize + 1);
    if (!json_buf) {
        fclose(fp);
        remove(SEARCH_TEMP_FILE);
        snprintf(output, output_size, "Error: Out of memory for parse (%ld bytes)", fsize);
        return ESP_ERR_NO_MEM;
    }

    size_t nread = fread(json_buf, 1, (size_t)fsize, fp);
    json_buf[nread] = '\0';
    fclose(fp);
    remove(SEARCH_TEMP_FILE);

    /* Parse and format results */
    cJSON *root = cJSON_Parse(json_buf);
    free(json_buf);

    if (!root) {
        snprintf(output, output_size, "Error: Failed to parse search results");
        return ESP_FAIL;
    }

    format_results(root, output, output_size);
    cJSON_Delete(root);

    ESP_LOGI(TAG, "Search complete, %d bytes result", (int)strlen(output));
    return ESP_OK;
}

esp_err_t tool_web_search_set_key(const char *api_key)
{
    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open(MIMI_NVS_SEARCH, NVS_READWRITE, &nvs));
    ESP_ERROR_CHECK(nvs_set_str(nvs, MIMI_NVS_KEY_API_KEY, api_key));
    ESP_ERROR_CHECK(nvs_commit(nvs));
    nvs_close(nvs);

    strncpy(s_search_key, api_key, sizeof(s_search_key) - 1);
    ESP_LOGI(TAG, "Search API key saved");
    return ESP_OK;
}
