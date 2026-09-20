// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "esp_http_server.h"

// Check the web UI pack embedded in the app image (tools/pack_spa.py).
// Returns false if the firmware was built without a www-spa dist/ — pages
// then answer with a "web UI not built" note; REST and WS still work.
bool spa_mount();

// Register the SPA catchall handler against the given httpd. Must run
// AFTER all /api/* handlers so esp_http_server's prefix matching
// gives them priority.
bool spa_register(httpd_handle_t hd);
