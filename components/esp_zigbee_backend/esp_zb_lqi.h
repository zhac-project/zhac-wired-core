// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// esp_zb_lqi -- link quality for the UI, derived from RSSI. No ESP dependencies.
//
// The LQI byte Espressif's 802.15.4 radio appends to every frame (passed up by
// esp-zigbee as ezb_apsde_data_ind_t.lqi) sits at 9..11 whatever the signal:
// -30 dBm next to the hub and -94 dBm across the house read the same
// (esp-idf#17734, open). Every device showed LQI ~10, which on the usual 0..255
// scale reads as a dying link.
//
// So map RSSI instead, linearly from -100 dBm (about the radio's sensitivity:
// ESP32-C6 -104, H2 -102.5) up to -36 dBm and stronger (a perfect link) onto
// 0..255 -- the window zigbee-on-host's notes give for EFR32 coordinators, so
// the numbers read like the ones zigbee2mqtt users know. The ZNP path keeps
// the LQI the TI radio computes itself. See test/host/.
#pragma once

#include <cstdint>

namespace zhc_zb {

inline uint8_t lqi_from_rssi(int8_t rssi_dbm) {
    constexpr int kFloor = -100;
    constexpr int kTop   = -36;
    if (rssi_dbm <= kFloor) return 0;
    if (rssi_dbm >= kTop) return 255;
    return static_cast<uint8_t>((rssi_dbm - kFloor) * 255 / (kTop - kFloor));
}

}  // namespace zhc_zb
