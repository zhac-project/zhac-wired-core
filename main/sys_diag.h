// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// sys_diag -- the runtime diagnostics the Info page renders.
//
// One builder, two consumers (WS `status.get` and REST `/api/status`), so the
// two cannot drift. Before this existed they each hand-rolled a different
// subset with different key names, which is why the Info page showed "—" for
// almost every row: the SPA reads `cpu_c0`, `int_free`, `heap_min`… and the
// firmware was emitting `heap_internal_free`, `uptime_s`, `fw`.
#pragma once

#include "ArduinoJson.h"

// Fill `d` with the full diagnostics set the SPA's Info page expects.
//
// `cpu_ctx_slot` selects which CPU%-sampling window to use. The sampler works
// by delta against a caller-owned baseline, so two callers sharing one window
// corrupt each other's readings (zap_common/sys_metrics.h documents this).
// Each cadence therefore gets its own slot:
//   0 = the periodic status.tick push
//   1 = on-demand WS status.get / REST /api/status
enum SysDiagCpuSlot : uint8_t {
    SYS_DIAG_CPU_TICK   = 0,
    SYS_DIAG_CPU_ONDEMAND = 1,
};

void sys_diag_fill(JsonObject d, SysDiagCpuSlot cpu_ctx_slot);

// Broadcast a `status.tick` event carrying the same payload. Called from the
// main loop; without it the Info page only updates on mount and on WS
// reconnect, so CPU% would sit on whatever value it had when the tab opened.
void sys_diag_push_tick();
