#!/bin/sh
# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Host check of main.cpp's log_heap_info() (P2 review 2026-11-19 #2): main-largest is the largest free block of
# the main internal heap alone -- not of the 32 KB reserve pool PSRAM keeps for DMA, which would stand in for it
# as soon as the main heap fragments. The REAL function is cut out of main/main.cpp by sed; the heaps are a fake
# with the caps IDF v6.0 gives each region on the P4 (heap/port/esp32p4/memory_layout.c, esp_psram.c).
#   sh run.sh        build and run; exit code = number of failed checks
set -e
H=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$H/../../.." && pwd)
W=$(mktemp -d "${TMPDIR:-/tmp}/wired-host-heap-info.XXXXXX")
trap 'rm -rf "$W"' EXIT
export PATH=/usr/bin:$PATH   # the ESP32-ULP `as` on PATH poisons g++
sed -n '/^static void log_heap_info(/,/^}/p' "$R/main/main.cpp" > "$W/slice.inc"
test "$(wc -l < "$W/slice.inc")" -gt 5   # the slice found the whole function
g++ -std=c++17 -g -fsanitize=address,undefined -Wall -Wextra -Werror -I"$W" "$H/harness.cpp" -o "$W/harness"
set +e
"$W/harness"
