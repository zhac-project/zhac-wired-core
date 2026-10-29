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

// Early in app_main (NVS and the log ring up): counts boots that crashed within
// two minutes of starting. After three in a row it rolls back to the other
// image when it can (does not return; the old image shows why), else returns
// true: safe mode, no Lua, MQTT or cloud link this boot (review 2026-10-24 PB-04).
bool ota_update_boot_guard();
bool ota_update_safe_mode();

// "pending" while a fresh image is being checked, "verified" once it is kept
// (or was never on trial), "rolling_back" for the seconds before the reboot.
const char* ota_update_state();

// Why the previous update was undone, or nullptr. Cleared when an update starts.
const char* ota_update_rollback_reason();

// The image is kept, but the device store's move to zhac_data could not be marked final
// yet (storage full?): retried every few seconds; the old copy stays on nvs until then.
// A restart meanwhile loses nothing (review 2026-10-28 R-03, fix review N4; 2026-10-29 N-05).
bool ota_update_store_unconfirmed();

// The same flag read straight from NVS, for boot code that runs before ota_update_init() (cloud logs). Set
// when the health deadline undid an update; it stays set until the next update starts or an image is kept.
// Reads NVS: main task only.
bool ota_update_rolled_back();

// False (and `*err` set) when an update is already running or the URL is
// not https://. The download is refused (ota.complete with err) when it is not
// this firmware, or an older release and `allow_older` is false (ota_image.h).
bool ota_update_start(const char* url, bool allow_older, const char** err);
