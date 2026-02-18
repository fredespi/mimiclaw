#include "telegram/telegram_http.h"

#include <stdio.h>
#include <string.h>

static int s_failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        s_failures++; \
        fprintf(stderr, "FAIL: %s\n", msg); \
    } \
} while (0)

static void test_post_request(void)
{
    telegram_http_request_t req;
    const char *token = "123";
    const char *method = "sendMessage";
    const char *body = "{\"text\":\"hi\"}";

    telegram_http_prepare_request(&req, token, method, body);

    CHECK(req.is_post, "POST request should set is_post");
    CHECK(req.content_type && strcmp(req.content_type, "application/json") == 0,
          "POST request should set content_type");
    CHECK(req.content_length == strlen(body), "POST request length mismatch");
    CHECK(strcmp(req.url, "https://api.telegram.org/bot123/sendMessage") == 0,
          "POST request URL mismatch");
}

static void test_get_request(void)
{
    telegram_http_request_t req;
    const char *token = "abc";
    const char *method = "getUpdates?offset=7";

    telegram_http_prepare_request(&req, token, method, NULL);

    CHECK(!req.is_post, "GET request should not set is_post");
    CHECK(req.content_type == NULL, "GET request should not set content_type");
    CHECK(req.content_length == 0, "GET request should have 0 length");
    CHECK(strcmp(req.url, "https://api.telegram.org/botabc/getUpdates?offset=7") == 0,
          "GET request URL mismatch");
}

int main(void)
{
    test_post_request();
    test_get_request();

    if (s_failures == 0) {
        printf("OK\n");
        return 0;
    }

    fprintf(stderr, "%d test(s) failed\n", s_failures);
    return 1;
}
