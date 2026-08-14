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

- **mDNS discovery** (`net_discovery.cpp`) — `<hostname>.local` plus an `_http._tcp`
  service record. Hostname is `CONFIG_ZHAC_MDNS_HOSTNAME` (default `zhac`); two units on
  one LAN collide and mDNS renames one, so the boot log is authoritative, not the config.
  A failed service record degrades to name-resolution-only rather than aborting.
- **The mono-core control surface**, ported: REST (`/api/devices`, `/api/rules`,
  `/api/scripts`, `/api/system`, `/api/groups`, `/api/remote`, `/api/status`), the WS
  bridge, the SPA catchall, `sys_state`, `log_ring`, `device_options`, `groups_store`,
  MQTT and the metrics exporter — plus the `hap_master`/`hap_slave`/`mono_bridge` shims
  that turn the dual-chip SPI link into direct calls.
- **`components/zigbee_pool`** — compiles just `zigbee_pool.cpp` and
  `zigbee_diagnostics.cpp` out of the sibling `zigbee_mgr`, giving a real device pool and
  diagnostics ring with no ZNP in the binary. `zigbee_mgr` itself REQUIREs `znp_driver`
  and makes ~41 direct `znp_*()` calls, so it cannot be pulled in whole; those two files
  have zero.
- **`/api/net/status`** — wired-native link, speed, duplex, IP, gateway, MAC, hostname —
  alongside a `/api/wifi/*` compatibility shim so the shared `www-spa` runs unmodified.
  The shim reports `mode="eth"` with an empty SSID and `rssi=0`, `scan` returns an empty
  list, and the write routes answer **501** (not 404: 404 reads as "wrong route", 501 as
  "this build cannot do that"). Mirrored on WS as `net.status` plus `wifi.*` aliases.
- **`radio_state.{h,cpp}`** — `radio_present()` / `radio_ok()` derived from the
  `device_backend` registry, replacing mono's direct `zigbee_mgr_crashed()` calls. Both
  are false in Phase 0 and start reporting the real backend in Phase 1 with no change at
  the call sites.
- **`tools/check_sku_invariants.sh`** — makes the three deliberate omissions
  build-breaking. Matches on usage (`#include`, symbol calls, `CONFIG_` symbols) and
  strips comment lines first, so the comments documenting each absence do not trip the
  guard explaining them. Verified in both directions: passes clean, and catches a planted
  `#include "esp_wifi.h"` / `znp_driver_init()`.
- **CI** — sibling checkouts laid out side by side, the guard, an `esp32p4` build, and a
  job that fails on `unknown kconfig symbol` (see below).

### Notes

- **No radio in this phase, by decision.** The Zigbee stack arrives in Phase 1 as
  `esp_zigbee_backend` (`esp-zigbee-lib` on the P4 in `ESP_ZIGBEE_RADIO_MODE_UART_RCP`,
  driving a C6 running stock `ot_rcp`). Building the legacy CC2652/ZNP path here would mean
  debugging and then discarding a radio this SKU will never ship.
- **No Wi-Fi, ever, in this repo** — `esp_wifi` is absent from every `REQUIRES` list.
- Supersedes the 2026-05-27 Path-A decision in `extra/docs/ZNP_API_SURFACE_C6_PORT.md`
  **for this SKU only**; that record still governs the flagship line.
