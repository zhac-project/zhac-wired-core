#!/bin/sh
# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Host check of the radio-job worker (research 2026-11-18 B2): device.bind and device.groups.add / remove /
# refresh are checked on the web server's task and sent by a worker that answers when the device did; the web
# server's task is back at once, a full queue answers "busy", and an answer for a socket that went away is not
# sent. The code under test is cut verbatim out of main/ws_bridge.cpp by sed: the radio-jobs section, parse_ieee,
# send_err, reply_ok_or_err, dgm_arg_ieee, dgm_reply_list and the four command handlers. JsonWriter and
# ArduinoJson are the real headers. FreeRTOS (task, queue, mutex) runs on std::thread at 1 tick = 100 ns, so the
# worker's one-minute idle end takes 6 ms; the radio, the pool, the group mirror and the WebSocket server are
# fakes (harness.cpp).
#   sh run.sh                  build and run; exit code = number of mismatches (ASan/UBSan/TSan abort = non-zero)
#   WS_SRC=<file> sh run.sh    the same checks against another ws_bridge.cpp (a RED run)
# Needs the sibling checkouts (zhac-components) next to this repo, g++, and PATH=/usr/bin first.
set -e
H=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$H/../../../main" && pwd)
Z=$(cd "$H/../../../.." && pwd)
AJ=$Z/zhac-components/components/arduinojson/src
WS=$Z/zhac-components/components/ws_server/include   # utf8_seq.h, for json_buf.h
W=$(mktemp -d "${TMPDIR:-/tmp}/wired-host-radiojobs.XXXXXX")
trap 'rm -rf "$W"' EXIT

WSB=${WS_SRC:-$R/ws_bridge.cpp}
slice() {   # one function: its definition line ends with "{", its closing brace sits in column 0
    sed -n "/^static [a-z_0-9]* $1(.*{\$/,/^}/p" "$WSB" > "$W/part"
    test "$(grep -c '^}' "$W/part")" -eq 1
    cat "$W/part"
    echo
}
{
    slice parse_ieee
    slice send_err
    slice reply_ok_or_err
    sed -n '/radio-jobs begin/,/radio-jobs end/p' "$WSB"
    slice cmd_device_bind
    slice dgm_arg_ieee
    slice dgm_reply_list
    sed -n '/^static void cmd_device_groups_add_remove(int fd/,/^}/p' "$WSB"
    slice cmd_device_groups_refresh
} > "$W/ws_slice.inc"
grep -q 'radio-jobs end' "$W/ws_slice.inc"
grep -q '^static void cmd_device_groups_add_remove' "$W/ws_slice.inc"

cd "$W"
export PATH=/usr/bin:$PATH   # the ESP32-ULP `as` on PATH poisons g++
# -Wno-maybe-uninitialized: GCC 15 false positive in ArduinoJson 7.4.3 (CollectionIterator default constructor)
FLAGS="-std=gnu++23 -g -O1 -Wall -Wextra -Wno-maybe-uninitialized -Wno-unused-function -I. -isystem $AJ -I$R -I$WS"
g++ $FLAGS -fsanitize=address,undefined -fno-sanitize-recover=undefined "$H/harness.cpp" -o harness -lpthread
g++ $FLAGS -fsanitize=thread "$H/harness.cpp" -o harness_tsan -lpthread
set +e
./harness
rc=$?
echo "-- again under ThreadSanitizer"
TSAN_OPTIONS=halt_on_error=1 ./harness_tsan > tsan.log 2>&1
t=$?
tail -3 tsan.log
[ $t -ne 0 ] && grep -m1 -A12 'WARNING: ThreadSanitizer' tsan.log
exit $((rc + t))
