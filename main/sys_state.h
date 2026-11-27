// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// sys_state — mono-local equivalent of net-core's s3_internal system flags +
// API-auth token. In the dual chip these lived as file-statics in
// api_system.cpp / main.cpp; here they get their own small module so the
// settings + status + token handlers can share them.
//
// NVS: namespace "sys_cfg"  keys: metrics_en (u8), ap_disabled (u8), timezone (str),
//                                 metrics_mqtt (u8), metrics_mqtt_s (u8), remote_scripts (u8),
//                                 log_cloud (u8), remote_level (u8), remote_alarm (u8)
//      namespace "zhac_auth" keys: enabled (u8), token (str, 33 incl NUL)
#pragma once
#include <cstddef>
#include <cstdint>

// Load flags + token from NVS and apply auth to ws_server. Call once at boot
// after nvs_flash_init.
void sys_state_init();

bool sys_metrics_enabled();
bool sys_ap_disabled();
bool sys_auth_enabled();
// Set by main when the NVS partition could not be initialised: the hub runs
// locked and empty until the owner erases storage (system.storage_reset).
void sys_set_storage_error(bool err);
bool sys_storage_error();
// Set by main once TaskEventBus runs; false means rules, MQTT and the web UI
// see no device events. Read by the post-update health check.
void sys_set_event_task_ok(bool ok);
bool sys_event_task_ok();

void sys_set_metrics_enabled(bool en);   // persists sys_cfg/metrics_en
void sys_set_ap_disabled(bool dis);      // persists sys_cfg/ap_disabled
void sys_set_auth_enabled(bool en);      // persists zhac_auth/enabled + applies to ws_server

// Timezone: a POSIX TZ string ("EET-2EEST,M3.5.0/3,M10.5.0/4") -- printable
// ASCII without spaces or quotes, under 64 bytes -- or "": never set, UTC.
bool sys_set_timezone(const char* tz);   // persists sys_cfg/timezone + setenv("TZ"); false = refused
void sys_get_timezone(char* out, size_t cap);   // what is applied, "" if nothing

// "Stream metrics to MQTT" (metrics_mqtt.cpp): on/off and seconds between
// publishes, 1..60, 60 until set.
constexpr int sys_clamp_metrics_interval(long s) { return s < 1 ? 1 : s > 60 ? 60 : static_cast<int>(s); }
bool sys_metrics_mqtt_enabled();
int  sys_metrics_mqtt_interval_s();
void sys_set_metrics_mqtt_enabled(bool en);     // persists sys_cfg/metrics_mqtt
void sys_set_metrics_mqtt_interval_s(long s);   // clamps, persists sys_cfg/metrics_mqtt_s

// "Allow script changes from the cloud" (spec 2026-10-05 §3.1): sys_cfg/remote_scripts (u8 0/1), missing =
// off, so a storage reset turns it off. Changed only from this hub's own page (settings.set / POST
// /api/settings); the cloud relay refuses that key (local_only). Read by the relay's gate on task_remote.
bool sys_remote_scripts();
// Saves to NVS first, then RAM takes `on && saved` (below): OFF applies at once even when the save fails, ON
// only once it is saved, so RAM never says on while NVS does not. false = the save failed. After a failed OFF
// NVS may still say on and a reboot would bring it back, so the caller must tell the user.
bool sys_set_remote_scripts(bool on);
constexpr bool sys_remote_scripts_after_save(bool on, bool saved) { return on && saved; }

// "Cloud control" (review 2026-11-26 HL-03, ruling R18): how far the cloud reaches, 0 view, 1 devices, 2 full
// (remote_level_t). NVS sys_cfg/remote_level. The first boot of a firmware with it writes
// sys_remote_level_default(a cloud token is stored) once: a hub already linked keeps what it had, every other one
// (new, storage reset, or linked later) starts at view. Changed only from this hub's own page (the relay answers
// local_only); read by the relay's gate on task_remote for every command, so a change applies to the next one.
constexpr uint8_t sys_remote_level_default(bool cloud_token_stored) { return cloud_token_stored ? 2 : 0; }
const char* sys_remote_level_name(uint8_t level);   // "view", "devices", "full"; above 2 counts as "view"
int sys_remote_level_parse(const char* name);       // the level a name gives (case matters), -1 if none or null
uint8_t sys_remote_level();
// Saves to NVS first; RAM takes the level by the rule below: a lowering applies at once even when the save fails,
// a raise only once saved, so RAM never lets the cloud do more than NVS does. false = the save failed (or the level
// is not 0..2). After a failed lowering NVS may still hold the higher level, which a reboot brings back, so the
// caller must tell the user.
bool sys_set_remote_level(uint8_t level);
constexpr uint8_t sys_remote_level_after_save(uint8_t was, uint8_t want, bool saved) {
    return (saved || want < was) ? want : was;
}

// "Allow the cloud to disarm the alarm" (HL-02, R19): sys_cfg/remote_alarm (u8 0/1), missing = off on every hub,
// so a storage reset turns it off. Off, the cloud may only arm the Alarm device. Same rules as remote_scripts:
// changed only from this hub's own page, on only once saved (sys_remote_scripts_after_save), false = not saved.
bool sys_remote_alarm();
bool sys_set_remote_alarm(bool on);

// "Log to cloud" (spec 2026-10-06 cloud logs §4.1): 0 off, 1 errors, 2 warnings, 3 everything (logcloud::Level).
// NVS sys_cfg/log_cloud, missing = off, so a storage reset turns it off.
uint8_t sys_log_cloud();
// Saves to NVS first; RAM takes the new level only once it is saved. A failed save refuses and leaves the
// running level unchanged (spec §4.1). false = the save failed (or the level is not 0..3).
bool sys_set_log_cloud(uint8_t level);

// Copy the current API token into out (33 bytes incl NUL). Returns length.
size_t sys_get_api_token(char* out, size_t cap);

// Generate a fresh 32-hex-char token, persist it, apply to ws_server if auth
// is enabled, and copy it into out (cap >= 33). Returns true on success.
bool sys_rotate_api_token(char* out, size_t cap);
