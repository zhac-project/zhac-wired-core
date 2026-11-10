// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>

// Wi-Fi station through an ESP-Hosted co-processor (Kconfig ZHAC_WIFI_HOSTED).
// Ethernet stays the preferred uplink: the station's route priority is below
// Ethernet's, so with both up, traffic leaves on the cable.

// Call once after eth_start() (esp_netif and the event loop exist by then).
// Brings the station up on its own task and returns at once: a missing or
// silent co-processor logs an error and leaves Wi-Fi off, never stalls boot.
// `ap_disabled` (may be null) is asked before the setup access point opens and
// while it is up; true keeps it shut.
void wifi_hosted_start(bool (*ap_disabled)());

struct WifiStatus {
    bool   ready;        // co-processor answered, station started
    bool   configured;   // credentials stored
    bool   connected;    // associated and holding an IPv4 address
    char   ssid[33];
    char   ip[16];
    int8_t rssi;         // dBm, 0 when not connected
};
void wifi_hosted_status(WifiStatus* out);

struct WifiAp {
    char   ssid[33];
    int8_t rssi;
    bool   open;
};
// Blocking scan, a few seconds. Returns the number written, or -1 when the
// co-processor is not up. Hidden networks are left out.
int wifi_hosted_scan(WifiAp* out, size_t max);

// Store the credentials in the P4's NVS and join now. An empty ssid forgets
// the network. False on bad lengths (ssid 1..32, password <= 64) or NVS error.
bool wifi_hosted_connect(const char* ssid, const char* pass);
