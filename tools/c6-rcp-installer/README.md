<!--
SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
SPDX-License-Identifier: AGPL-3.0-or-later
-->
# c6-rcp-installer

A one-off ESP32-P4 app for the **Guition JC-ESP32P4-M3-DEV**. It replaces the on-module
ESP32-C6's factory ESP-Hosted slave firmware with our Zigbee radio firmware (`ot_rcp`). It
does this over the P4-to-C6 SDIO link using ESP-Hosted **slave OTA**, so you don't need
JP1, wires or a USB-UART adapter. When it finishes, it checks that the C6 answers spinel on
the traces zhac-wired-core uses (P4 TX 15 / RX 14 @ 460800).

**This is one-way.** Afterwards the C6 has no ESP-Hosted slave, so nothing can OTA it
again over SDIO. Going back to ESP-Hosted, or updating the RCP later, needs the JP1 UART.

## What it does

1. Checks the embedded C6 image. It must be an ESP32-C6 **app** image (magic `0xE9`, chip
   id 13, app descriptor `esp_ot_rcp`), its appended SHA-256 must be valid, and its
   sha256 must match the build-time value. Any failure stops the tool before it contacts
   the C6.
2. Resets the C6 (GPIO54) and connects to the ESP-Hosted slave over SDIO: 1-bit,
   10 MHz, packet mode. If there is no slave, it checks whether the C6 already speaks
   spinel. If it does, it prints `SUCCESS` and writes nothing. If neither answers, it
   prints `FAILED` and writes nothing.
3. Logs the slave's ESP-Hosted version and running app description.
4. Runs `esp_hosted_slave_ota_begin`, then `write` in 1500 B chunks (progress every
   10 %), then `end`, then `activate`. Any error stops the tool. It never activates after
   a failed write or end.
5. Shuts down esp_hosted, resets the C6 and asks it for `PROP_NCP_VERSION` over
   HDLC-lite spinel.

The last line on the console is always one of these:

```
C6-INSTALL: SUCCESS OPENTHREAD/...; ESP32C6; ...
C6-INSTALL: FAILED <reason>
```

The tool then halts. It never reboots itself, and it never reads or writes NVS.

## Build

```sh
. ~/.espressif/v6.0/esp-idf/export.sh
cd zhac-wired-core
# 1. the ot_rcp APP image, 4 MB/64 KB-page variant (see rcp-ota.defaults)
RCP_EXTRA_DEFAULTS=$PWD/tools/c6-rcp-installer/rcp-ota.defaults \
  tools/build-rcp.sh build.rcp-ota/merged-unused.bin build.rcp-ota
# 2. the installer (embeds build.rcp-ota/build/esp_ot_rcp.bin)
cd tools/c6-rcp-installer
idf.py set-target esp32p4 && idf.py build
idf.py merge-bin -o c6-rcp-installer-merged.bin     # fallback only, see below
```

## Flash: the safe way (keeps NVS, including the admin password)

The installer uses **the same `partitions.csv`** as zhac-wired-core (table at `0xC000`,
bootloader at `0x2000`, `nvs` at `0xD000` 64K, `otadata` at `0x1D000` 8K, `ota_0` at
`0x20000` 6M). Write only the app into `ota_0`, and erase only `otadata` so the bootloader
already on the board picks `ota_0`. Do not write the bootloader, the partition table or
NVS.

Connect to the CH340 USB-C port. The port is usually `/dev/ttyUSB0`.

```sh
cd zhac-wired-core/tools/c6-rcp-installer
# optional pre-check: the board's partition table must be byte-identical
esptool --chip esp32p4 -p /dev/ttyUSB0 read-flash 0xC000 0xC00 /tmp/pt-board.bin
cmp /tmp/pt-board.bin build/partition_table/partition-table.bin && echo "table OK"

esptool --chip esp32p4 -p /dev/ttyUSB0 -b 460800 --after no-reset \
  write-flash 0x20000 build/c6_rcp_installer.bin
esptool --chip esp32p4 -p /dev/ttyUSB0 erase-region 0x1D000 0x2000
idf.py -p /dev/ttyUSB0 monitor        # resets the P4; or any terminal @115200
```

The run takes about 5 to 30 s. The OTA itself is a few seconds; a failed SDIO connect
waits through its retries first.

**Fallback (wipes NVS):** `esptool --chip esp32p4 -p /dev/ttyUSB0 -b 460800 write-flash
0x0 build/c6-rcp-installer-merged.bin`. The merged image covers `0x0`–`0xF84B0` and pads
the NVS region with `0xFF`.

## Back to zhac-wired-core

Write the release's app-only image back into `ota_0` and erase `otadata` again. NVS is
untouched, so the password and settings survive.

```sh
esptool --chip esp32p4 -p /dev/ttyUSB0 -b 460800 --after no-reset \
  write-flash 0x20000 zhac-wired-p4-rev1x-<tag>-ota.bin
esptool --chip esp32p4 -p /dev/ttyUSB0 erase-region 0x1D000 0x2000
```

For a local build, use `build/zhac-wired-core.bin` from the repo root instead of the
`-ota.bin` file.

## If it fails

- `FAILED ota_begin` / `ota_write` / `ota_end`: the C6 still boots its old slot. Its
  active partition is never written, so the tool is safe to re-run.
- **An assert in `process_init_event` right after "connecting…"**: the C6 runs a 2.12.x
  slave in SDIO streaming mode. Rebuild with the `sdkconfig.streaming` overlay (the
  command is in that file) and flash that build instead.
- `FAILED ... no spinel reply` after a successful OTA: power-cycle the board and run the
  tool again. With no hosted slave left, it goes straight to the spinel check.
