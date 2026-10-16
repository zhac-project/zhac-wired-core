// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// "Log to cloud" on the hub (spec 2026-10-06 cloud logs §4). See log_cloud.cpp.
#pragma once

#include <cstdint>

#include "log_cloud_core.h"

// Boot: takes the previous run's restart tail out of no-init RAM, starts a fresh one, reads a fresh core
// dump's summary (once per dump) and allocates the backlog. Call from app_main after sys_state_init() and
// before log_ring_init(): the hook writes the tail from its first line.
void log_cloud_boot();

// The log hook's call for every line. It only copies, under a spinlock: it never sends, logs, allocates or
// blocks, so no line can re-enter it.
void log_cloud_capture(char level, const char* tag, const char* msg, int64_t up_us);

// Starts the uploader task; call once, after remote_client_init().
void log_cloud_start();

// settings.set changed the level: "off" empties the backlog at once.
void log_cloud_on_setting();

// Just before a planned esp_restart(): tells the next run why.
void log_cloud_planned_restart(logcloud::Planned p);

// Lines handed to the cloud link, and lines the backlog dropped, since the hub started.
void log_cloud_stats(uint32_t* sent, uint32_t* dropped);
