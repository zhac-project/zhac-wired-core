// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstdint>

// Bring up the internal EMAC + external RMII PHY and attach it to
// esp_netif with the default DHCP client. Safe to call once, from
// app_main, after nvs_flash_init. Never aborts: a missing or dead PHY
// logs an error and leaves the link down so the rest of the firmware
// (rules, local logging, and later the radio) still runs.
void eth_start();

// Snapshot of link + IP state. No SSID/RSSI -- this is copper.
struct NetStatus {
    bool     link_up;
    bool     has_ip;
    char     ip[16];        // dotted quad, empty until DHCP completes
    char     netmask[16];
    char     gw[16];
    char     mac[18];       // aa:bb:cc:dd:ee:ff
    uint32_t speed_mbps;    // 10 or 100, 0 when down
    bool     duplex_full;

    // IPv6. Both empty until the link is up; `global` stays empty on networks
    // with no router advertisements (most NAT test benches), which is normal
    // and not an error -- link-local alone is enough for same-segment peers.
    //
    // 46 = INET6_ADDRSTRLEN: 39 chars for the address, plus room for a "%iface"
    // scope suffix and the NUL.
    bool     has_ip6;
    char     ip6_link_local[46];
    char     ip6_global[46];   // first global or unique-local address seen
};
void eth_get_status(NetStatus* out);

bool eth_link_up();
