// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sys_state.h"

#include <atomic>
#include <cstdio>
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
static char s_tz[64]          = "";   // as applied; "" = UTC
static bool s_metrics_mqtt    = false;
static int  s_metrics_mqtt_s  = 60;
static std::atomic<bool> s_remote_scripts{false};   // read on task_remote, written on the httpd task

static_assert(sys_clamp_metrics_interval(0) == 1 && sys_clamp_metrics_interval(-5) == 1 &&
              sys_clamp_metrics_interval(1) == 1 && sys_clamp_metrics_interval(30) == 30 &&
              sys_clamp_metrics_interval(60) == 60 && sys_clamp_metrics_interval(61) == 60 &&
              sys_clamp_metrics_interval(100000) == 60,
              "metrics interval clamps to 1..60 s");

// A POSIX TZ string is printable ASCII without spaces or quotes
// ("<+0530>-5:30"), as zap_ntp_host_ok has it for the time server. Anything
// else could not be a timezone, and /api/status would carry it into JSON.
static bool tz_ok(const char* tz) {
    for (size_t n = 0; tz[n]; n++) {
        const unsigned char c = static_cast<unsigned char>(tz[n]);
        if (c <= 0x20 || c >= 0x7F || c == '"' || c == '\\' || n + 1 >= sizeof(s_tz)) return false;
    }
    return true;
}

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
    s_metrics_mqtt    = nvs_get_u8("sys_cfg", "metrics_mqtt", 0) != 0;
    s_metrics_mqtt_s  = sys_clamp_metrics_interval(nvs_get_u8("sys_cfg", "metrics_mqtt_s", 60));
    s_remote_scripts.store(nvs_get_u8("sys_cfg", "remote_scripts", 0) != 0);
    // API auth (enabled flag, token, admin password) lives in auth.cpp.

    nvs_handle_t h;
    char tz[sizeof(s_tz)];
    if (nvs_open("sys_cfg", NVS_READONLY, &h) == ESP_OK) {
        size_t sz = sizeof(tz);
        if (nvs_get_str(h, "timezone", tz, &sz) == ESP_OK && tz[0] && tz_ok(tz)) {
            memcpy(s_tz, tz, sizeof(s_tz));
            setenv("TZ", tz, 1);
            tzset();
        }
        nvs_close(h);
    }

    ESP_LOGI(TAG, "init: metrics=%d ap_disabled=%d tz=%s metrics_mqtt=%d/%ds remote_scripts=%d", s_metrics_enabled,
             s_ap_disabled, s_tz[0] ? s_tz : "UTC", s_metrics_mqtt, s_metrics_mqtt_s, (int)s_remote_scripts.load());
}

bool sys_metrics_enabled() { return s_metrics_enabled; }
bool sys_ap_disabled()     { return s_ap_disabled; }
bool sys_auth_enabled()    { return auth_enabled(); }   // auth.cpp owns access control
static bool s_storage_error = false;
void sys_set_storage_error(bool err) { s_storage_error = err; }
bool sys_storage_error()             { return s_storage_error; }
static bool s_event_task_ok = false;
void sys_set_event_task_ok(bool ok) { s_event_task_ok = ok; }
bool sys_event_task_ok()            { return s_event_task_ok; }

void sys_set_metrics_enabled(bool en) {
    s_metrics_enabled = en;
    nvs_set_u8_commit("sys_cfg", "metrics_en", en ? 1 : 0);
}

void sys_set_ap_disabled(bool dis) {
    s_ap_disabled = dis;
    nvs_set_u8_commit("sys_cfg", "ap_disabled", dis ? 1 : 0);
}

void sys_set_auth_enabled(bool en) { auth_set_enabled(en); }

bool sys_set_timezone(const char* tz) {
    if (!tz || !tz_ok(tz)) return false;
    nvs_handle_t h;
    if (nvs_open("sys_cfg", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "timezone", tz);
        nvs_commit(h);
        nvs_close(h);
    }
    snprintf(s_tz, sizeof(s_tz), "%s", tz);
    setenv("TZ", tz, 1);
    tzset();
    return true;
}

void sys_get_timezone(char* out, size_t cap) { snprintf(out, cap, "%s", s_tz); }

bool sys_metrics_mqtt_enabled()    { return s_metrics_mqtt; }
int  sys_metrics_mqtt_interval_s() { return s_metrics_mqtt_s; }

void sys_set_metrics_mqtt_enabled(bool en) {
    s_metrics_mqtt = en;
    nvs_set_u8_commit("sys_cfg", "metrics_mqtt", en ? 1 : 0);
}

void sys_set_metrics_mqtt_interval_s(long s) {
    s_metrics_mqtt_s = sys_clamp_metrics_interval(s);
    nvs_set_u8_commit("sys_cfg", "metrics_mqtt_s", static_cast<uint8_t>(s_metrics_mqtt_s));
}

size_t sys_get_api_token(char* out, size_t cap) { return auth_token_copy(out, cap); }

bool sys_rotate_api_token(char* out, size_t cap) { return auth_rotate_token(out, cap); }

bool sys_remote_scripts() { return s_remote_scripts.load(); }

void sys_set_remote_scripts(bool on) {
    s_remote_scripts.store(on);
    nvs_set_u8_commit("sys_cfg", "remote_scripts", on ? 1 : 0);
}
