// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "ArduinoJson.h"
#include "esp_http_server.h"

// Fill `d` with the full status snapshot. One builder for both transports:
// REST `GET /api/status` and WS `status.get` return the same keys, so the
// web UI's Settings and Info pages see the same values whichever it asked.
// `sku` = "wired" lets the shared web UI hide what this build cannot do
// (Wi-Fi, RainMaker uplink, a second chip).
void api_status_fill(JsonObject d);

// The MQTT metrics stream's payload (metrics_mqtt.cpp): the Info page's
// Resources figures (cpu_c0 cpu_c1 heap heap_min int_free int_min int_blk
// psram_free psram_min psram_blk psram_total stack_hwm stack_hwm_task uptime)
// plus device_count, link_up and, with the cloud client built in,
// remote_state -- the same keys and values /api/status carries.
void api_status_fill_metrics(JsonObject d);

// Register `GET /api/status` against the given httpd. Returns true on
// success. Call after ws_server_init() so the httpd handle is valid.
bool api_status_register(httpd_handle_t hd);
