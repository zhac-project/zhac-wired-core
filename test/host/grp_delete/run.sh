#!/bin/sh
# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Host check of a group delete cut by a power loss (review PB-08): the real main/groups_store.cpp over
# the fake NVS and FreeRTOS headers in ../nvs_fake.
#   sh run.sh        build and run; exit code = number of mismatches
# Needs the sibling zhac-net-core checkout (utf8_seq.h, for json_buf.h), g++ and PATH=/usr/bin first.
set -e
H=$(cd "$(dirname "$0")" && pwd)
F=$(cd "$H/../nvs_fake" && pwd)
R=$(cd "$H/../../../main" && pwd)
WS=$(cd "$H/../../../../zhac-net-core/components/ws_server/include" && pwd)
W=$(mktemp -d "${TMPDIR:-/tmp}/wired-host-grp-delete.XXXXXX")
trap 'rm -rf "$W"' EXIT

cd "$W"
export PATH=/usr/bin:$PATH   # the ESP32-ULP `as` on PATH poisons g++
g++ -std=c++17 -g -fsanitize=address,undefined -Wall -Wextra -I"$F/include" -I"$R" -I"$WS" \
    "$R/groups_store.cpp" "$F/nvs_fake.cpp" "$H/harness.cpp" -o harness
set +e
./harness
