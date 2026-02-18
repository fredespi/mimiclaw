#pragma once

#include "esp_err.h"
#include <stdint.h>

/*
 * Initialize SNTP and wait for a valid system time.
 * Returns ESP_OK on success, ESP_ERR_TIMEOUT on timeout.
 */
esp_err_t time_sync_wait(uint32_t timeout_ms);
