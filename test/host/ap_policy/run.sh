#!/bin/sh
# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Host check of the setup access point rule (ap_step, components/wifi_hosted/wifi_hosted.cpp, cut out by sed
# between "ap-policy begin" and "ap-policy end"): review 2026-11-09 WN-01, WN-02.
#   sh run.sh        build and run; exit code = number of mismatches
set -e
H=$(cd "$(dirname "$0")" && pwd)
F=$(cd "$H/../../../components/wifi_hosted" && pwd)/wifi_hosted.cpp
W=$(mktemp -d "${TMPDIR:-/tmp}/wired-host-appolicy.XXXXXX")
trap 'rm -rf "$W"' EXIT
sed -n '/ap-policy begin/,/ap-policy end/p' "$F" > "$W/slice.inc"
grep -q '^static int ap_step' "$W/slice.inc"
export PATH=/usr/bin:$PATH   # the ESP32-ULP `as` on PATH poisons g++
g++ -std=c++17 -g -fsanitize=address,undefined -Wall -Wextra -Wno-unused-function -I"$W" "$H/harness.cpp" -o "$W/harness"
set +e
"$W/harness"
