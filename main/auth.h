// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// auth — access control for the REST and WebSocket APIs. A port of the
// dual-chip S3's (zhac-net-core main.cpp + rest_ops.cpp): same NVS keys
// (`zhac_auth`), same wire credential (a 32-hex API token, `X-Api-Key` on
// REST, a first `auth` message on WS), same admin-password login that trades
// the password for the token, same per-peer lockout. Secure by default:
// CONFIG_ZHAC_API_AUTH_DEFAULT_ENABLED, and a stored choice always wins.
#pragma once

#include <cstdint>
#include <cstddef>

#include "esp_http_server.h"

void   auth_init();                              // after nvs_flash_init
bool   auth_enabled();
bool   auth_storage_error();                    // true when zhac_auth could not be opened: sign-in forced on, no password set-up
void   auth_set_enabled(bool en);                // persists; applies to REST + WS
bool   auth_password_is_set();
uint32_t auth_setup_secs_left();               // >0 only while setup is open (no password, hub booted < 10 min ago)
size_t auth_token_copy(char* out, size_t cap);
bool   auth_rotate_token(char* out, size_t cap); // out >= 33; deauths live WS clients
bool   auth_check_token(const char* token);      // WS first-message auth

// Register a route that needs the API token while auth is on: answers 401
// JSON instead of calling `u->handler`. Use for every route except the
// public ones (GET /api/status, /api/auth/login, /api/auth/setup, the SPA).
esp_err_t auth_register_uri(httpd_handle_t hd, const httpd_uri_t* u);

// POST /api/auth/login and /api/auth/setup (public), /api/auth/password.
void auth_register(httpd_handle_t hd);
