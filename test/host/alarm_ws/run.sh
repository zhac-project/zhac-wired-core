#!/bin/sh
# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Host check of the hub page's alarm.* commands (spec 2026-11-24 Safety profiles): only an absent id means "no id"
# (a null or empty one disarmed every profile, review W18), and alarm.get answers an error instead of a cut config
# when its document or its reply runs out of memory (W20).
#   sh run.sh                  build and run; exit code = number of mismatches (ASan/UBSan abort = non-zero too)
#   MUTANT=<name> sh run.sh    same, after breaking ONE line in the work copy (never in the repo), to show the
#                              harness can fail: null_id empty_id no_doc_check no_env_check
#   WS_SRC=<file> sh run.sh    the same checks against another ws_bridge.cpp (a RED run: `git show <rev>:main/ws_bridge.cpp`)
# The code under test is cut verbatim out of main/ws_bridge.cpp by sed (send_err, reply_data, reply_ok_or_err,
# cmd_alarm_get, alarm_profile_arg, cmd_alarm_arm / disarm / silence); JsonWriter, psram_json.h, alarm.h, utf8_seq.h
# and ArduinoJson are the real headers. Only the neighbours are stubs (esp_heap_caps.h, harness.cpp): the
# allocator, the reply sink, the pool and the panel. Needs the sibling checkout (zhac-components) next to this repo,
# g++, and PATH=/usr/bin first (set below).
set -e
H=$(cd "$(dirname "$0")" && pwd)
Z=$(cd "$H/../../../.." && pwd)
R=$Z/zhac-wired-core/main
C=$Z/zhac-components/components
AJ=$C/arduinojson/src
WS=$C/ws_server/include
W=$(mktemp -d "${TMPDIR:-/tmp}/wired-host-alarm-ws.XXXXXX")
trap 'rm -rf "$W"' EXIT
cp "$H/esp_heap_caps.h" "$H/slice_tu.cpp" "$H/harness.cpp" "$W/"

# the real commands, one function at a time (single-line signatures, closing brace in column 0)
WSB=${WS_SRC:-$R/ws_bridge.cpp}
: > "$W/ws_slice.inc"
for f in send_err reply_data reply_ok_or_err cmd_alarm_get alarm_profile_arg cmd_alarm_arm cmd_alarm_disarm cmd_alarm_silence; do
    sed -n "/^static [a-z_0-9]* $f(.*{\$/,/^}/p" "$WSB" > "$W/$f.part"
    test "$(wc -l < "$W/$f.part")" -gt 4          # the slice found the whole function
    test "$(grep -c '^}' "$W/$f.part")" -eq 1     # and only that function
    cat "$W/$f.part" >> "$W/ws_slice.inc"
    echo >> "$W/ws_slice.inc"
    rm "$W/$f.part"
done

mutate() {   # mutate <file> <sed expression>: fails unless the file really changed
    cp "$W/$1" "$W/$1.orig"
    sed -i "$2" "$W/$1"
    if cmp -s "$W/$1" "$W/$1.orig"; then echo "mutant did not apply" >&2; exit 99; fi
    rm "$W/$1.orig"
}
case "${MUTANT:-}" in
    "") ;;
    null_id)      mutate ws_slice.inc 's/if (v\.isUnbound()) return true;/if (v.isNull()) return true;/' ;;
    empty_id)     mutate ws_slice.inc 's/if (!s || !\*s) {/if (!s) {/' ;;
    no_doc_check) mutate ws_slice.inc 's/^    if (d\.overflowed()).*$//' ;;
    no_env_check) mutate ws_slice.inc 's/^    if (env\.overflowed()).*$//' ;;
    *) echo "unknown MUTANT" >&2; exit 98 ;;
esac

cd "$W"
export PATH=/usr/bin:$PATH   # the ESP32-ULP `as` on PATH poisons g++
# -Wno-maybe-uninitialized: GCC 15 false positive in ArduinoJson 7.4.3 (CollectionIterator default constructor)
FLAGS="-std=gnu++23 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -Wall -Wextra -Wno-maybe-uninitialized -I. -isystem $AJ -I$R -I$WS"
g++ $FLAGS -c slice_tu.cpp -o slice_tu.o
g++ $FLAGS -c harness.cpp -o harness.o
g++ $FLAGS slice_tu.o harness.o -o harness
set +e
./harness
rc=$?
exit $rc
