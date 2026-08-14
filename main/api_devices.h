// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "esp_http_server.h"

// Registers GET /api/devices + POST /api/permit_join + POST /api/device_state.
bool api_devices_register(httpd_handle_t hd);
