#!/bin/sh
# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Host check of the alarm panel's glue (main/alarm.cpp, Safety profiles, spec 2026-11-24): sensors and keypads in,
# the task's pass (state records, notifications, the Alarm device, events, outputs, the page), saves, restarts.
# The REAL alarm.cpp is copied from main/ and included whole by harness.cpp, with the real alarm_core.cpp; its task
# loop runs in turns with the test, on a fake clock. Only the neighbours are fakes (stubs/, harness.cpp), and each of them
# aborts when it is called under the alarm mutex.
#   sh run.sh                  build and run the five modes (main, boot, legacy, fallback: one boot each; restart:
#                              chains of boots on one flash, for power cuts and restarts); exit code = the modes that failed
#   MUTANT=<name> sh run.sh    same, after breaking ONE line in the work copy of alarm.cpp (never in the repo), to
#                              show the harness can fail: cfg_overwritten | no_orphans | no_keep_follow | st_ignored |
#                              cloud_one_name | replaces_any
# Needs the sibling zhac-components checkout next to this repo (ZHAC_COMPONENTS overrides) and g++.
set -e
H=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$H/../../.." && pwd)
C=${ZHAC_COMPONENTS:-$(cd "$R/../zhac-components/components" && pwd)}
W=$(mktemp -d "${TMPDIR:-/tmp}/wired-host-alarm-glue.XXXXXX")
trap 'rm -rf "$W"' EXIT
export PATH=/usr/bin:$PATH   # the ESP32-ULP `as` on PATH poisons g++

cp "$R/main/alarm.cpp" "$W/alarm.cpp"
mutate() {   # mutate <sed expression>: fails unless the copy really changed
    cp "$W/alarm.cpp" "$W/alarm.cpp.orig"
    sed -i "$1" "$W/alarm.cpp"
    if cmp -s "$W/alarm.cpp" "$W/alarm.cpp.orig"; then echo "mutant did not apply" >&2; exit 99; fi
    rm "$W/alarm.cpp.orig"
}
case "${MUTANT:-}" in
    "") ;;
    cfg_overwritten) mutate 's/nvs_put("cfg2", buf, n)/nvs_put("cfg", buf, n)/' ;;                     # the stage 1 config is rewritten
    no_orphans)      mutate 's/if (k >= 0 && !s_cfg->outputs\[k\].keep && s_n_orphans < kAlarmMaxOutputs)/if (false)/' ;;   # a save forgets what still sounds
    no_keep_follow)  mutate 's/for (uint8_t k = 0; kept && k < s_cfg->n_outputs; k++) {/for (uint8_t k = 0; false \&\& k < s_cfg->n_outputs; k++) {/' ;;   # a collapsed alarm closes no valve
    st_ignored)      mutate 's/s_st_have ? s_st_saved : nullptr, \*s_cfg, s_panel/nullptr, *s_cfg, s_panel/' ;;   # a newer stage 1 state is not read at boot
    cloud_one_name)  mutate 's/notify("alarm_changed_by_cloud", 0, what\[0\] ? what : nullptr, msg);/notify("alarm_changed_by_cloud", 0, nullptr, msg);/' ;;   # the cloud's repeat filter would merge a disarm into the arm before it
    replaces_any)    mutate 's/if (i != p \&\& s_cfg->profiles\[i\].cls == AlarmClass::INTRUSION \&\& s_panel/if (i != p \&\& s_panel/' ;;   # an armed water profile would refuse the cloud's arm of Night
    *) echo "unknown MUTANT" >&2; exit 98 ;;
esac

INC="-I$W -I$H/stubs -I$R/main -I$C/arduinojson/src -I$C/ws_server/include"
for c in device_cmd device_shadow event_bus remote_client tg_gw zap_common zap_store zhc_adapter zigbee_mgr; do
    INC="$INC -I$C/$c/include"
done
# shellcheck disable=SC2086
g++ -std=gnu++20 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra -Werror -Wno-unused-function \
    -Wno-maybe-uninitialized -pthread $INC "$H/harness.cpp" "$R/main/alarm_core.cpp" -o "$W/harness"
set +e
fails=0
for m in main boot legacy fallback restart; do
    # No leak check: the panel lives as long as the hub, and the task's thread is still waiting when a mode ends.
    ASAN_OPTIONS=detect_leaks=0 "$W/harness" $m || fails=$((fails + 1))
done
exit $fails
