<!--
SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# zhac-wired-core

Single-chip **ESP32-P4** ZHAC firmware. The only uplink is **wired Ethernet** — there is
no Wi-Fi, no Bluetooth and no provisioning flow anywhere in this build.

It is the `zhac-mono-core` architecture (its own `main/`, direct calls through the
`mono_bridge` shim instead of HAP-over-SPI) retargeted from `esp32s3` to `esp32p4`, with
`esp_eth` in place of `esp_wifi`.

## Status

**Phase 0, in progress.** Tasks 1–2 of
`../extra/docs/plans/2026-08-14-zhac-wired-core-phase0.md` are implemented: board bring-up
and the Ethernet netif. The control surface (REST/WS/SPA), storage, rules and Lua follow in
Tasks 4–8.

**There is no radio in Phase 0**, deliberately. The Zigbee stack arrives in Phase 1 as
`esp_zigbee_backend` — `esp-zigbee-lib` on the P4 in `ESP_ZIGBEE_RADIO_MODE_UART_RCP`,
driving an ESP32-C6 running stock Espressif `ot_rcp`. Until then `/api/devices` is expected
to return an empty list; that is the correct result, not a fault.

Nothing here has been hardware-verified yet.

## Hardware

**Guition JC-ESP32P4-M3-DEV** (on the `JC-ESP32P4-M3-C6` module).

- ESP32-P4, 32 MB PSRAM, 16 MB NOR flash
- 100M Ethernet via an **IP101** PHY at SMI address 1, wired to ESP-IDF's default esp32p4
  EMAC pins — MDC 31, MDIO 52, REF_CLK-in 50, PHY power-enable 51, data 28/29/30/34/35/49
- ESP32-C6 on the same module, reachable over what were the SDIO traces
  (P4 14/15/54 ↔ C6 20/21/EN) — reserved for the Phase-1 radio

The ZHAC flagship rig (`WT0132P4-A1`) has **no Ethernet PHY** and cannot run this firmware
past the boot smoke test. `extra/docs/esp32_p4_gpio_allocation.md` describes that other
board and does not apply here.

### Silicon revision — read before flashing

ESP32-P4 comes in two **incompatible** revision families, and one Kconfig picks which one a
binary targets. A binary built for one will not boot on the other.

| `CONFIG_ESP32P4_SELECTS_REV_LESS_V3` | rev choices | `REV_MAX_FULL` |
|---|---|---|
| `y` ← **this repo**, matching `zhac-main-core` | v0.0 / v0.1 / v1.0 | 199 |
| `n` (IDF default) | v3.0 / v3.1 | 399 |

The chip config here is taken wholesale from `zhac-main-core` — the one validated on ZHAC
P4 hardware — and all eight settings resolve identically: revision family, `SPIRAM_MODE_HEX`
@ 200 MHz, 360 MHz CPU, QIO flash @ 40 MHz, 16 MB.

The accepted range is **v0.0 – v1.99**, and both ZHAC P4 boards fall inside it, so one build
serves both:

| board | revision |
|---|---|
| Guition JC-ESP32P4-M3-DEV | v1.x *(confirmed 2026-08-14)* |
| WT0132P4-A1 bench rig | v1.3 |

Neither can be targeted by the v3.x family at all, so this is not a compromise — it is the
only setting that works for ZHAC hardware.

> **Setting `CONFIG_ESP32P4_REV_MIN_0=y` without the gate is silently useless.** kconfgen
> knows the symbol but cannot select it, so it drops to the default `_301` **with no warning
> of any kind** — not even "unknown kconfig symbol". A revision mismatch then fails at
> *flash* time, long after a green build. `tools/check_resolved_config.sh` exists to catch
> exactly this, by asserting what came out rather than what went in.

## Build

```sh
source ~/.espressif/v6.0/esp-idf/export.sh
idf.py set-target esp32p4
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

## Multi-target layout

The tree is shaped for a second SoC — an **ESP32-S31-solo** SKU, where the S31's own
802.15.4 radio replaces the C6 and Ethernet is RGMII rather than RMII. That is blocked on
toolchain, not on this repo (ESP-IDF has no `esp32s31` target before v6.1; `esp-zigbee-lib`
2.0.1 ships no `esp32s31` library). See `../extra/docs/WIRED_CORE_MULTI_TARGET_DESIGN.md`
for why it stays in this repo rather than becoming a fourth firmware fork.

Three things differ per target; everything else is shared.

| Concern | Mechanism |
|---|---|
| Chip config | `sdkconfig.defaults.<target>`, auto-applied **after** `sdkconfig.defaults` |
| Ethernet PHY | one `board_eth_new()` impl per board, picked by `IDF_TARGET` in `main/CMakeLists.txt` |
| Zigbee radio | *(Phase 1)* Kconfig choice — `UART_RCP` on P4, `NATIVE` on S31 |

```
sdkconfig.defaults              shared, target-neutral
sdkconfig.defaults.esp32p4      P4: revision family, 360 MHz, QIO@40, PSRAM placement
main/eth.h                      public API — no PHY or interface in it
main/eth_common.cpp             netif, DHCP, events, status   (no #if IDF_TARGET)
main/board_eth.h                the seam: board_eth_new(&mac, &phy)
main/eth_phy_ip101_rmii.cpp     P4  / IP101 / RMII
```

> `sdkconfig.defaults` **must exist** even if it were empty — per the IDF docs, the
> per-target file is loaded *"if and only if"* the base file does. Deleting it silently
> stops every `sdkconfig.defaults.<target>` from being read.

An unrecognised target fails at CMake time with a message naming the two files to add,
rather than as a mysterious link error.

Sibling repos are **read-only inputs**, resolved via `EXTRA_COMPONENT_DIRS` and
`EMBEDDED_ZHC_PATH`: `zhac-components`, `zhac-net-core`, `zhac-main-core`, `embedded-zhc`,
`www-spa`. A change needed in one of them belongs in that repo, not here.

## Deliberate omissions

These are decisions, not gaps. `tools/check_no_rainmaker.sh` (Task 9) makes them
build-breaking rather than conventions nobody reads.

| Absent | Why |
|---|---|
| **RainMaker** | not part of this SKU |
| **Wi-Fi / `esp_wifi`** | Ethernet-only by design |
| **`znp_driver`, `ezsp_driver`, `zigbee_mgr`** | the radio here is `esp_zigbee` + `ot_rcp`; the legacy backends belong to the flagship line |
| **OTA / dual app slots** | single `factory` slot for now; a follow-up, and it changes `partitions.csv` |
| **BLE** | structurally impossible — the P4 has no radio of its own |

## Licence

AGPL-3.0-or-later, matching the other firmware cores. SPDX headers are enforced.
