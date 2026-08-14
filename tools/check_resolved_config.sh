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

if [ "$fail" -eq 0 ]; then
    echo "OK: resolved chip config matches zhac-main-core."
fi
exit "$fail"
