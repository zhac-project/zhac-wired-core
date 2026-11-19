#!/bin/sh
# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Host check of mqtt_glue.cpp's <root>/devices/<IEEE>/state publisher (research 2026-11-18 A12): the device's
# whole state once per report, retained; a press (action) at once and not retained; a removed device's retained
# state cleared. The REAL functions are cut out of main/mqtt_glue.cpp by sed; the shadow, the MQTT client and the
# Home Assistant bridge are fakes; ha_discovery.cpp (state_retained, x100_to_json) is the real one.
#   sh run.sh        build and run; exit code = number of failed checks
set -e
H=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$H/../../.." && pwd)
C=$(cd "$R/../zhac-components/components" && pwd)
W=$(mktemp -d "${TMPDIR:-/tmp}/wired-host-mqtt-state.XXXXXX")
trap 'rm -rf "$W"' EXIT
export PATH=/usr/bin:$PATH   # the ESP32-ULP `as` on PATH poisons g++
for f in 'bool value_json' 'void add_attrs' 'size_t state_json' 'void publish_state' 'void on_attr' 'void on_device_leave' 'void on_availability'; do
    sed -n "/^$f(/,/^}/p" "$R/main/mqtt_glue.cpp" >> "$W/slice.inc"
done
grep -q '^void on_attr(' "$W/slice.inc"   # the slice found the entry point
g++ -std=c++17 -g -fsanitize=address,undefined -Wall -Wextra -Werror -I"$W" \
    -I"$C/device_shadow/test/host/stubs" -I"$C/device_shadow/include" -I"$C/event_bus/include" \
    -I"$C/zap_common/include" -I"$C/ha_bridge/include" -I"$C/arduinojson/src" \
    "$H/harness.cpp" "$C/ha_bridge/ha_discovery.cpp" -o "$W/harness"
set +e
"$W/harness"
