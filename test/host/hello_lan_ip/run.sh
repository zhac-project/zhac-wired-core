#!/bin/sh
# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Host check of ws_bridge.cpp's remote_hello_extra() (P1 review 2026-11-19 #4): the cloud learns the hub's LAN
# address from Ethernet, else from Wi-Fi on a build with it. The REAL function is cut out of main/ws_bridge.cpp by
# sed and built twice, with and without CONFIG_ZHAC_WIFI_HOSTED; the network status calls are fakes.
#   sh run.sh        build and run; exit code = number of failed checks
set -e
H=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$H/../../.." && pwd)
W=$(mktemp -d "${TMPDIR:-/tmp}/wired-host-hello-lan-ip.XXXXXX")
trap 'rm -rf "$W"' EXIT
export PATH=/usr/bin:$PATH   # the ESP32-ULP `as` on PATH poisons g++
sed -n '/^extern "C" size_t remote_hello_extra/,/^}/p' "$R/main/ws_bridge.cpp" > "$W/slice.inc"
test "$(wc -l < "$W/slice.inc")" -gt 5   # the slice found the whole function
FLAGS="-std=c++17 -g -fsanitize=address,undefined -Wall -Wextra -Werror -I$W -I$R/main -I$R/components/wifi_hosted/include"
g++ $FLAGS -DCONFIG_ZHAC_WIFI_HOSTED=1 "$H/harness.cpp" -o "$W/wifi"
g++ $FLAGS "$H/harness.cpp" -o "$W/wired"
set +e
"$W/wifi"; a=$?
"$W/wired"; b=$?
exit $((a + b))
