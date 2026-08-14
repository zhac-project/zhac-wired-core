// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstdint>
#include <cstddef>

// In-memory ring of the last N log lines captured via esp_log_set_vprintf().
// Used by the /api/logs REST endpoint so the web-UI can display recent
// firmware output without a serial console. Sized for PSRAM residency.
//
// Also fans out each captured line to optional live sinks — MQTT
// publish + WebSocket broadcast — gated by runtime flags so zero cost
// applies when both are disabled (the default).

void log_ring_init();

// Serialise the ring into `out` as a JSON object
//   {"logs":["<line1>","<line2>",...]}
// in chronological order (oldest first). Returns the number of bytes
// written (up to cap-1, always NUL-terminated). Returns 0 on failure
// (including truncation so large payloads don't produce invalid JSON).
size_t log_ring_to_json(char* out, size_t cap);

// ── Live sinks ──────────────────────────────────────────────────────
//
// Sinks fire for every captured line (S3 + P4-relayed). Both default
// OFF on boot — operator opts in via Settings. Level is one of
// 'E','W','I','D','V' and lines below the threshold are dropped for
// that sink only (ring + /api/logs are unaffected).

void log_sinks_set_mqtt(bool enabled, char min_level);
void log_sinks_set_ws  (bool enabled, char min_level);

bool log_sinks_get_mqtt_enabled();
bool log_sinks_get_ws_enabled();
char log_sinks_get_mqtt_level();
char log_sinks_get_ws_level();

// Telemetry counters (for /api/status + soak tests).
uint32_t log_sinks_mqtt_published();
uint32_t log_sinks_mqtt_dropped_rate();
uint32_t log_sinks_mqtt_dropped_disconnected();

// Explicit P4-sourced log entry. Called from hap_bridge's LOG_LINE
// handler so the `src` field on MQTT/WS reads "p4" instead of "s3".
// Writes to the ring AND to the live sinks.
void log_sinks_publish_p4(char level, const char* tag, const char* msg);
