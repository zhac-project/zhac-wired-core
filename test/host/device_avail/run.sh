#!/bin/sh
# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Host check of ws_bridge.cpp's on_device_availability() (research 2026-11-18 B7): the hub page and the cloud learn
# that a mains device stopped answering its availability ping, and that it came back, as an attr.bulk entry with no
# attrs. The REAL function is cut out of main/ws_bridge.cpp by sed; the push and the clock are fakes.
#   sh run.sh        build and run; exit code = number of failed checks
set -e
H=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$H/../../.." && pwd)
C=${ZHAC_COMPONENTS:-$(cd "$R/../zhac-components/components" && pwd)}
W=$(mktemp -d "${TMPDIR:-/tmp}/wired-host-device-avail.XXXXXX")
trap 'rm -rf "$W"' EXIT
export PATH=/usr/bin:$PATH   # the ESP32-ULP `as` on PATH poisons g++
sed -n '/^static void on_device_availability(/,/^}/p' "$R/main/ws_bridge.cpp" > "$W/slice.inc"
g++ -std=c++17 -g -fsanitize=address,undefined -Wall -Wextra -Werror -I"$W" -I"$C/event_bus/include" \
    -I"$C/zap_common/include" "$H/harness.cpp" -o "$W/harness"
set +e
"$W/harness"
