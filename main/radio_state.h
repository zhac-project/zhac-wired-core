// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// radio_state -- protocol-neutral "is there a radio, and is it working?"
//
// Replaces the direct zigbee_mgr_crashed() calls that mono-core's
// api_status.cpp and ws_bridge.cpp make. That function lives in
// zigbee_mgr.cpp, which this firmware does not compile: zigbee_mgr is
// ZNP-bound (see components/zigbee_pool/CMakeLists.txt), and this SKU's radio
// is esp-zigbee over ot_rcp, arriving in Phase 1.
//
// Both answers are derived from the device_backend registry instead, so they
// stay correct across the Phase 1 cutover with no change at the call sites:
// in Phase 0 no backend is registered and both return false; once
// esp_zigbee_backend registers itself they start reporting it.
#pragma once

// True once any device backend has registered itself. Distinguishes "no radio
// fitted" from "radio fitted but broken" -- a distinction mono-core could not
// make, because it had exactly one hard-wired backend.
bool radio_present();

// True when a backend is registered AND reports itself running. This is what
// the `zigbee_ok` field in /api/status and the WS status frame carry.
bool radio_ok();

#include <cstdint>

// ── Permit join ──────────────────────────────────────────────────────────
//
// Wraps zigbee_permit_join() and remembers when the window closes, because
// nothing else does: the backend can open the network but cannot answer "is it
// still open, and for how long?". The SPA polls exactly that to drive its
// countdown badge.
//
