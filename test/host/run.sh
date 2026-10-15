#!/bin/sh
# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Host checks of the wired hub's central WebSocket / REST handlers (remote rules and scripts, spec 2026-10-05):
#   switch/   the "Allow script changes from the cloud" switch: a failed save never leaves it on (sys_state.cpp,
#             system_apply_settings)
#   rules/    rule.list paging, rule.create / update (base) / check, the rule.* pushes (ws_bridge.cpp)
#   scripts/  script.list / read / part / write / check / reload, script.error, the REST handlers (ws_bridge.cpp,
#             api_scripts.cpp), on real Lua 5.5
# Each runs the REAL code, cut out of main/ by sed or copied, against stubs of its neighbours, under ASan/UBSan,
# and each takes MUTANT=<name> to show it can fail (see its run.sh). Needs the sibling checkouts next to this repo
# (zhac-components, zhac-net-core, zhac-main-core with managed_components/georgik__lua), g++, libcrypto.
#   sh test/host/run.sh        exit code = the suites that failed
fails=0
for s in switch rules scripts; do
    if sh "$(dirname "$0")/$s/run.sh" > /dev/null 2>&1; then echo "$s: ok"; else echo "$s: FAILED (sh test/host/$s/run.sh shows why)"; fails=$((fails + 1)); fi
done
exit $fails
