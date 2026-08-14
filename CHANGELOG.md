<!--
SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# Changelog

All notable changes to `zhac-wired-core` are recorded here. Format follows the convention
used across the other ZHAC repos: an `## [Unreleased]` section accumulates work, and its
contents become the release-tag annotation at `just release`.

## [Unreleased]

Initial firmware. Nothing hardware-verified yet.

### Added

- **Repo skeleton targeting `esp32p4`** — 16 MB flash, single `factory` app slot, no
  `phy_init` partition (it holds Wi-Fi PHY calibration data, meaningless in a build with no
  radio on the host). PSRAM mandatory, with `SPIRAM_RODATA` and external BSS placement.
- **`CONFIG_ESP32P4_REV_MIN_0=y`** so the rev-v1.3 bench part is accepted. IDF v6.0's
  default minimum revision rejects it at flash time, not build time.
- **Wired Ethernet (`eth.cpp`)** — internal EMAC plus an external IP101 PHY over RMII, with
  `esp_netif` and the DHCP client. Exposes `eth_start()`, `eth_link_up()` and
  `eth_get_status(NetStatus*)`.
  - The PHY power-enable GPIO is driven high and allowed to settle *before* the PHY is
    probed. Skipping this makes SMI reads return all-ones and PHY detection fail with a
    misleading "no PHY found".
  - `REF_CLK` is sourced from the PHY (`EMAC_CLK_EXT_IN`). It must stay that way: with PSRAM
    enabled the EMAC and PSRAM share the MPLL, and at 80 MHz PSRAM speed no integer divisor
    produces 50 MHz within tolerance, so `EMAC_CLK_OUT` fails EMAC init outright.
  - `NetStatus` carries link, IP, netmask, gateway, MAC, speed and duplex — and
    deliberately no SSID or RSSI field, so Wi-Fi assumptions cannot survive in callers.
- **Board pins as Kconfig** (`Kconfig.projbuild`) for the Guition JC-ESP32P4-M3-DEV. The
  Phase-1 radio pins are declared now, unused, so the board wiring lives in one place.

### Notes

- **No radio in this phase, by decision.** The Zigbee stack arrives in Phase 1 as
  `esp_zigbee_backend` (`esp-zigbee-lib` on the P4 in `ESP_ZIGBEE_RADIO_MODE_UART_RCP`,
  driving a C6 running stock `ot_rcp`). Building the legacy CC2652/ZNP path here would mean
  debugging and then discarding a radio this SKU will never ship.
- **No Wi-Fi, ever, in this repo** — `esp_wifi` is absent from every `REQUIRES` list.
- Supersedes the 2026-05-27 Path-A decision in `extra/docs/ZNP_API_SURFACE_C6_PORT.md`
  **for this SKU only**; that record still governs the flagship line.
