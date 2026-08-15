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

// Register the backend with the device_backend registry. Call once at boot,
// AFTER zhac_adapter_init() and the NVS-backed stores, BEFORE anything expects
// devices to exist. Returns false if registration fails; the firmware is
// designed to keep running radio-less in that case.
//
// The Zigbee stack itself is not started here -- DeviceBackend::init does that,
// which is what zigbee_mgr_init() used to do for the ZNP path.
bool esp_zigbee_backend_register(void);
