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

### Changed

- **Device library brought current with zigbee-herdsman-converters v26.101.0.**
  `embedded-zhc` is resolved by sibling path (`EMBEDDED_ZHC_PATH`), not pinned, so this
  is a rebuild rather than a code change here — but it is worth recording what the
  firmware now carries, because two of the changes alter decode behaviour for devices
  this board already talks to:
  - **A Tuya on/off datapoint wire-typed as ENUM now decodes.** The datapoint decoder
    previously required the raw value to be exactly `Bool` and silently dropped anything
    else; Tuya firmwares ship the same logical datapoint as BOOL on one batch and ENUM on
    another. Nine definitions covering TRV601 / TRV602 / TS0601_thermostat_1 were affected.
  - **`action_duration` is no longer suffixed by endpoint** on multi-endpoint devices, so
    it arrives under the name the definition and z2m both use.
  - Two Mazda TRV definitions were reading one element past the end of their datapoint
    table on every lookup miss.
  - 42 new device definitions across the v26.99.0 and v26.101.0 windows; the library is
    now 5847 definitions over 387 vendors.

  Both targets rebuilt clean against it — `esp32s31` (IDF v6.1-beta1) at 0x37e1c0 bytes,
  42% of the app partition free, and `esp32p4` (IDF v6.0). Not yet re-run on hardware.

### Added

- **Repo skeleton targeting `esp32p4`** — 16 MB flash, single `factory` app slot, no
  `phy_init` partition (it holds Wi-Fi PHY calibration data, meaningless in a build with no
  radio on the host). PSRAM mandatory, with `SPIRAM_RODATA` and external BSS placement.
- **Chip configuration taken wholesale from `zhac-main-core`** — the config validated on
  ZHAC P4 hardware. All eight chip-critical settings now resolve identically: revision
  family, `SPIRAM_MODE_HEX` @ 200 MHz, 360 MHz CPU (IDF would default to 400), QIO flash
  @ 40 MHz (IDF would default to DIO @ 80 MHz), 16 MB.
  - **`CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y` + `CONFIG_ESP32P4_REV_MIN_0=y`.** P4 has two
    incompatible revision families — v0.x–v1.x and v3.x — and that first symbol picks
    which one the binary targets. A binary built for one will not boot on the other.
    Accepted range is v0.0–v1.99, covering both ZHAC P4 boards (Guition
    JC-ESP32P4-M3-DEV v1.x, confirmed 2026-08-14; WT0132P4-A1 bench rig v1.3), so one
    build serves both. Neither is targetable by the v3.x family at all.
  - Setting `REV_MIN_0` **without** the gate is silently useless: kconfgen knows the
    symbol but cannot select it, so it drops to the default `_301` with **no warning at
    all**, not even "unknown kconfig symbol". Verified by experiment.
- **`tools/check_resolved_config.sh`** — asserts what the config *resolved to*, not what
  was requested. There are two ways a `sdkconfig.defaults` line can do nothing and only
  the first is noisy: an unknown symbol (warns) versus a known-but-unselectable one
  (silent). Grepping build output catches only the former. Verified both directions.
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

- **Shaped for a second SoC**, ahead of an ESP32-S31-solo SKU (its own 802.15.4 radio
  replacing the C6, RGMII instead of RMII). Pure refactor of the P4 build — the resolved
  `sdkconfig` is **byte-identical** to before it, and the only binary delta is +290 bytes
  of `.text` from splitting a translation unit, with **no DRAM or PSRAM change**.
  - `sdkconfig.defaults` split into a target-neutral base plus
    `sdkconfig.defaults.esp32p4`, which ESP-IDF applies automatically afterwards
    (`tools/cmake/kconfig.cmake:182`). The base file must exist even if empty — per the
    IDF docs the per-target file loads *"if and only if"* it does.
  - Ethernet split into target-neutral plumbing (`eth_common.cpp`: netif, DHCP, events,
    status) and one board implementation behind `board_eth_new()` (`board_eth.h`), picked
    by `IDF_TARGET` in `main/CMakeLists.txt`. Selecting by file rather than by `#if` means
    a build for one target cannot compile the other's code. An unrecognised target fails
    at CMake time with a message naming what to add.
  - `eth_get_status()` now reports gigabit link speed rather than collapsing anything
    non-100M to 10 — the S31 has a gigabit MAC.
  - CI is a target matrix with the IDF version pinned **per row**; the S31 row is present
    but commented, with the two toolchain blockers recorded.

### Notes

- **No radio in this phase, by decision.** The Zigbee stack arrives in Phase 1 as
  `esp_zigbee_backend` (`esp-zigbee-lib` on the P4 in `ESP_ZIGBEE_RADIO_MODE_UART_RCP`,
  driving a C6 running stock `ot_rcp`). Building the legacy CC2652/ZNP path here would mean
  debugging and then discarding a radio this SKU will never ship.
- **No Wi-Fi, ever, in this repo** — `esp_wifi` is absent from every `REQUIRES` list.
- Supersedes the 2026-05-27 Path-A decision in `extra/docs/ZNP_API_SURFACE_C6_PORT.md`
  **for this SKU only**; that record still governs the flagship line.
