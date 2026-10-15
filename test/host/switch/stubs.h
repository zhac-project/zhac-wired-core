// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Host stand-ins for what main/sys_state.cpp and system_apply_settings (main/api_system.cpp) touch outside
// themselves: ESP-IDF's NVS / log / error names, the auth module and the neighbours of system_apply_settings.
// The NVS fake (harness.cpp) can fail at open, set and commit.
#pragma once
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <ArduinoJson.h>

typedef int esp_err_t;
constexpr esp_err_t ESP_OK = 0;
constexpr esp_err_t ESP_FAIL = -1;
constexpr esp_err_t ESP_ERR_NVS_NOT_INITIALIZED = 0x1101;    // the real values
constexpr esp_err_t ESP_ERR_NVS_NOT_FOUND = 0x1102;
constexpr esp_err_t ESP_ERR_NVS_NOT_ENOUGH_SPACE = 0x1105;
const char* esp_err_to_name(esp_err_t code);

void harness_log(char level, const char* tag, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
#define ESP_LOGE(tag, fmt, ...) harness_log('E', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) harness_log('W', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) harness_log('I', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) ((void)0)

typedef uint32_t nvs_handle_t;
typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;
extern "C" {
esp_err_t nvs_open(const char* ns, nvs_open_mode_t mode, nvs_handle_t* h);
esp_err_t nvs_get_u8(nvs_handle_t h, const char* key, uint8_t* out);
esp_err_t nvs_set_u8(nvs_handle_t h, const char* key, uint8_t v);
esp_err_t nvs_get_str(nvs_handle_t h, const char* key, char* out, size_t* len);
esp_err_t nvs_set_str(nvs_handle_t h, const char* key, const char* v);
esp_err_t nvs_commit(nvs_handle_t h);
void nvs_close(nvs_handle_t h);
}

// auth.h (main/auth.cpp owns access control)
bool auth_enabled();
void auth_set_enabled(bool en);
size_t auth_token_copy(char* out, size_t cap);
bool auth_rotate_token(char* out, size_t cap);

// neighbours of system_apply_settings
void mqtt_glue_apply_settings(JsonDocument& doc);
bool ntp_cfg_set_server(const char* host);
extern "C" void metrics_mqtt_publisher_start();
void log_sinks_set_mqtt(bool enabled, char min_level);
void log_sinks_set_ws(bool enabled, char min_level);
bool log_sinks_get_mqtt_enabled();
bool log_sinks_get_ws_enabled();
char log_sinks_get_mqtt_level();
char log_sinks_get_ws_level();
