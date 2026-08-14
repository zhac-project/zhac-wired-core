// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "esp_http_server.h"

// Mount the SPIFFS partition labelled `spa` at /spa. Safe to call once
// at boot; idempotent on success. Returns true iff mounted.
bool spa_mount();

// Register the SPA catchall handler against the given httpd. Must run
// AFTER all /api/* handlers so esp_http_server's prefix matching
// gives them priority.
bool spa_register(httpd_handle_t hd);
