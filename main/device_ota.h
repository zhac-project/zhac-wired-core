// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Zigbee device firmware update from the hub page (research 2026-11-18 C2): the user uploads a device's OTA file,
// the hub holds it in PSRAM and the stack's OTA server serves it to that device only. Rules: device_ota_core.h;
// the stack side: components/esp_zigbee_backend/esp_zb_ota.cpp.
#pragma once

#include "esp_http_server.h"

// POST /api/device/ota?ieee=0x..., GET /api/device/ota, POST /api/device/ota/cancel; and the stack's callbacks.
bool device_ota_register(httpd_handle_t hd);

// From the main loop: ends an update whose device left the hub, and takes an ended update's image out of the stack.
void device_ota_tick();
