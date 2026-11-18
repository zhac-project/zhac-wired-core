// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstdint>

#include "esp_http_server.h"

// Registers GET /api/devices + POST /api/permit_join + POST /api/device_state.
bool api_devices_register(httpd_handle_t hd);
// Queues a manual configure of a known, interviewed device (WS device.configure and
// POST /api/device/configure share it); false otherwise. See api_devices.cpp.
bool device_configure_request(uint64_t ieee);
