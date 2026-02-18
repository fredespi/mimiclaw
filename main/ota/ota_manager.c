#include "ota_manager.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_https_ota.h"
#include "esp_app_desc.h"

static const char *TAG = "ota";

#define OTA_MARKER_PATH "/spiffs/ota_marker.txt"

static bool is_https(const char *url)
{
    return url && strncmp(url, "https://", 8) == 0;
}

/* Build a callback URL from the firmware URL:
 * http://192.168.1.100:8199/mimiclaw.bin  →  http://192.168.1.100:8199/ota_done
 */
static void build_callback_url(const char *fw_url, char *out, size_t out_sz)
{
    /* Find the last '/' */
    const char *last_slash = strrchr(fw_url, '/');
    if (last_slash) {
        size_t prefix_len = (size_t)(last_slash - fw_url + 1);
        if (prefix_len >= out_sz) prefix_len = out_sz - 1;
        memcpy(out, fw_url, prefix_len);
        snprintf(out + prefix_len, out_sz - prefix_len, "ota_done");
    } else {
        snprintf(out, out_sz, "%s", fw_url);
    }
}

static void save_marker(const char *callback_url)
{
    FILE *f = fopen(OTA_MARKER_PATH, "w");
    if (f) {
        fprintf(f, "%s", callback_url);
        fclose(f);
        ESP_LOGI(TAG, "OTA marker saved: %s", callback_url);
    } else {
        ESP_LOGE(TAG, "Failed to write OTA marker");
    }
}

esp_err_t ota_update_from_url(const char *url)
{
    if (!url || url[0] == '\0') {
        ESP_LOGE(TAG, "OTA URL is empty");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Starting OTA from: %s", url);

    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 120000,
        .buffer_size = 4096,
        .keep_alive_enable = true,
    };

    /* Only attach TLS cert bundle for HTTPS URLs */
    if (is_https(url)) {
        config.crt_bundle_attach = esp_crt_bundle_attach;
    }

    esp_https_ota_config_t ota_config = {
        .http_config = &config,
    };

    esp_err_t ret = esp_https_ota(&ota_config);
    if (ret == ESP_OK) {
        /* Save marker so after reboot we can POST back to the deploy server */
        char callback_url[200];
        build_callback_url(url, callback_url, sizeof(callback_url));
        save_marker(callback_url);

        ESP_LOGI(TAG, "OTA successful, restarting...");
        esp_restart();
    } else {
        ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(ret));
    }

    return ret;
}

void ota_check_post_update(void)
{
    FILE *f = fopen(OTA_MARKER_PATH, "r");
    if (!f) return;  /* No marker — normal boot */

    char callback_url[200] = {0};
    fgets(callback_url, sizeof(callback_url), f);
    fclose(f);

    /* Delete marker immediately so we don't loop */
    remove(OTA_MARKER_PATH);

    if (callback_url[0] == '\0') return;

    ESP_LOGI(TAG, "Post-OTA: notifying %s", callback_url);

    /* Build a simple JSON body with version info */
    const esp_app_desc_t *app = esp_app_get_description();
    char body[256];
    snprintf(body, sizeof(body),
             "{\"status\":\"ok\",\"version\":\"%s\",\"idf\":\"%s\"}",
             app->version, app->idf_ver);

    esp_http_client_config_t config = {
        .url = callback_url,
        .timeout_ms = 10000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGW(TAG, "Post-OTA callback: failed to init HTTP client");
        return;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "Post-OTA callback sent (HTTP %d)", status);
    } else {
        ESP_LOGW(TAG, "Post-OTA callback failed: %s (server may have already exited)",
                 esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
}
