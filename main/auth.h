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
bool   auth_check_token(const char* token, int fd);   // WS first-message auth; fd = the socket (failures lock out its peer)

// The cross-site filter (req_filter.h, review 2026-10-24 HS-01). `from_hub_page`
// is false for a request with a foreign Origin, or a Host that is not the
// hub's IP address or name. `allowed` applies it only while sign-in is off,
// when nothing else stands between a web page and the hub; every API route and
// the WebSocket upgrade use it. Neither answers; callers send the 403 below.
bool   auth_request_from_hub_page(httpd_req_t* req);
bool   auth_request_allowed(httpd_req_t* req);
esp_err_t auth_reply_cross_site(httpd_req_t* req);   // 403 {"error":"cross_site"}

// Register a route that needs the API token while auth is on: answers 401
// JSON instead of calling `u->handler` (403 first if the filter above refuses
// the request). Use for every route except the public ones (GET /api/status,
// /api/auth/login, /api/auth/setup, the SPA), which apply the filter themselves.
esp_err_t auth_register_uri(httpd_handle_t hd, const httpd_uri_t* u);

// POST /api/auth/login and /api/auth/setup (public), /api/auth/password.
void auth_register(httpd_handle_t hd);
