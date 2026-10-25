// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "esp_http_server.h"

// Review 2026-10-24 PB-05: the task watchdog now resets the hub
// (CONFIG_ESP_TASK_WDT_PANIC), but the httpd task and the Zigbee stack run
// inside library code that cannot feed it. This task, itself watched by the
// task watchdog, checks that each still answers: the web server runs a queued
// probe, and the Zigbee stack's heartbeat (esp_zigbee_backend_heartbeat_us)
// moves. Silent for three minutes, the hub restarts. Start once the web server and
// the radio are up.
void supervisor_start(httpd_handle_t hd);
