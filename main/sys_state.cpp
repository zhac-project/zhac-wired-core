// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sys_state.h"

#include <cstdlib>
#include <cstring>
#include <ctime>

#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"
#include "ws_server.h"

static const char* TAG = "sys_state";

static bool s_metrics_enabled = false;
static bool s_ap_disabled     = false;
static bool s_auth_enabled    = false;
static char s_api_token[33]   = {0};

static uint8_t nvs_get_u8(const char* ns, const char* key, uint8_t def) {
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK) return def;
    uint8_t v = def;
    nvs_get_u8(h, key, &v);
    nvs_close(h);
    return v;
}

static void nvs_set_u8_commit(const char* ns, const char* key, uint8_t v) {
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, key, v);
    nvs_commit(h);
    nvs_close(h);
}

static void hex32(const uint8_t in[16], char out[33]) {
    static const char* H = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        out[i * 2]     = H[in[i] >> 4];
        out[i * 2 + 1] = H[in[i] & 0x0f];
    }
    out[32] = '\0';
}

void sys_state_init() {
    s_metrics_enabled = nvs_get_u8("sys_cfg", "metrics_en", 0) != 0;
    s_ap_disabled     = nvs_get_u8("sys_cfg", "ap_disabled", 0) != 0;
    s_auth_enabled    = nvs_get_u8("zhac_auth", "enabled", 0) != 0;

    nvs_handle_t h;
    if (nvs_open("zhac_auth", NVS_READONLY, &h) == ESP_OK) {
        size_t sz = sizeof(s_api_token);
        if (nvs_get_str(h, "token", s_api_token, &sz) != ESP_OK) s_api_token[0] = '\0';
        nvs_close(h);
    }

    char tz[64];
    if (nvs_open("sys_cfg", NVS_READONLY, &h) == ESP_OK) {
        size_t sz = sizeof(tz);
        if (nvs_get_str(h, "timezone", tz, &sz) == ESP_OK && tz[0]) {
            setenv("TZ", tz, 1);
            tzset();
        }
        nvs_close(h);
    }

    if (s_auth_enabled && s_api_token[0]) {
        ws_server_set_api_token(s_api_token);
    }
    ESP_LOGI(TAG, "init: metrics=%d ap_disabled=%d auth=%d token=%s",
             s_metrics_enabled, s_ap_disabled, s_auth_enabled,
             s_api_token[0] ? "set" : "none");
}

bool sys_metrics_enabled() { return s_metrics_enabled; }
bool sys_ap_disabled()     { return s_ap_disabled; }
bool sys_auth_enabled()    { return s_auth_enabled; }

void sys_set_metrics_enabled(bool en) {
    s_metrics_enabled = en;
    nvs_set_u8_commit("sys_cfg", "metrics_en", en ? 1 : 0);
}

void sys_set_ap_disabled(bool dis) {
    s_ap_disabled = dis;
    nvs_set_u8_commit("sys_cfg", "ap_disabled", dis ? 1 : 0);
}

void sys_set_auth_enabled(bool en) {
    s_auth_enabled = en;
    nvs_set_u8_commit("zhac_auth", "enabled", en ? 1 : 0);
    ws_server_set_api_token(en && s_api_token[0] ? s_api_token : nullptr);
}

void sys_set_timezone(const char* tz) {
    if (!tz) return;
    nvs_handle_t h;
    if (nvs_open("sys_cfg", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "timezone", tz);
        nvs_commit(h);
        nvs_close(h);
    }
    setenv("TZ", tz, 1);
    tzset();
}

size_t sys_get_api_token(char* out, size_t cap) {
    if (!out || cap == 0) return 0;
    snprintf(out, cap, "%s", s_api_token);
    return strlen(out);
}

bool sys_rotate_api_token(char* out, size_t cap) {
    if (cap < 33) return false;
    uint8_t rnd[16];
    esp_fill_random(rnd, sizeof(rnd));
    hex32(rnd, s_api_token);

    nvs_handle_t h;
    if (nvs_open("zhac_auth", NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = (nvs_set_str(h, "token", s_api_token) == ESP_OK) &&
              (nvs_commit(h) == ESP_OK);
    nvs_close(h);
    if (!ok) return false;

    if (s_auth_enabled) ws_server_set_api_token(s_api_token);
    memcpy(out, s_api_token, 33);
    return true;
}
