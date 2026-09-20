// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// Over-the-air update for this single-chip build: WS `ota.update {url}`
// downloads an app image (a release's `-ota.bin`) over HTTPS into the idle
// OTA slot and reboots into it. The web UI is embedded in the app, so it is
// replaced too. Progress goes out as `ota.start` / `ota.progress` /
// `ota.complete` with target "self".
// Starts the post-update health check when the running image is still on
// trial (see ota_update.cpp). Call once the web server and radio are up.
void ota_update_init();

// "pending" while a fresh image is being checked, "verified" once it is kept
// (or was never on trial), "rolling_back" for the seconds before the reboot.
const char* ota_update_state();

// Why the previous update was undone, or nullptr. Cleared when an update starts.
const char* ota_update_rollback_reason();

// False (and `*err` set) when an update is already running or the URL is
// not https://.
bool ota_update_start(const char* url, const char** err);
