#!/bin/sh
# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Host check of the "Allow script changes from the cloud" switch: a failed save never leaves it on, is
# reported to the page, and sending off again repairs a failed off (spec 2026-10-05 §3.1).
#   sh run.sh                  build and run; exit code = number of mismatches
#   MUTANT=<name> sh run.sh    same, after breaking ONE line in the work copies (never in the repo), to show the
#                              harness can fail: ram_on_unsaved | ret_true | no_400 | no_heal | no_error_log
# The code under test is copied verbatim from main/ (sys_state.cpp, sys_state.h, ws_bridge.h) or cut out of it
# (system_apply_settings, by sed); only the neighbours are stubs (stubs.h, harness.cpp). Needs the sibling
# zhac-components checkout (ArduinoJson) next to this repo, g++ and PATH=/usr/bin first (set below).
set -e
H=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$H/../../../main" && pwd)
AJ=$(cd "$H/../../../../zhac-components/components/arduinojson/src" && pwd)
NC=$(cd "$H/../../../../zhac-net-core/components/ws_server/include" && pwd)   # utf8_seq.h
W=$(mktemp -d "${TMPDIR:-/tmp}/wired-host-switch.XXXXXX")
trap 'rm -rf "$W"' EXIT

cp "$R/sys_state.cpp" "$R/sys_state.h" "$R/ws_bridge.h" "$R/log_cloud.h" "$R/log_cloud_core.h" "$R/log_cloud_core.cpp" "$R/json_buf.h" "$H/stubs.h" "$H/slice_tu.cpp" "$H/harness.cpp" "$W/"
for h in nvs.h esp_log.h esp_random.h auth.h; do echo '#include "stubs.h"' > "$W/$h"; done
sed -n '/^bool system_apply_settings/,/^}/p' "$R/api_system.cpp" > "$W/system_apply_settings.inc"
test "$(wc -l < "$W/system_apply_settings.inc")" -gt 40   # the slice found the whole function

mutate() {   # mutate <file> <sed expression>: fails unless the file really changed
    cp "$W/$1" "$W/$1.orig"
    sed -i "$2" "$W/$1"
    if cmp -s "$W/$1" "$W/$1.orig"; then echo "mutant did not apply" >&2; exit 99; fi
    rm "$W/$1.orig"
}
case "${MUTANT:-}" in
    "") ;;
    ram_on_unsaved) mutate sys_state.cpp 's/sys_remote_scripts_after_save(on, e == ESP_OK)/on/' ;;
    ret_true)       mutate sys_state.cpp 's/return e == ESP_OK;/return true;/' ;;
    no_error_log)   mutate sys_state.cpp 's/if (e != ESP_OK) ESP_LOGE/if (false) ESP_LOGE/' ;;
    no_400)         mutate system_apply_settings.inc 's/^\( *\)return false;$/\1(void)0;/' ;;
    no_heal)        mutate system_apply_settings.inc 's/if (!(on && was))/if (on != was)/' ;;
    *) echo "unknown MUTANT" >&2; exit 98 ;;
esac

cd "$W"
export PATH=/usr/bin:$PATH   # the ESP32-ULP `as` on PATH poisons g++
FLAGS="-std=c++17 -g -fsanitize=address,undefined -Wall -Wextra -I. -I$AJ -I$NC"
g++ $FLAGS -c sys_state.cpp -o sys_state.o
g++ $FLAGS -c log_cloud_core.cpp -o log_cloud_core.o
g++ $FLAGS -c slice_tu.cpp -o slice_tu.o
g++ $FLAGS -c harness.cpp -o harness.o
g++ $FLAGS sys_state.o log_cloud_core.o slice_tu.o harness.o -o harness
set +e
./harness
rc=$?
exit $rc
