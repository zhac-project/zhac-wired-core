// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Host stand-ins for what main/auth.cpp touches outside itself: ESP-IDF's NVS / log / timer / random,
// FreeRTOS's mutex, PSA hashing, the HTTP server, the WebSocket server and the set-up window.
// The WebSocket fake (harness.cpp) remembers the installed token and whether a socket is signed in.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <netinet/in.h>
#include <sys/socket.h>

typedef int esp_err_t;
constexpr esp_err_t ESP_OK = 0;
constexpr esp_err_t ESP_FAIL = -1;

#define ESP_LOGE(tag, fmt, ...) do { (void)(tag); if (0) std::printf(fmt, ##__VA_ARGS__); } while (0)
#define ESP_LOGW(tag, fmt, ...) do { (void)(tag); if (0) std::printf(fmt, ##__VA_ARGS__); } while (0)
#define ESP_LOGI(tag, fmt, ...) do { (void)(tag); if (0) std::printf(fmt, ##__VA_ARGS__); } while (0)
#define ESP_LOGD(tag, fmt, ...) do { (void)(tag); if (0) std::printf(fmt, ##__VA_ARGS__); } while (0)

int64_t esp_timer_get_time();
void    esp_fill_random(void* buf, size_t len);

typedef void* SemaphoreHandle_t;
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
SemaphoreHandle_t xSemaphoreCreateMutex();
int xSemaphoreTake(SemaphoreHandle_t, int);
int xSemaphoreGive(SemaphoreHandle_t);

typedef uint32_t nvs_handle_t;
enum nvs_open_mode_t { NVS_READONLY, NVS_READWRITE };
esp_err_t nvs_open(const char* ns, nvs_open_mode_t mode, nvs_handle_t* out);
esp_err_t nvs_get_u8(nvs_handle_t, const char* key, uint8_t* out);
esp_err_t nvs_set_u8(nvs_handle_t, const char* key, uint8_t v);
esp_err_t nvs_get_str(nvs_handle_t, const char* key, char* out, size_t* len);
esp_err_t nvs_set_str(nvs_handle_t, const char* key, const char* v);
esp_err_t nvs_get_blob(nvs_handle_t, const char* key, void* out, size_t* len);
esp_err_t nvs_set_blob(nvs_handle_t, const char* key, const void* v, size_t len);
esp_err_t nvs_commit(nvs_handle_t);
void      nvs_close(nvs_handle_t);

typedef int psa_status_t;
constexpr psa_status_t PSA_SUCCESS = 0;
#define PSA_ALG_SHA_256 0
psa_status_t psa_crypto_init();
psa_status_t psa_hash_compute(int alg, const uint8_t* in, size_t in_len, uint8_t* out, size_t out_cap, size_t* out_len);

typedef struct httpd_req { size_t content_len; void* user_ctx; } httpd_req_t;
typedef void* httpd_handle_t;
enum httpd_method_t { HTTP_GET, HTTP_POST };
typedef struct httpd_uri {
    const char* uri; httpd_method_t method; esp_err_t (*handler)(httpd_req_t*); void* user_ctx;
} httpd_uri_t;
int       httpd_req_to_sockfd(httpd_req_t*);
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t*, const char* field, char* val, size_t len);
esp_err_t httpd_resp_set_status(httpd_req_t*, const char* status);
esp_err_t httpd_resp_set_type(httpd_req_t*, const char* type);
esp_err_t httpd_resp_sendstr(httpd_req_t*, const char* str);
int       httpd_req_recv(httpd_req_t*, char* buf, size_t len);
esp_err_t httpd_register_uri_handler(httpd_handle_t, const httpd_uri_t*);

void ws_server_set_api_token(const char* token);
void ws_server_fd_deauth_all();

constexpr uint32_t kZapSetupWindowS = 600;
uint32_t zap_setup_secs_left();
