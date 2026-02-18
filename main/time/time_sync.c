#include "time/time_sync.h"
#include "mimi_config.h"

#include <time.h>
#include <sys/time.h>
#include "esp_log.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "time_sync";

esp_err_t time_sync_wait(uint32_t timeout_ms)
{
    setenv("TZ", MIMI_TIMEZONE, 1);
    tzset();

    time_t now = 0;
    struct tm timeinfo = {0};
    time(&now);
    localtime_r(&now, &timeinfo);
    if (timeinfo.tm_year >= (2020 - 1900)) {
        return ESP_OK;
    }

    if (!esp_sntp_enabled()) {
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "pool.ntp.org");
        esp_sntp_setservername(1, "time.google.com");
        esp_sntp_init();
    }

    const TickType_t delay = pdMS_TO_TICKS(500);
    uint32_t waited = 0;
    while (waited < timeout_ms) {
        vTaskDelay(delay);
        waited += 500;
        time(&now);
        localtime_r(&now, &timeinfo);
        if (timeinfo.tm_year >= (2020 - 1900)) {
            ESP_LOGI(TAG, "Time synced: %04d-%02d-%02d %02d:%02d:%02d",
                     timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                     timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
            esp_sntp_stop();
            return ESP_OK;
        }
    }

    ESP_LOGW(TAG, "Time sync timed out");
    esp_sntp_stop();
    return ESP_ERR_TIMEOUT;
}
