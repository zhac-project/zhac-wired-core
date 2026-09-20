// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sys_state.h"

#include <cstdlib>
#include <cstring>
#include <ctime>

#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"
#include "auth.h"

static const char* TAG = "sys_state";

static bool s_metrics_enabled = false;
static bool s_ap_disabled     = false;

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


void sys_state_init() {
    s_metrics_enabled = nvs_get_u8("sys_cfg", "metrics_en", 0) != 0;
    s_ap_disabled     = nvs_get_u8("sys_cfg", "ap_disabled", 0) != 0;
    // API auth (enabled flag, token, admin password) lives in auth.cpp.

    nvs_handle_t h;
    char tz[64];
    if (nvs_open("sys_cfg", NVS_READONLY, &h) == ESP_OK) {
        size_t sz = sizeof(tz);
        if (nvs_get_str(h, "timezone", tz, &sz) == ESP_OK && tz[0]) {
            setenv("TZ", tz, 1);
            tzset();
        }
        nvs_close(h);
    }

    ESP_LOGI(TAG, "init: metrics=%d ap_disabled=%d", s_metrics_enabled, s_ap_disabled);
}

bool sys_metrics_enabled() { return s_metrics_enabled; }
bool sys_ap_disabled()     { return s_ap_disabled; }
bool sys_auth_enabled()    { return auth_enabled(); }   // auth.cpp owns access control

void sys_set_metrics_enabled(bool en) {
    s_metrics_enabled = en;
    nvs_set_u8_commit("sys_cfg", "metrics_en", en ? 1 : 0);
}

void sys_set_ap_disabled(bool dis) {
    s_ap_disabled = dis;
    nvs_set_u8_commit("sys_cfg", "ap_disabled", dis ? 1 : 0);
}

void sys_set_auth_enabled(bool en) { auth_set_enabled(en); }

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

size_t sys_get_api_token(char* out, size_t cap) { return auth_token_copy(out, cap); }

bool sys_rotate_api_token(char* out, size_t cap) { return auth_rotate_token(out, cap); }
