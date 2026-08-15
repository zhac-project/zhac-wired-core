#!/bin/sh
# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Assert that chip-critical settings RESOLVED to what we asked for.
#
# WHY THIS EXISTS -- there are two ways a line in sdkconfig.defaults can do
# nothing, and only one of them is noisy:
#
#   1. Unknown symbol      -> kconfgen prints "unknown kconfig symbol". Caught
#                             by grepping the reconfigure output.
#   2. Known-but-unselectable -> kconfgen SILENTLY drops it to the choice
#                             default. No warning of any kind.
#
# Case 2 bit this repo with CONFIG_ESP32P4_REV_MIN_0, which needs
# CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y to be selectable at all. Without the
# gate it silently became _301 -- a binary for the wrong P4 silicon family,
# which fails at FLASH time, long after a green build.
#
# So: check what came out, not what went in.
#
# Expected values are taken from zhac-main-core, the config validated on ZHAC
# P4 hardware. Run after configuring:  ./tools/check_resolved_config.sh
set -e

SDKCONFIG="${1:-sdkconfig}"
if [ ! -f "$SDKCONFIG" ]; then
    echo "FAIL: $SDKCONFIG not found -- run idf.py reconfigure first" >&2
    exit 1
fi

fail=0
expect() {
    key="$1"; want="$2"; why="$3"
    got=$(grep -E "^${key}=" "$SDKCONFIG" | head -1 | cut -d= -f2-)
    if [ "$got" != "$want" ]; then
        echo "FAIL: $key resolved to '${got:-<unset>}', expected '$want'" >&2
        echo "  -> $why" >&2
        fail=1
    fi
}

TARGET=$(grep -E '^CONFIG_IDF_TARGET=' "$SDKCONFIG" | cut -d= -f2- | tr -d '"')

# ---------------------------------------------------------------- esp32s31 --
if [ "$TARGET" = "esp32s31" ]; then
    expect CONFIG_SPIRAM y          "PSRAM is mandatory for this firmware."
    expect CONFIG_SPIRAM_SPEED 250  "250 MHz keeps MPLL at 500, so EMAC can derive 125 MHz for RGMII. At 200 MHz the MPLL lands at 400 and Ethernet init fails with 'config emac interface failed'."

    # Load-bearing. Flash and PSRAM share a cache on this part: without XIP the
    # cache is disabled for every flash op, and any PSRAM access during that
    # window (we deliberately put zhc_adapter's .bss there, and device_shadow
    # puts its own buffers there via EXT_RAM_BSS_ATTR) raises
    # "Cache error / Cache access error" inside nvs_flash_init. The failure is
    # layout-sensitive, so it can hide through several builds and then wedge
    # the board in a reset loop after an unrelated change.
    expect CONFIG_SPIRAM_XIP_FROM_PSRAM y \
        "Required whenever any .bss lives in PSRAM on this SoC -- see main/psram_bss.lf."
    expect CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY y \
        "Opens app.lf's extram_bss branch; without it main/psram_bss.lf silently does nothing and Ethernet dies for want of DMA descriptors."
    expect CONFIG_ZB_RADIO_NATIVE y "S31 has its own 802.15.4; the RCP path is the P4 configuration."
    expect CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH y \
        "Coredumps off. Usually means the espcoredump component fell out of the build graph."

    if [ "$fail" -eq 0 ]; then
        echo "OK: resolved chip config valid for esp32s31."
    fi
    exit "$fail"
fi

# ----------------------------------------------------------------- esp32p4 --
# Silicon family. The pair must agree: the gate opens the v0/v1 choices, and
# REV_MIN_0 selects the lowest. Either alone is wrong.
expect CONFIG_ESP32P4_SELECTS_REV_LESS_V3 y \
    "P4 has two incompatible revision families; this picks v0.x-v1.x, matching main-core and the v1.3 bench rig. A binary for one family will not boot on the other."
expect CONFIG_ESP32P4_REV_MIN_FULL 0 \
    "Silently becomes 301 if the LESS_V3 gate is off. That is the failure this script exists to catch."
expect CONFIG_ESP32P4_REV_MAX_FULL 199 \
    "Follows the family gate; 399 means the v3.x family got selected."

# PSRAM. 32 MB on this part needs HEX mode; a wrong mode under-reports or
# fails detection, and the firmware requires PSRAM to run at all.
expect CONFIG_SPIRAM y            "PSRAM is mandatory for this firmware."
expect CONFIG_SPIRAM_MODE_HEX y   "32 MB P4 PSRAM is 16-line HEX mode."
expect CONFIG_SPIRAM_SPEED 200    "Matches main-core."

# Clock and flash: main-core's validated values, not the IDF defaults
# (400 MHz / DIO / 80 MHz) this project would otherwise land on.
expect CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ 360        "IDF default is 400; main-core runs 360."
expect CONFIG_ESPTOOLPY_FLASHFREQ '"40m"'         "IDF default is 80m; main-core uses 40m."
expect CONFIG_ESPTOOLPY_FLASHSIZE '"16MB"'        "Board has 16 MB."

# Coredump. Not paranoia: set(COMPONENTS main) means an unrequired component
# is never built, and an unbuilt component contributes no Kconfig -- so this
# symbol silently became "unknown" and was ignored until espcoredump was added
# to main's REQUIRES. Exactly the silent-drop class this script exists for.
expect CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH y \
    "Coredumps off. Usually means the espcoredump component fell out of the build graph."

if [ "$fail" -eq 0 ]; then
    echo "OK: resolved chip config matches zhac-main-core."
fi
exit "$fail"
