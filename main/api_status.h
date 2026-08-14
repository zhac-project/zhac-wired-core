// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "esp_http_server.h"

// Register `GET /api/status` against the given httpd. Returns true on
// success. Call after ws_server_init() so the httpd handle is valid.
bool api_status_register(httpd_handle_t hd);
