<!--
SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# zhac-wired-core

ZHAC on a single ESP32 board with an Ethernet jack. The Zigbee coordinator, the full
[embedded-zhc](https://github.com/zhac-project/embedded-zhc) device library, rules, Lua,
MQTT and the web UI all run on the board. There is no Wi-Fi, no Linux host and no cloud
account: plug in Ethernet and open **http://zhac.local**.

It is the `zhac-mono-core` architecture (its own `main/`, direct calls through the
`mono_bridge` shim instead of HAP-over-SPI) with `esp_eth` in place of `esp_wifi`, and
`esp-zigbee-lib` as the Zigbee stack.

## Status — 2026-09-18

| Target | Board | Zigbee radio | State |
|---|---|---|---|
| `esp32s31` | Espressif ESP32-S31 Function-Core | the S31's own 802.15.4 | **Runs on hardware.** Forms a network, pairs and decodes a real device, ZCL groups, permit-join, live web UI. Building it needs ESP-IDF v6.1-beta1. |
| `esp32p4` | Guition JC-ESP32P4-M3-DEV | the ESP32-C6 on the module, running `ot_rcp` | **Builds. Has not run on hardware yet.** |

Still open before anyone should rely on it:

- a join → permit-join → bind cycle on hardware since the 2026-09 SDK-lock fix;
- a first over-the-air update and a first sign-in on hardware (both built, neither run yet).

Bugs, missing devices and reports about other boards go to the
[ZHAC issue tracker](https://github.com/zhac-project/zhac-platform/issues/new/choose).

## Boards

| Board | SoC | Ethernet | Zigbee radio | Status |
|---|---|---|---|---|
| Guition JC-ESP32P4-M3-DEV (≈ $14) | ESP32-P4, silicon v1.x | IP101, 100 Mbit, RMII | on-module ESP32-C6 running `ot_rcp` | builds, not yet run |
| Espressif ESP32-S31 Function-Core | ESP32-S31 | YT8531, 1 Gbit, RGMII | the S31's own 802.15.4 | runs on hardware |
| Any other ESP32-P4 board | check the revision first | set the `ZHAC_ETH_*` pins in menuconfig | an 802.15.4 ESP chip (C6/H2) running `ot_rcp` on a spare UART | untested — [send a board report](https://github.com/zhac-project/zhac-platform/issues/new?template=board-report.yml) |

The WT0132P4-A1 board used by the dual-chip flagship has **no Ethernet PHY** and cannot
run this firmware past a boot smoke test. `extra/docs/esp32_p4_gpio_allocation.md`
describes that other board and does not apply here.

### Check your P4's silicon revision before flashing

ESP32-P4 silicon comes in two families that cannot run each other's binaries: v0.x–v1.x
and v3.x. This firmware is built for **v0.x–v1.x**. Check yours:

```sh
esptool --port /dev/ttyACM0 flash-id      # esptool v4: esptool.py --port /dev/ttyACM0 flash_id
```

The chip line shows the revision:

```
Chip type:          ESP32-P4 (revision v1.3)
```

- **v0.x or v1.x** — go ahead.
- **v3.x** — this build will not boot on your board, and the failure only shows up at
  flash time. [Send a board report](https://github.com/zhac-project/zhac-platform/issues/new?template=board-report.yml)
  so a v3.x build can be added.

#### How the build pins the revision

One Kconfig picks which family a binary targets:

| `CONFIG_ESP32P4_SELECTS_REV_LESS_V3` | rev choices | `REV_MAX_FULL` |
|---|---|---|
| `y` ← **this repo**, matching `zhac-main-core` | v0.0 / v0.1 / v1.0 | 199 |
| `n` (IDF default) | v3.0 / v3.1 | 399 |

The chip config here is taken wholesale from `zhac-main-core` — the one validated on ZHAC
P4 hardware — and all eight settings resolve identically: revision family,
`SPIRAM_MODE_HEX` @ 200 MHz, 360 MHz CPU, QIO flash @ 40 MHz, 16 MB. Both ZHAC P4 boards
fall inside v0.0 – v1.99, so one build serves both:

| board | revision |
|---|---|
| Guition JC-ESP32P4-M3-DEV | v1.x *(confirmed 2026-08-14)* |
| WT0132P4-A1 bench rig | v1.3 |

> **Setting `CONFIG_ESP32P4_REV_MIN_0=y` without the gate is silently useless.** kconfgen
> knows the symbol but cannot select it, so it drops to the default `_301` **with no warning
> of any kind** — not even "unknown kconfig symbol". A revision mismatch then fails at
> *flash* time, long after a green build. `tools/check_resolved_config.sh` exists to catch
> exactly this, by asserting what came out rather than what went in.

### Zigbee radio (ESP32-C6)

The ESP32-P4 has no radio. On the Guition module the ESP32-C6 next to it is the Zigbee
radio, running Espressif's stock `ot_rcp` re-pinned to the traces that join the two
chips (`rcp/sdkconfig.defaults`). A new board ships the C6 with Wi-Fi co-processor
firmware instead, so it needs `zhac-rcp-c6-<version>.bin` from a release **once**.

The P4 cannot do this itself here: the two chips share only the C6's former SDIO pins,
while the C6's ROM bootloader listens on its UART0 pins (16/17) and needs its BOOT pin
held low — neither reaches the P4. So the C6 is flashed through its own header:

1. Stop the P4 so it stops resetting the C6: hold **BOOT**, tap **RESET**, release BOOT.
2. Wire a 3.3 V USB-serial adapter: **TX → `C6_U0RXD`**, **RX → `C6_U0TXD`**, **GND → GND**;
   leave 3.3 V unconnected. (Labels from Guition's schematic of the sibling
   JC4880P443C board, which uses the same module — check yours.)
3. Hold **`C6_IO9`** to GND while pulsing **`C6_CHIP_PU`** to GND, then:

```sh
esptool --chip esp32c6 --port /dev/ttyUSB0 erase-flash
esptool --chip esp32c6 --port /dev/ttyUSB0 write-flash 0x0 zhac-rcp-c6-<version>.bin
```

or use the **Zigbee radio** button on the [browser flasher](https://zhac-project.github.io/zhac-docs/flash/).
To build the image yourself: `tools/build-rcp.sh build.rcp/zhac-rcp-c6.bin` after sourcing
ESP-IDF v6.0.

Without it the hub still boots. The first attempt to start the radio fails and resets the
board; the next boot skips the radio, serves the web UI and reports
`radio_error: "radio_crashed"`, which the UI shows. Reset to try again.

### Guition wiring

- ESP32-P4, 32 MB PSRAM, 16 MB NOR flash
- 100M Ethernet via an **IP101** PHY at SMI address 1, wired to ESP-IDF's default esp32p4
  EMAC pins — MDC 31, MDIO 52, REF_CLK-in 50, PHY power-enable 51, data 28/29/30/34/35/49
- ESP32-C6 on the same module, reached over what were its SDIO traces
  (P4 14/15/54 ↔ C6 20/21/EN) — the Zigbee radio. The spinel UART runs C6 TX 20 →
  P4 RX 14 and P4 TX 15 → C6 RX 21, derived from the SDIO pin map and **not yet confirmed
  on hardware**: if the radio never answers, swap `ZHAC_RCP_UART_RX_GPIO`/`_TX_GPIO`.

## First boot

The step-by-step version, with pictures, is
[Your first 20 minutes](https://github.com/zhac-project/zhac-docs/blob/master/FIRST_20_MINUTES.md).

1. Connect Ethernet to a network with DHCP, then power the board.
2. Open **http://zhac.local**. If that name does not resolve, use the address your router
   assigned — the board shows up there as `zhac` — or read it from the serial console.
3. Choose the **admin password** on the first visit — until then, anyone who reaches the
   hub first could claim it.
4. Go to **Devices → + Add a device**, then hold your device's pairing button; the panel says
   when the device has joined and when the hub has read what it is.

Two ZHAC boards on one LAN collide on the name: mDNS renames the second one, and its boot
log shows the name it got.

## Access

Sign-in is on by default, as on the dual-chip build: every REST call needs the API token
(`X-Api-Key`), every WebSocket must send it in a first `auth` message, and the web UI trades
the admin password for that token. Five wrong attempts in a minute lock that address out for
the rest of the minute.

**First claim.** A hub with no password lets the first visitor set one, but only in the first
ten minutes after power-on. After that the web UI says so and asks for a power cycle: a hub
that was never set up cannot be claimed by whoever finds it on the network a month later.
Status reports the seconds left as `auth_setup_secs_left`.

If the sign-in storage cannot be opened at boot, the hub locks itself instead of opening up:
sign-in stays on with a token that exists only for that boot and is printed on the serial
console; set-up is refused until storage is reset.

Lost the password? The serial console prints the token at every boot; sign in with it
(**Login → Use API token instead**), then set a new password in Settings. Settings → *Auth*
turns sign-in off for a lab bench — then anyone on the LAN can run Lua, update the firmware
and reset the Zigbee network.

**Clock.** The boards have no real-time clock, so the hub sets its time once it has an
address: from the time server the router offers with its lease, if any, else from
`pool.ntp.org`. On a network with no internet access and a router that offers none, name a
time server on it under Settings → Time; otherwise the hub takes the time from the browser
that opens its web UI, and needs that again after every power cut. Until it has the
time, a device's "last seen" shows "—" and time-based rules (`Time#Cron`) wait.

## Build from source

The sibling repos must sit next to this one — `zhac-components`, `zhac-net-core`,
`zhac-main-core` (the Lua engine compiles its sources), `embedded-zhc` and `www-spa` — exactly
as `zhac-platform` lays them out. Build the web UI
first: `idf.py build` never runs `npm`, and without `../www-spa/dist` the firmware is built
without the web UI (the build warns).

```sh
( cd ../www-spa && npm ci && npm run build )
source ~/.espressif/v6.0/esp-idf/export.sh
idf.py set-target esp32p4
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

### Building for `esp32s31`

ESP-IDF **v6.1-beta1** is required — no earlier release has the `esp32s31`
target. Its own `export.sh` does not work in this install (it looks for a
virtualenv the toolchain installer never created), so source the helper
instead:

```sh
. tools/s31-env.sh          # must be SOURCED, not executed
idf.py -B build.s31 build
idf.py -B build.s31 -p /dev/ttyACM0 flash monitor
```

The helper resolves the v6.1 venv, puts ninja / cmake / the RISC-V toolchain on
PATH, skips the missing constraints file, and sets `ESP_IDF_VERSION` (the
component manager crashes on `None` without it). Use it for every S31 build:
mixing it with a manually-invoked `python3 .../idf.py` leaves the build
directory recording a different interpreter and IDF then refuses to build until
`fullclean`.

The **P4** target is unaffected and still builds under v6.0, whose `export.sh`
is fine:

```sh
. ~/.espressif/v6.0/esp-idf/export.sh
idf.py -B build build
```

On your machine every sibling is resolved by path, so the firmware picks up whatever is
checked out next to it. CI and releases do not: `release-manifest.json` names the exact
commit of each sibling, `build.yml` and `release.yml` check them out at those commits, and a
release ships the manifest and a `sources-<tag>.txt` next to the image. When the firmware needs
a newer sibling, commit and push the sibling first, then run `tools/bump_siblings.sh` (it
refuses dirty or unpushed checkouts) and commit the manifest with the change that needs it.

## Updates

The web UI's **OTA** page lists the published releases that fit the hub (read from GitHub by
the browser, matched on chip and silicon family) and installs the chosen one; a URL field for
an image that is not a release stays under *Advanced*.
The hub downloads it over HTTPS into the idle app slot and reboots into it. The new firmware
is then on trial: the hub keeps it only once its storage answers, its web server and event
dispatcher are up, its sign-in storage opened and, if the Zigbee radio worked before the
update, the radio works again -- an unplugged Ethernet
cable is deliberately not a reason to go back. Unmet after ten minutes, or a crash before
that, and the bootloader boots the previous version; status then reports why as
`ota_rollback_reason` and the OTA page shows it. Status reports `ota_state` (`pending` during
the trial, `verified` after) and refuses a second update while the trial runs. The web UI is packed into the app image (`tools/pack_spa.py`), so it is
updated together with the firmware — there is no separate UI partition to fall out of step.

Boards flashed before OTA existed have the old single-slot layout and need one full USB
flash (the browser flasher erases first) to move to this one.

**What an update's trust rests on.** The hub downloads over HTTPS and checks the server's
certificate against the built-in root bundle, so the bytes come from GitHub and nobody on the
path can change them. `SHA256SUMS` in the release lets you check a file you downloaded by
hand. `sources-<tag>.txt` names the exact commits every file was built from, so anyone can
rebuild and compare. Images are **not signed**: the hub does not check who published the file,
only that it came intact from the URL it was given. Install from the OTA page's list or from
the project's own releases page; an image from anywhere else runs with the hub's full access.

## Multi-target layout

One tree serves two SoCs. On the **ESP32-S31** the chip's own 802.15.4 radio replaces the
C6 and Ethernet is RGMII rather than RMII. Its only remaining catch is the toolchain:
ESP-IDF has no `esp32s31` target before **v6.1-beta1**, which is why CI and releases build
the P4 only. The library half is solved — `esp-zigbee-lib` **2.0.4** ships `esp32s31` libs including the `native`
radio flavor (verified in the registry archive 2026-08-15; 2.0.1 had none, so `^2.0.4` is
the floor), and a C6 RAM proxy (`../extra/s31-spike/`) already links the native coordinator
stack at ≈ **18.6 KB static DIRAM**. See `../extra/docs/WIRED_CORE_MULTI_TARGET_DESIGN.md`
for why S31 stays in this repo rather than becoming a fourth firmware fork.

Three things differ per target; everything else is shared.

| Concern | Mechanism |
|---|---|
| Chip config | `sdkconfig.defaults.<target>`, auto-applied **after** `sdkconfig.defaults` |
| Ethernet PHY | one `board_eth_new()` impl per board, picked by `IDF_TARGET` in `main/CMakeLists.txt` |
| Zigbee radio | Kconfig — `UART_RCP` on P4, `NATIVE` on S31 |

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

These are decisions, not gaps. `tools/check_sku_invariants.sh` makes them
build-breaking rather than conventions nobody reads.

| Absent | Why |
|---|---|
| **RainMaker** | not part of this SKU |
| **Wi-Fi / `esp_wifi`** | Ethernet-only by design |
| **`znp_driver`, `ezsp_driver`, `zigbee_mgr`** | the radio here is `esp_zigbee` + `ot_rcp`; the legacy backends belong to the flagship line |
| **BLE** | structurally impossible — the P4 has no radio of its own |

## Licence

AGPL-3.0-or-later, matching the other firmware cores. SPDX headers are enforced.
