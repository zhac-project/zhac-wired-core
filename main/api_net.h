// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "esp_http_server.h"

// Registers:
//   GET  /api/net/status      -- wired-native link + IP snapshot
//   GET  /api/wifi/status     -- compatibility shim for the shared www-spa
//   GET  /api/wifi            -- alias of the above
//   GET  /api/wifi/scan       -- always {"networks":[]}
//   POST /api/wifi/connect    -- 501
//   POST /api/wifi/disconnect -- 501
//   POST /api/wifi            -- 501
bool api_net_register(httpd_handle_t hd);
