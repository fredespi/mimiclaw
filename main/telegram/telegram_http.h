#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char url[256];
    const char *content_type;
    bool is_post;
    size_t content_length;
} telegram_http_request_t;

void telegram_http_prepare_request(telegram_http_request_t *req,
                                   const char *bot_token,
                                   const char *method,
                                   const char *post_data);

#ifdef __cplusplus
}
#endif
