#!/bin/sh
# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Host check of ws_bridge.cpp's erase_storage_keeping_pin() (research 2026-11-18 A4): "Erase storage" keeps the
# Zigbee network, so it must keep the coordinator address pin too, and leave NVS shut so the restart's store
# flush cannot write the old rules or states back. The REAL function is cut out of main/ws_bridge.cpp by sed.
#   sh run.sh        build and run; exit code = number of failed checks
set -e
H=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$H/../../../main" && pwd)
W=$(mktemp -d "${TMPDIR:-/tmp}/wired-host-storage-reset.XXXXXX")
trap 'rm -rf "$W"' EXIT
export PATH=/usr/bin:$PATH   # the ESP32-ULP `as` on PATH poisons g++
sed -n '/^static esp_err_t erase_storage_keeping_pin/,/^}/p' "$R/ws_bridge.cpp" > "$W/slice.inc"
test "$(wc -l < "$W/slice.inc")" -gt 10   # the slice found the whole function
g++ -std=c++17 -g -fsanitize=address,undefined -Wall -Wextra -I"$W" "$H/harness.cpp" -o "$W/harness"
set +e
"$W/harness"
