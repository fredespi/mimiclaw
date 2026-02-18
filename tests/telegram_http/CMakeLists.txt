cmake_minimum_required(VERSION 3.16)
project(telegram_http_test C)

enable_testing()

add_executable(telegram_http_test
    test_telegram_http.c
    ../../main/telegram/telegram_http.c
)

target_include_directories(telegram_http_test PRIVATE
    ../../main
)

add_test(NAME telegram_http_test COMMAND telegram_http_test)
