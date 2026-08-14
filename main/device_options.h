// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// device_options — per-device runtime options (occupancy_timeout /
// debounce_ms / throttle_ms), persisted to NVS namespace "zhac_opt" keyed by
// ieee, and applied to device_shadow.
//
// Single-chip parity with net-core's `api_device_options_set`: net-core
// stores the body to `zhac_opt` and sends DEVICE_OPTIONS_SET over HAP so the
// P4 device_shadow applies the three setters. Here there is no HAP hop —
// device_shadow is local, so we apply the setters directly and re-apply from
// NVS on boot. Key scheme ("%014llx") matches net-core for store compatibility.
#pragma once
#include <cstddef>
#include <cstdint>

// Parse an options JSON object (occupancy_timeout? / debounce_ms? /
// flood_protection? / throttle_ms?; any other keys ignored), apply the
// present fields to device_shadow, and persist the blob to NVS. Returns true
// if all present fields applied. `json`/`len` is the raw object body.
bool device_options_set(uint64_t ieee, const char* json, size_t len);

// Copy the stored options JSON for `ieee` into `out` (NUL-terminated).
// Returns the string length, or 0 if nothing stored (out set to "").
size_t device_options_get_json(uint64_t ieee, char* out, size_t cap);

// Boot: re-apply every persisted options blob to device_shadow. Call once
// after device_shadow_init + the device-pool restore.
void device_options_restore_all();
