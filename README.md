<!--
SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# ZHAC wired hub — one board, plug in Ethernet, done

ZHAC is a Zigbee hub that runs entirely on one ESP32 board: the coordinator, a device library
of about 5,000 devices (generated from zigbee2mqtt's definitions), rules, Lua scripts, MQTT,
Home Assistant discovery and the web UI. No Wi-Fi, no Linux box, no cloud account.

## Get one running in ten minutes

You need an **Espressif ESP32-S31 Function-CoreBoard** (about $25: Ethernet, the Zigbee radio,
USB-C and a status LED are on it, nothing to solder or wire), a USB-C cable, an Ethernet cable,
and Chrome or Edge on a desktop computer.

1. Connect the board's **USB-to-UART** port (the USB-C next to the RJ45 jack) to the computer.
2. Open **https://zhac-project.github.io/zhac-docs/flash/**, press **Install** under
   *ESP32-S31*, pick the serial port. About two minutes.
3. Plug in Ethernet and open **http://zhac.local**. The first visit asks you to set a password.
4. Devices → **Permit join**, put your Zigbee device in pairing mode. The LED blinks green while
   the join window is open and flashes blue on every Zigbee frame.

Updates come from the web UI (Settings → Update) and keep your devices, rules and settings.
Prefer a terminal? `pip install esptool`, then from the
[releases page](https://github.com/zhac-project/zhac-wired-core/releases):

```sh
esptool --chip esp32s31 --port /dev/ttyUSB0 write-flash \
  0x2000 zhac-wired-s31-<version>-bootloader.bin 0xC000 zhac-wired-s31-<version>-partition-table.bin \
  0x1D000 zhac-wired-s31-<version>-otadata.bin 0x20000 zhac-wired-s31-<version>-ota.bin
```

That keeps devices, rules and settings. The single merged `zhac-wired-s31-<version>.bin` written at
`0x0` is a clean start: it covers the settings area and erases it.

## What works on the S31 today (all of it run on hardware)

- Pairing, interviews of sleepy battery devices, configure/bind, Tuya datapoint devices, native
  ZCL group membership so zone remotes (MiBoxer FUT089Z and the like) are heard.
- Rules (a small DSL) and Lua scripts; MQTT with a state topic per device; Home Assistant
  discovery (Settings → MQTT); Telegram-free, cloud-free.
- Web UI: devices, rules, scripts, groups, diagnostics (tasks, heap, unhandled frames), logs,
  update. Status LED. A boot guard that keeps the UI up if the radio ever dies.

## Limits, honestly

- Ethernet only. The chip has Wi-Fi 6, BLE and 802.15.4; this firmware uses the wire and the
  802.15.4 radio, nothing else. That is the point of a hub, but say so before buying.
- Board only: no case yet (a printable one is welcome), no PoE; powered over USB-C.
- The first over-the-air update from the web UI has been built and tested against a release
  feed, not yet through a full release cycle. The browser flasher always works.
- Backup/restore of pairings is not there yet; a full erase means re-pairing.
- ESP-IDF v6.1 is the first release with the S31. Releases ship prebuilt, so you do not need it
  unless you build yourself (below).

Bugs, missing devices and reports about other boards go to the
[ZHAC issue tracker](https://github.com/zhac-project/zhac-platform/issues/new/choose).

## Status — 2026-09-26

| Target | Board | Zigbee radio | State |
|---|---|---|---|
| `esp32s31` | Espressif ESP32-S31 Function-CoreBoard | the S31's own 802.15.4 | **Recommended.** Runs on hardware: pairing, rules, MQTT/HA, groups, LED, update page. |
| `esp32p4` | Guition JC-ESP32P4-M3-DEV | the ESP32-C6 on the module, running `ot_rcp` | Builds and releases. Ethernet and the Zigbee coordinator run on hardware (verified 2026-09-26, radio installed via the [C6 RCP installer](#zigbee-radio-esp32-c6)); device pairing and a longer soak are still pending. |

## Boards

| Board | SoC | Ethernet | Zigbee radio | Status |
|---|---|---|---|---|
| Espressif ESP32-S31 Function-CoreBoard (≈ $25) | ESP32-S31 | YT8531, 1 Gbit, RGMII | the S31's own 802.15.4 | runs on hardware, recommended |
| Guition JC-ESP32P4-M3-DEV (≈ $14) | ESP32-P4, silicon v1.x | IP101, 100 Mbit, RMII | on-module ESP32-C6 running `ot_rcp` | Ethernet + coordinator run on hardware (2026-09-26); pairing and soak pending |
| Any other ESP32-P4 board | check the revision first | set the `ZHAC_ETH_*` pins in menuconfig | an 802.15.4 ESP chip running `ot_rcp` | untested |

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
chips (`rcp/sdkconfig.defaults`). A new board ships the C6 with its factory ESP-Hosted
Wi-Fi co-processor firmware instead, so `ot_rcp` has to be installed onto it **once**
before the hub can use it.

**Fresh Guition M3-DEV — two steps:**

1. **Install the Zigbee radio (one time).** Flash
   `zhac-c6-rcp-installer-p4-rev1x-<version>.bin` at `0x0` (or use the **1. Install the
   Zigbee radio** button on the
   [browser flasher](https://zhac-project.github.io/zhac-docs/flash/)). This is a one-off
   ESP32-P4 app: over the SDIO pins the P4 and C6 already share, it talks to the C6's
   factory ESP-Hosted slave, OTA-writes `ot_rcp` into its inactive slot, resets the C6 and
   checks it answers spinel — no JP1, no wires, no USB-serial adapter needed. Allow 5 to
   30 seconds; if you have no serial monitor open, that is enough time on real hardware
   (the OTA itself takes a few seconds, most of the rest is a retried SDIO connect). To
   confirm it worked, either watch the console (`idf.py -p PORT monitor`, or any terminal
   at 115200) for the line `C6-INSTALL: SUCCESS …`, or just go on to step 2 and check the
   hub's web UI does not report a radio-down / `radio_crashed` state.
   **This is one-way:** afterwards the C6 no longer runs ESP-Hosted, so this tool cannot
   be used again to update the radio later — see "Advanced" below. Running it again on an
   already-installed C6 is safe: it detects spinel and writes nothing.
2. **Install the hub.** Flash the normal `zhac-wired-p4-rev1x-<version>.bin` image, as in
   [Build from source](#build-from-source) / the browser flasher's **2. Install the hub**
   button.

Full detail — build commands, the exact byte offsets, and what to do if a step
fails — is in [`tools/c6-rcp-installer/README.md`](tools/c6-rcp-installer/README.md).

<details>
<summary>Advanced: flashing the C6 by wire (JP1) — for a later radio update, or if you
have an ESP-Prog</summary>

The installer above is one-way, so updating `ot_rcp` again later, or recovering a C6 that
never got ESP-Hosted in the first place, needs the C6's own debug header, **JP1** (2×13,
2.54 mm; labels below are from the **M3-DEV's own schematic**, not the sibling
JC4880P443C board referenced by earlier notes): `C6_U0RXD` pin 20, `C6_U0TXD` pin 22,
`C6_IO9` pin 24, `C6_CHIP_PU` pin 26, GND on pins 5 and 6 (pins 16/18 are ambiguous in the
schematic — use 5/6).

1. Stop the P4 so it stops resetting the C6: hold **BOOT**, tap **RESET**, release BOOT.
2. Wire a **3.3 V** USB-serial adapter — a 5 V TTL adapter will damage the C6: adapter
   **TX → JP1-20**, adapter **RX → JP1-22**, adapter **GND → JP1-6**; leave the adapter's
   3V3/5V pin unconnected.
3. Put the C6 in download mode: jumper **JP1-24 → JP1-5** (GND), then briefly touch
   **JP1-26** to GND and release.
4. `esptool --chip esp32c6 --port /dev/ttyUSB0 erase-flash`, then
   `esptool --chip esp32c6 --port /dev/ttyUSB0 write-flash 0x0 zhac-rcp-c6-<version>.bin`
   (or the **Zigbee radio** button on the browser flasher, picking the adapter's port).
   Build the image yourself with `tools/build-rcp.sh build.rcp/zhac-rcp-c6.bin` after
   sourcing ESP-IDF v6.0.
5. Remove the IO9 jumper, power-cycle the board.

This route did not produce clean serial for the maintainer even with the wiring above
(garbled output on both directions) — treat it as a fallback, not the default path. An
ESP-Prog, which drives IO9/CHIP_PU itself, is more likely to work than a plain adapter.

</details>

Without a working radio the hub still boots. The first attempt to start it fails and
resets the board; the next boot skips the radio, serves the web UI and reports
`radio_error: "radio_crashed"`, which the UI shows. Reset to try again.

### Guition wiring

- ESP32-P4, 32 MB PSRAM, 16 MB NOR flash
- 100M Ethernet via an **IP101** PHY at SMI address 1, wired to ESP-IDF's default esp32p4
  EMAC pins — MDC 31, MDIO 52, REF_CLK-in 50, **PHY reset 51** (active low, held high;
  the schematic names it `PHY_RSTN`, not a power-enable line), data 28/29/30/34/35/49
- ESP32-C6 on the same module, reached over what were its SDIO traces
  (P4 14/15/54 ↔ C6 20/21/EN) — the Zigbee radio. The spinel UART runs C6 TX 20 →
  P4 RX 14 and P4 TX 15 → C6 RX 21, **confirmed on hardware 2026-09-26** (coordinator
  formed a PAN over this link at 460800 baud).

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

If the storage partition cannot be initialised at boot, the hub no longer erases it by itself:
it boots locked and empty, reports `storage_error`, and Settings offers the erase. If only the
sign-in storage cannot be opened, the hub likewise locks itself instead of opening up:
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

## Status LED

Boards with an addressable RGB LED (`CONFIG_ZHAC_STATUS_LED_GPIO`; the ESP32-S31 dev board has
one on GPIO 60) show: **green blink** while the join window is open, **blue flash** on Zigbee
traffic, off otherwise.
