#!/bin/sh
# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Host check of mqtt_glue.cpp's on_mqtt_rx() (research 2026-11-18 A12): what the hub publishes under the root
# itself (device state, logs, metrics, availability) never comes back to the rule engine or Lua, and a long
# payload reaches them with the id of its whole text. The REAL function is cut out of main/mqtt_glue.cpp by sed;
# the Home Assistant bridge and the event bus are fakes; mqtt_payload.cpp is the real one.
#   sh run.sh        build and run; exit code = number of failed checks
set -e
H=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$H/../../.." && pwd)
C=$(cd "$R/../zhac-components/components" && pwd)
W=$(mktemp -d "${TMPDIR:-/tmp}/wired-host-mqtt-rx.XXXXXX")
trap 'rm -rf "$W"' EXIT
export PATH=/usr/bin:$PATH   # the ESP32-ULP `as` on PATH poisons g++
for f in 'bool own_echo' 'void on_mqtt_rx'; do
    sed -n "/^$f(/,/^}/p" "$R/main/mqtt_glue.cpp" >> "$W/slice.inc"
done
grep -q '^void on_mqtt_rx(' "$W/slice.inc"   # the slice found the entry point
g++ -std=c++17 -g -fsanitize=address,undefined -Wall -Wextra -Werror -I"$W" \
    -I"$C/simple_rules/test/host/stubs" -I"$C/event_bus/include" -I"$C/zap_common/include" \
    "$H/harness.cpp" "$C/event_bus/mqtt_payload.cpp" -o "$W/harness"
set +e
"$W/harness"
