#include "ws_server.h"
#include "mimi_config.h"
#include "bus/message_bus.h"
#include "ota/ota_manager.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ws";

static httpd_handle_t s_server = NULL;

/* Simple client tracking */
typedef struct {
    int fd;
    char chat_id[32];
    bool active;
} ws_client_t;

static ws_client_t s_clients[MIMI_WS_MAX_CLIENTS];

static ws_client_t *find_client_by_fd(int fd)
{
    for (int i = 0; i < MIMI_WS_MAX_CLIENTS; i++) {
        if (s_clients[i].active && s_clients[i].fd == fd) {
            return &s_clients[i];
        }
    }
    return NULL;
}

static ws_client_t *find_client_by_chat_id(const char *chat_id)
{
    for (int i = 0; i < MIMI_WS_MAX_CLIENTS; i++) {
        if (s_clients[i].active && strcmp(s_clients[i].chat_id, chat_id) == 0) {
            return &s_clients[i];
        }
    }
    return NULL;
}

static ws_client_t *add_client(int fd)
{
    for (int i = 0; i < MIMI_WS_MAX_CLIENTS; i++) {
        if (!s_clients[i].active) {
            s_clients[i].fd = fd;
            snprintf(s_clients[i].chat_id, sizeof(s_clients[i].chat_id), "ws_%d", fd);
            s_clients[i].active = true;
            ESP_LOGI(TAG, "Client connected: %s (fd=%d)", s_clients[i].chat_id, fd);
            return &s_clients[i];
        }
    }
    ESP_LOGW(TAG, "Max clients reached, rejecting fd=%d", fd);
    return NULL;
}

static void remove_client(int fd)
{
    for (int i = 0; i < MIMI_WS_MAX_CLIENTS; i++) {
        if (s_clients[i].active && s_clients[i].fd == fd) {
            ESP_LOGI(TAG, "Client disconnected: %s", s_clients[i].chat_id);
            s_clients[i].active = false;
            return;
        }
    }
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        /* WebSocket handshake — register client */
        int fd = httpd_req_to_sockfd(req);
        add_client(fd);
        return ESP_OK;
    }

    // ESP32: WebSocket frame APIs not available, treat as plain HTTP POST
    int fd = httpd_req_to_sockfd(req);
    ws_client_t *client = find_client_by_fd(fd);
    const char *chat_id = client ? client->chat_id : "ws_unknown";

    size_t buf_len = httpd_req_get_hdr_value_len(req, "Content-Length");
    if (buf_len == 0) return ESP_OK;
    char *buf = calloc(1, buf_len + 1);
    if (!buf) return ESP_ERR_NO_MEM;
    int ret = httpd_req_recv(req, buf, buf_len);
    if (ret <= 0) {
        free(buf);
        return ESP_FAIL;
    }
    cJSON *root = cJSON_Parse(buf);
    free(buf);

    if (!root) {
        return ESP_OK;
    }

    cJSON *type = cJSON_GetObjectItem(root, "type");
    cJSON *content = cJSON_GetObjectItem(root, "content");

    if (type && cJSON_IsString(type) && strcmp(type->valuestring, "message") == 0
        && content && cJSON_IsString(content)) {

        /* Determine chat_id */
        cJSON *cid = cJSON_GetObjectItem(root, "chat_id");
        if (cid && cJSON_IsString(cid)) {
            chat_id = cid->valuestring;
            /* Update client's chat_id if provided */
            if (client) {
                strncpy(client->chat_id, chat_id, sizeof(client->chat_id) - 1);
            }
        }

        ESP_LOGI(TAG, "WS message from %s: %.40s...", chat_id, content->valuestring);

        /* Push to inbound bus */
        mimi_msg_t msg = {0};
        strncpy(msg.channel, MIMI_CHAN_WEBSOCKET, sizeof(msg.channel) - 1);
        strncpy(msg.chat_id, chat_id, sizeof(msg.chat_id) - 1);
        msg.content = strdup(content->valuestring);
        if (msg.content) {
            message_bus_push_inbound(&msg);
        }
    }

    /* Always send an HTTP response (required for POST handler) */
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "type", "response");
    cJSON_AddStringToObject(resp, "content", "OK");
    cJSON_AddStringToObject(resp, "chat_id", chat_id);
    char *json_str = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);

    cJSON_Delete(root);

    if (json_str) {
        httpd_resp_set_type(req, "application/json");
        esp_err_t send_ret = httpd_resp_send(req, json_str, strlen(json_str));
        if (send_ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to send response: %d", send_ret);
        }
        free(json_str);
    }

    return ESP_OK;
}

/* ---------- OTA endpoint ---------- */
static esp_err_t ota_handler(httpd_req_t *req)
{
    char body[256] = {0};
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    body[received] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *url = cJSON_GetObjectItem(root, "url");
    if (!url || !cJSON_IsString(url) || !url->valuestring[0]) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing \"url\" field");
        return ESP_FAIL;
    }

    /* Copy URL before we free JSON */
    char ota_url[200];
    strncpy(ota_url, url->valuestring, sizeof(ota_url) - 1);
    ota_url[sizeof(ota_url) - 1] = '\0';
    cJSON_Delete(root);

    ESP_LOGI(TAG, "OTA via HTTP: %s", ota_url);

    /* Respond immediately, then start OTA */
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\",\"message\":\"OTA starting\"}");

    /* Small delay so the HTTP response is flushed */
    vTaskDelay(pdMS_TO_TICKS(500));

    esp_err_t err = ota_update_from_url(ota_url);
    /* If we get here, OTA failed (success reboots) */
    ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(err));
    return ESP_FAIL;
}

esp_err_t ws_server_start(void)
{
    memset(s_clients, 0, sizeof(s_clients));

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = MIMI_WS_PORT;
    config.ctrl_port = MIMI_WS_PORT + 1;
    config.max_open_sockets = MIMI_WS_MAX_CLIENTS;

    esp_err_t ret = httpd_start(&s_server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WebSocket server: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Register URI for ESP32: treat as HTTP POST */
    httpd_uri_t ws_uri = {
        .uri = "/ws",
        .method = HTTP_POST,
        .handler = ws_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &ws_uri);

    /* OTA endpoint — POST {"url":"http://host:port/firmware.bin"} */
    httpd_uri_t ota_uri = {
        .uri = "/ota",
        .method = HTTP_POST,
        .handler = ota_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &ota_uri);

    ESP_LOGI(TAG, "WebSocket server started on port %d", MIMI_WS_PORT);
    return ESP_OK;
}

esp_err_t ws_server_send(const char *chat_id, const char *text)
{
    // Disabled for ESP32: responses must be sent from ws_handler
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ws_server_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
        ESP_LOGI(TAG, "WebSocket server stopped");
    }
    return ESP_OK;
}
