// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// board_eth -- the one seam between the target-neutral Ethernet plumbing in
// eth_common.cpp and the board's specific MAC/PHY wiring.
//
// Exactly ONE implementation is compiled, selected by IDF_TARGET in
// main/CMakeLists.txt:
//
//   esp32p4  -> eth_phy_ip101_rmii.cpp    Guition JC-ESP32P4-M3-DEV
//   esp32s31 -> eth_phy_yt8531_rgmii.cpp  S31 Function-Core  (not yet written)
//
// Everything board-specific belongs behind this call: the data interface
// (RMII vs RGMII), data-plane pin assignment, REF_CLK direction, PHY power and
// reset sequencing, and any post-init PHY quirks. eth_common.cpp must stay free
// of #if IDF_TARGET.
#pragma once

#include "esp_eth.h"
#include "esp_err.h"

// Create this board's MAC and PHY instances.
//
// On success returns ESP_OK with both out-params set. On failure it logs the
// specific cause and returns an error; the caller leaves the link down rather
// than aborting, so the rest of the firmware still runs on a board with a dead
// or absent PHY.
esp_err_t board_eth_new(esp_eth_mac_t** out_mac, esp_eth_phy_t** out_phy);
