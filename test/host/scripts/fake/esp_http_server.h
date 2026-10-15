// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Host stand-in for esp_http_server.h: only what api_scripts.cpp uses.
#pragma once
#include <cstddef>
#include <cstdint>
#include <sys/types.h>
typedef int esp_err_t;
constexpr esp_err_t ESP_OK = 0, ESP_FAIL = -1;
typedef void* httpd_handle_t;
enum { HTTP_DELETE = 0, HTTP_GET = 1, HTTP_POST = 3, HTTP_PUT = 4 };
struct httpd_req_t { int method; char uri[513]; size_t content_len; void* user_ctx; };
typedef esp_err_t (*httpd_handler_t)(httpd_req_t*);
struct httpd_uri_t { const char* uri; int method; httpd_handler_t handler; void* user_ctx; };
typedef enum {
    HTTPD_400_BAD_REQUEST = 400, HTTPD_404_NOT_FOUND = 404, HTTPD_405_METHOD_NOT_ALLOWED = 405,
    HTTPD_408_REQ_TIMEOUT = 408, HTTPD_411_LENGTH_REQUIRED = 411, HTTPD_413_CONTENT_TOO_LARGE = 413,
    HTTPD_500_INTERNAL_SERVER_ERROR = 500
} httpd_err_code_t;
#define HTTPD_SOCK_ERR_FAIL      -1   // what httpd_req_recv answers (esp_http_server.h, IDF 6.0 and 6.1-beta1)
#define HTTPD_SOCK_ERR_INVALID   -2
#define HTTPD_SOCK_ERR_TIMEOUT   -3
int       httpd_req_recv(httpd_req_t* r, char* buf, size_t len);
size_t    httpd_req_get_hdr_value_len(httpd_req_t* r, const char* field);
esp_err_t httpd_resp_set_type(httpd_req_t* r, const char* type);
esp_err_t httpd_resp_send(httpd_req_t* r, const char* buf, ssize_t len);
esp_err_t httpd_resp_sendstr(httpd_req_t* r, const char* str);
esp_err_t httpd_resp_send_err(httpd_req_t* r, httpd_err_code_t code, const char* msg);
