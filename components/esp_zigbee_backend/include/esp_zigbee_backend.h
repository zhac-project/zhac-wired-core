// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// esp_zigbee_backend -- registers esp-zigbee-lib as ZHAC's DeviceBackend.
//
// One implementation, both SKUs. The stack always runs on the host; only where
// the 802.15.4 PHY lives differs, and the lib's Kconfig selects that per target
// (NATIVE on the S31's own radio, UART_RCP to a C6 running ot_rcp on the P4).
// Nothing above zhc_adapter can tell the difference.
#pragma once

#include <cstdint>
#include <ctime>

// Register the backend with the device_backend registry. Call once at boot,
// AFTER zhac_adapter_init() and the NVS-backed stores, BEFORE anything expects
// devices to exist. Returns false if registration fails; the firmware is
// designed to keep running radio-less in that case.
//
// The Zigbee stack itself is not started here -- DeviceBackend::init does that,
// which is what zigbee_mgr_init() used to do for the ZNP path.
bool esp_zigbee_backend_register(void);

// Why the radio is down, or nullptr while it is fine. Stable strings, sent to
// the web UI as status `radio_error`:
//   "radio_crashed"     the previous boot died while starting the radio, so this
//                       boot runs without it (see the boot guard in the .cpp)
//   "radio_init_failed" the stack reported an error while starting
const char* esp_zigbee_backend_last_error(void);

// Wall-clock seconds for ZapDevice::last_seen, which the web UI reads as Unix
// time (the dual-chip build's zigbee_mgr does the same). 0 until SNTP has set
// the clock, i.e. before 2020-01-01; callers keep the previous value then, and
// the UI shows "—" for a device never seen with a set clock.
inline uint32_t esp_zigbee_backend_wall_clock_s(void) {
    const time_t t = time(nullptr);
    return t >= 1577836800 ? static_cast<uint32_t>(t) : 0;
}
