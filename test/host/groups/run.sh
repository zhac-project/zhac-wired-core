#!/bin/sh
# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Host check of the group JSON and group commands (review F10, F34): grp_to_json
# (main/groups_store.cpp), group_list_json and group_cmd (main/api_groups.cpp) cut
# out by sed; json_buf.h, groups_store.h and zhac-components' device_cmd JSON
# helpers copied verbatim; the store and the device layer are fakes in harness.cpp.
#   sh run.sh        build and run; exit code = number of mismatches
# Needs the sibling zhac-components checkout (ArduinoJson), g++ and PATH=/usr/bin first.
set -e
H=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$H/../../../main" && pwd)
AJ=$(cd "$H/../../../../zhac-components/components/arduinojson/src" && pwd)
DC=$(cd "$H/../../../../zhac-components/components/device_cmd" && pwd)
WS=$(cd "$H/../../../../zhac-net-core/components/ws_server/include" && pwd)   # utf8_seq.h, for json_buf.h
W=$(mktemp -d "${TMPDIR:-/tmp}/wired-host-groups.XXXXXX")
trap 'rm -rf "$W"' EXIT

cp "$R/groups_store.h" "$R/json_buf.h" "$H/harness.cpp" "$DC/include/device_cmd.h" "$DC/include/device_cmd_json.h" \
   "$DC/device_cmd_json.cpp" "$W/"
echo '#pragma once' > "$W/nvs.h"
{ echo '#include "groups_store.h"'; echo '#include "json_buf.h"'; echo '#include "device_cmd_json.h"'
  echo '#include <cstdio>'; echo '#include <cstring>'; echo 'bool grp_find(uint16_t id, GrpRecord& out);'
  sed -n '/^size_t grp_to_json/,/^}/p' "$R/groups_store.cpp"
  sed -n '/^size_t group_list_json/,/^}/p' "$R/api_groups.cpp"
  sed -n '/^size_t group_cmd/,/^}/p' "$R/api_groups.cpp"; } > "$W/slice_tu.cpp"
test "$(grep -c '^}' "$W/slice_tu.cpp")" -eq 3   # all three functions were found

cd "$W"
export PATH=/usr/bin:$PATH   # the ESP32-ULP `as` on PATH poisons g++
FLAGS="-std=c++17 -g -fsanitize=address,undefined -Wall -Wextra -I. -I$AJ -I$WS"
g++ $FLAGS slice_tu.cpp device_cmd_json.cpp harness.cpp -o harness
set +e
./harness
