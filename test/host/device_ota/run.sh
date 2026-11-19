#!/bin/sh
# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Host check of the Zigbee device firmware update core (research 2026-11-18 C2): main/device_ota_core.h, under
# ASan/UBSan. Needs g++ and PATH=/usr/bin first.
#   sh run.sh        build and run; exit code = number of mismatches
set -e
H=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$H/../../../main" && pwd)
W=$(mktemp -d "${TMPDIR:-/tmp}/wired-host-devota.XXXXXX")
trap 'rm -rf "$W"' EXIT
export PATH=/usr/bin:$PATH   # the ESP32-ULP `as` on PATH poisons g++
g++ -std=c++17 -g -fsanitize=address,undefined -Wall -Wextra -Werror -I"$R" "$H/harness.cpp" -o "$W/harness"
"$W/harness"
