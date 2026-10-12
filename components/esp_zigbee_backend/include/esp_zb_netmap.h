// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// esp_zb_netmap -- the Zigbee network map scan (spec 2026-10-12 §3, §4).
// Manual only: nothing scans until esp_zb_netmap_start() is called.
#pragma once

#include <cstddef>

// Starts a scan. nullptr when it started, else "busy" (one is running),
// "not_ready" (no Zigbee network yet) or "oom".
const char* esp_zb_netmap_start(void);

// {"scanning":bool,"progress":{"done":n,"total":m,"current":"name"},"map":{...}|null}
// in a PSRAM buffer the caller frees with heap_caps_free; *len excludes the NUL.
// nullptr when out of memory.
char* esp_zb_netmap_get_json(size_t* len);

// Called from the scan task with "net.map.progress" / "net.map.done" and the
// event's data as JSON. One listener; nullptr removes it.
typedef void (*NetmapListener)(const char* event, const char* data, size_t len);
void esp_zb_netmap_set_listener(NetmapListener fn);
