#pragma once

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Run a Telegram HTTPS GET /getMe test.
 * Returns ESP_OK on HTTP 200 with ok=true.
 */
esp_err_t telegram_tls_self_test(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
