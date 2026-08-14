// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// api_groups — Zigbee device groups (NVS-backed) for mono. CRUD ports
// net-core's api_groups verbatim (no P4); group.cmd fans the command out to
// each member via direct zhac_adapter_send_* instead of a SET_ATTRIBUTE HAP
// roundtrip per member.
#pragma once
#include "esp_http_server.h"
#include <cstddef>

bool api_groups_register(httpd_handle_t hd);

// Shared with the WS command handlers (ws_bridge.cpp). REST + WS both call
// these; the caller supplies the raw JSON body/args.
size_t group_list_json(char* out, size_t cap);                              // {"groups":[...]}
bool   group_create(const char* body, size_t len, char* out, size_t cap, size_t* n);
bool   group_get(const char* body, size_t len, char* out, size_t cap, size_t* n);     // false = 404
bool   group_update(const char* body, size_t len, char* out, size_t cap, size_t* n);  // false = 404
bool   group_delete_req(const char* body, size_t len);
size_t group_cmd(const char* body, size_t len, char* out, size_t cap);      // {"ok",..,"sent","failed"}
