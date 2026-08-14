// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// api_remote — control plane for the optional outbound remote-WSS client.
// Mirrors net-core's api_remote (status / connect / disconnect). The whole
// feature is gated by CONFIG_ZHAC_REMOTE_CLIENT_ENABLE; when off these are
// no-op stubs so the unconditional callers (main, ws_bridge) still link.
#pragma once
#include "esp_http_server.h"
#include <cstddef>

bool   api_remote_register(httpd_handle_t hd);

// Shared with the WS command handlers (ws_bridge.cpp):
size_t remote_status_json(char* out, size_t cap);                       // {enabled,state,...}
bool   remote_connect_req(const char* body, size_t len);                // save creds + enable
bool   remote_disconnect_req(const char* body, size_t len, bool* forget_out);
