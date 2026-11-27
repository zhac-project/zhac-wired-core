#!/bin/sh
# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Host check of the "Allow script changes from the cloud" switch: a failed save never leaves it on, is
# reported to the page, and sending off again repairs a failed off (spec 2026-10-05 §3.1). Also "Cloud control"
# and "Allow the cloud to disarm the alarm" (review 2026-11-26 HL-03, HL-02): the first boot's level, a raise
# only once saved, a lowering at once, and the relay refusing the next command after a lowering.
#   sh run.sh                  build and run; exit code = number of mismatches
#   MUTANT=<name> sh run.sh    same, after breaking ONE line in the work copies (never in the repo), to show the
#                              harness can fail: ram_on_unsaved | ret_true | no_400 | no_heal | no_error_log |
#                              level_raise_unsaved | level_migrate_always | level_token_ignored | level_no_repair |
#                              level_hook_full | alarm_hook_on
# The code under test is copied verbatim from main/ (sys_state.cpp, sys_state.h, ws_bridge.h) or cut out of it
# (system_apply_settings and the relay hooks, by sed); remote_allow.cpp is zhac-components' own. Only the
# neighbours are stubs (stubs.h, harness.cpp). Needs the sibling zhac-components checkout (ArduinoJson,
# remote_client) next to this repo, g++ and PATH=/usr/bin first (set below).
set -e
H=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$H/../../../main" && pwd)
AJ=$(cd "$H/../../../../zhac-components/components/arduinojson/src" && pwd)
NC=$(cd "$H/../../../../zhac-components/components/ws_server/include" && pwd)   # utf8_seq.h
RCD=$(cd "$H/../../../../zhac-components/components/remote_client" && pwd)     # remote_allow.*
W=$(mktemp -d "${TMPDIR:-/tmp}/wired-host-switch.XXXXXX")
trap 'rm -rf "$W"' EXIT

cp "$R/sys_state.cpp" "$R/sys_state.h" "$R/ws_bridge.h" "$R/log_cloud.h" "$R/log_cloud_core.h" "$R/log_cloud_core.cpp" "$R/json_buf.h" "$H/stubs.h" "$H/slice_tu.cpp" "$H/harness.cpp" "$W/"
for h in nvs.h esp_log.h esp_random.h auth.h; do echo '#include "stubs.h"' > "$W/$h"; done
sed -n '/^bool system_apply_settings/,/^}/p' "$R/api_system.cpp" > "$W/system_apply_settings.inc"
test "$(wc -l < "$W/system_apply_settings.inc")" -gt 40   # the slice found the whole function
sed -n -E '/^extern "C" (bool|remote_level_t) remote_(scripts_allowed|level_now|alarm_allowed)\(void\)/p' "$R/ws_bridge.cpp" > "$W/remote_hooks.inc"
test "$(wc -l < "$W/remote_hooks.inc")" -eq 3 || { echo "ws_bridge.cpp: the three one-line relay hooks not found" >&2; exit 97; }

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
    level_raise_unsaved)  mutate sys_state.cpp 's/s_remote_level.store(sys_remote_level_after_save(s_remote_level.load(), level, e == ESP_OK));/s_remote_level.store(level);/' ;;
    level_migrate_always) mutate sys_state.cpp 's/if (e == ESP_OK) return v <= 2 ? v : 0;/if (e == ESP_OK) e = ESP_ERR_NVS_NOT_FOUND;/' ;;
    level_token_ignored)  mutate sys_state.cpp 's/const bool linked = cloud_token_stored();/const bool linked = true;/' ;;
    level_no_repair)      mutate system_apply_settings.inc 's/const bool saved = sys_set_remote_level(want);/const bool saved = want == was || sys_set_remote_level(want);/' ;;
    level_hook_full)      mutate remote_hooks.inc 's/{ return static_cast<remote_level_t>(sys_remote_level()); }/{ return REMOTE_LEVEL_FULL; }/' ;;
    alarm_hook_on)        mutate remote_hooks.inc 's/{ return sys_remote_alarm(); }/{ return true; }/' ;;
    *) echo "unknown MUTANT" >&2; exit 98 ;;
esac

cd "$W"
export PATH=/usr/bin:$PATH   # the ESP32-ULP `as` on PATH poisons g++
FLAGS="-std=c++17 -g -fsanitize=address,undefined -Wall -Wextra -I. -I$AJ -I$NC -I$RCD/include"
g++ $FLAGS -c sys_state.cpp -o sys_state.o
g++ $FLAGS -c log_cloud_core.cpp -o log_cloud_core.o
g++ $FLAGS -c slice_tu.cpp -o slice_tu.o
g++ $FLAGS -c harness.cpp -o harness.o
g++ $FLAGS -c "$RCD/remote_allow.cpp" -o remote_allow.o
g++ $FLAGS sys_state.o log_cloud_core.o slice_tu.o harness.o remote_allow.o -o harness
set +e
./harness
rc=$?
exit $rc
