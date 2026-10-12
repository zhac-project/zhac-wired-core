// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// esp_zb_netmap_json -- the map object of spec 2026-10-12 §3.3, built from a
// finished walk. Pure (ArduinoJson only), host-tested.
#pragma once

#include "ArduinoJson.h"
#include "esp_zb_netmap_walk.h"

#include <cstdint>

namespace zhc_zb::netmap {

// Fills `doc` (cleared first). `ts` is epoch s (0 = clock not set). Names and
// models are copied into the document, so `w.known()` may go away afterwards.
void build_json(JsonDocument& doc, const Walk& w, uint32_t ts, uint32_t duration_ms);

}  // namespace zhc_zb::netmap
