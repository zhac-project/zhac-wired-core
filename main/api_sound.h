// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "esp_http_server.h"
// GET/PUT /api/sound, POST /api/sound/play and /api/sound/stop: 4 URI handlers. Without CONFIG_ZHAC_AUDIO it
// registers nothing (false), so the 404 hides the hub page's Sound card.
bool api_sound_register(httpd_handle_t hd);
