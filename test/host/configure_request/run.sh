#!/bin/sh
# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Host check of api_devices.cpp's device_configure_request() (P1 review 2026-11-19): a manual Configure (WS
# device.configure, POST /api/device/configure) is queued to the configure worker with a fresh start, never run on
# the web server task. The REAL function is cut out of main/api_devices.cpp by sed; the pool and the queue are fakes,
# and nothing provides zhac_adapter_configure, so a slice that still configured inline would not link.
#   sh run.sh        build and run; exit code = number of failed checks
set -e
H=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$H/../../../main" && pwd)
ZC=$(cd "$H/../../../../zhac-components/components/zap_common/include" && pwd)
W=$(mktemp -d "${TMPDIR:-/tmp}/wired-host-configure-request.XXXXXX")
trap 'rm -rf "$W"' EXIT
export PATH=/usr/bin:$PATH   # the ESP32-ULP `as` on PATH poisons g++
sed -n '/^bool device_configure_request/,/^}/p' "$R/api_devices.cpp" > "$W/slice.inc"
test "$(wc -l < "$W/slice.inc")" -gt 5   # the slice found the whole function
g++ -std=c++17 -g -fsanitize=address,undefined -Wall -Wextra -I"$W" -I"$ZC" "$H/harness.cpp" -o "$W/harness"
set +e
"$W/harness"
