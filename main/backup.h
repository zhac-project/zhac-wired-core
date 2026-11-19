// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Full backup and restore (research 2026-11-18 C1): the whole hub (network, devices, rules, scripts, settings,
// sign-in) as one password-encrypted file. The core is backup_core.h; this is its esp_partition and web side.
#pragma once

#include <cstdint>

#include "esp_http_server.h"

// First thing in app_main, before anything opens the storage: writes a restore staged by POST /api/backup/restore
// over the partitions. Returns the NWK frame counter the restored network must send above (0: none).
uint32_t backup_apply_pending();

// Once the log ring captures: logs again what backup_apply_pending() did, so the Logs page shows it.
void backup_log_outcome();

// POST /api/backup/full and POST /api/backup/restore.
bool backup_register(httpd_handle_t hd);
