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
static std::atomic<uint8_t> s_log_cloud{0};      // read by the log hook on every task, written on the httpd task
static std::atomic<uint8_t> s_remote_level{0};   // as s_remote_scripts; view until sys_state_init reads it
static std::atomic<bool> s_remote_alarm{false};

static_assert(sys_clamp_metrics_interval(0) == 1 && sys_clamp_metrics_interval(-5) == 1 &&
              sys_clamp_metrics_interval(1) == 1 && sys_clamp_metrics_interval(30) == 30 &&
              sys_clamp_metrics_interval(60) == 60 && sys_clamp_metrics_interval(61) == 60 &&
              sys_clamp_metrics_interval(100000) == 60,
              "metrics interval clamps to 1..60 s");

static_assert(sys_remote_scripts_after_save(true, true) && !sys_remote_scripts_after_save(false, true) &&
              !sys_remote_scripts_after_save(true, false) && !sys_remote_scripts_after_save(false, false),
              "remote_scripts: RAM is on only when on was asked for and saved; off always applies");

static_assert(sys_remote_level_default(true) == 2 && sys_remote_level_default(false) == 0,
              "remote_level: a linked hub keeps full, every other one starts at view (R18)");
static_assert(sys_remote_level_after_save(0, 2, true) == 2 && sys_remote_level_after_save(0, 2, false) == 0 &&
              sys_remote_level_after_save(2, 0, false) == 0 && sys_remote_level_after_save(2, 1, false) == 1,
              "remote_level: a raise only once saved, a lowering at once");

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

// ESP_OK once the value is committed, else the first NVS error. The flags below keep ignoring it, as they
// always did; the cloud settings (remote_scripts, remote_alarm, remote_level, log_cloud) must not.
static esp_err_t nvs_set_u8_commit(const char* ns, const char* key, uint8_t v) {
    nvs_handle_t h;
    esp_err_t e = nvs_open(ns, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_set_u8(h, key, v);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

// remote_client's token (zhac_remote/token, remote_nvs.cpp): the hub has been linked to the cloud.
static bool cloud_token_stored() {
    nvs_handle_t h;
    if (nvs_open("zhac_remote", NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = 0;
    const bool stored = nvs_get_str(h, "token", nullptr, &len) == ESP_OK && len > 1;   // len counts the NUL
    nvs_close(h);
    return stored;
}

// The cloud-control level (HL-03, R18). No key yet (ESP_ERR_NVS_NOT_FOUND, also for no sys_cfg at all): the first
// boot of this firmware, which writes the default once, so the owner's later choice is never overwritten and a hub
// linked later stays at view. The default is a raise from view like any other: it applies once saved, and a failed
// write leaves view and is tried again at the next boot. Any other read error (NVS not usable) leaves view.
static uint8_t load_remote_level() {
    uint8_t v = 0;
    nvs_handle_t h;
    esp_err_t e = nvs_open("sys_cfg", NVS_READONLY, &h);
    if (e == ESP_OK) {
        e = nvs_get_u8(h, "remote_level", &v);
        nvs_close(h);
    }
    if (e == ESP_OK) return v <= 2 ? v : 0;   // a value no firmware writes counts as view
    if (e != ESP_ERR_NVS_NOT_FOUND) return 0;
    const bool linked = cloud_token_stored();
    const uint8_t def = sys_remote_level_default(linked);
    const esp_err_t w = nvs_set_u8_commit("sys_cfg", "remote_level", def);
    if (w != ESP_OK) ESP_LOGE(TAG, "remote_level=%u not saved: %s", (unsigned)def, esp_err_to_name(w));
    const uint8_t level = sys_remote_level_after_save(0, def, w == ESP_OK);
    ESP_LOGW(TAG, "cloud control: %s on this firmware's first start (%s)", sys_remote_level_name(level),
             linked ? "linked to the cloud" : "not linked");
    return level;
}

void sys_state_init() {
    s_metrics_enabled = nvs_get_u8("sys_cfg", "metrics_en", 0) != 0;
    s_ap_disabled     = nvs_get_u8("sys_cfg", "ap_disabled", 0) != 0;
    s_metrics_mqtt    = nvs_get_u8("sys_cfg", "metrics_mqtt", 0) != 0;
    s_metrics_mqtt_s  = sys_clamp_metrics_interval(nvs_get_u8("sys_cfg", "metrics_mqtt_s", 60));
    s_remote_scripts.store(nvs_get_u8("sys_cfg", "remote_scripts", 0) != 0);
    s_remote_alarm.store(nvs_get_u8("sys_cfg", "remote_alarm", 0) != 0);
    s_remote_level.store(load_remote_level());
    {
        const uint8_t lc = nvs_get_u8("sys_cfg", "log_cloud", 0);
        s_log_cloud.store(lc <= 3 ? lc : 0);
    }
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

    ESP_LOGI(TAG, "init: metrics=%d ap_disabled=%d tz=%s metrics_mqtt=%d/%ds remote_scripts=%d log_cloud=%u "
             "remote_level=%s remote_alarm=%d",
             s_metrics_enabled, s_ap_disabled, s_tz[0] ? s_tz : "UTC", s_metrics_mqtt, s_metrics_mqtt_s,
             (int)s_remote_scripts.load(), (unsigned)s_log_cloud.load(), sys_remote_level_name(s_remote_level.load()),
             (int)s_remote_alarm.load());
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

// The cloud switches (remote_scripts, remote_alarm): saved first, on only once saved.
static bool set_cloud_switch(std::atomic<bool>& s, const char* key, bool on) {
    const esp_err_t e = nvs_set_u8_commit("sys_cfg", key, on ? 1 : 0);
    if (e != ESP_OK) ESP_LOGE(TAG, "%s=%d not saved: %s", key, (int)on, esp_err_to_name(e));
    s.store(sys_remote_scripts_after_save(on, e == ESP_OK));
    return e == ESP_OK;
}

bool sys_set_remote_scripts(bool on) { return set_cloud_switch(s_remote_scripts, "remote_scripts", on); }

bool sys_remote_alarm() { return s_remote_alarm.load(); }

bool sys_set_remote_alarm(bool on) { return set_cloud_switch(s_remote_alarm, "remote_alarm", on); }

static constexpr const char* kRemoteLevelNames[] = {"view", "devices", "full"};

const char* sys_remote_level_name(uint8_t level) { return kRemoteLevelNames[level <= 2 ? level : 0]; }

int sys_remote_level_parse(const char* name) {
    for (int i = 0; name && i < 3; i++)
        if (strcmp(name, kRemoteLevelNames[i]) == 0) return i;
    return -1;
}

uint8_t sys_remote_level() { return s_remote_level.load(); }

bool sys_set_remote_level(uint8_t level) {
    if (level > 2) return false;
    const esp_err_t e = nvs_set_u8_commit("sys_cfg", "remote_level", level);
    if (e != ESP_OK) ESP_LOGE(TAG, "remote_level=%u not saved: %s", (unsigned)level, esp_err_to_name(e));
    s_remote_level.store(sys_remote_level_after_save(s_remote_level.load(), level, e == ESP_OK));
    return e == ESP_OK;
}

uint8_t sys_log_cloud() { return s_log_cloud.load(std::memory_order_relaxed); }

bool sys_set_log_cloud(uint8_t level) {
    if (level > 3) return false;
    const esp_err_t e = nvs_set_u8_commit("sys_cfg", "log_cloud", level);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "log_cloud=%u not saved: %s", (unsigned)level, esp_err_to_name(e));
        return false;
    }
    s_log_cloud.store(level);
    return true;
}
