// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// api_system — settings, API-token rotation, and Zigbee diagnostics for mono.
// Single-chip parity with net-core's api_system.cpp: settings + token are
// purely local (NVS + service config); diagnostics reads zb_diag_snapshot()
// directly instead of round-tripping DIAG_UNHANDLED to P4.
//
// Deferred to the log_ring cluster (#89): logs endpoint + log_mqtt/log_ws
// settings fields, and the alerts ring.
#pragma once
#include "esp_http_server.h"
#include <cstddef>

// Registers POST /api/settings, POST /api/token/rotate,
// GET /api/diagnostics/unhandled.
bool api_system_register(httpd_handle_t hd);

// Shared with the WS command handlers (ws_bridge.cpp):
bool   system_apply_settings(const char* json, size_t len);  // partial update; true if parsed
bool   system_rotate_token(char* out, size_t cap);           // out >= 33; true on success
size_t system_diagnostics_json(char* out, size_t cap);       // {"entries":[...]}
