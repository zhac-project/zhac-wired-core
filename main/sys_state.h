// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// sys_state — mono-local equivalent of net-core's s3_internal system flags +
// API-auth token. In the dual chip these lived as file-statics in
// api_system.cpp / main.cpp; here they get their own small module so the
// settings + status + token handlers can share them.
//
// NVS: namespace "sys_cfg"  keys: metrics_en (u8), ap_disabled (u8), timezone (str)
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

void sys_set_metrics_enabled(bool en);   // persists sys_cfg/metrics_en
void sys_set_ap_disabled(bool dis);      // persists sys_cfg/ap_disabled
void sys_set_auth_enabled(bool en);      // persists zhac_auth/enabled + applies to ws_server
void sys_set_timezone(const char* tz);   // persists sys_cfg/timezone + setenv("TZ")

// Copy the current API token into out (33 bytes incl NUL). Returns length.
size_t sys_get_api_token(char* out, size_t cap);

// Generate a fresh 32-hex-char token, persist it, apply to ws_server if auth
// is enabled, and copy it into out (cap >= 33). Returns true on success.
bool sys_rotate_api_token(char* out, size_t cap);
