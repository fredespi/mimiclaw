#include "telegram_http.h"

#include <stdio.h>
#include <string.h>

void telegram_http_prepare_request(telegram_http_request_t *req,
                                   const char *bot_token,
                                   const char *method,
                                   const char *post_data)
{
    if (!req) return;

    memset(req, 0, sizeof(*req));

    if (!bot_token) bot_token = "";
    if (!method) method = "";

    snprintf(req->url, sizeof(req->url),
             "https://api.telegram.org/bot%s/%s", bot_token, method);

    if (post_data) {
        req->is_post = true;
        req->content_type = "application/json";
        req->content_length = strlen(post_data);
    }
}
