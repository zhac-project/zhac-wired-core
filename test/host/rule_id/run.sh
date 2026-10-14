#!/bin/sh
# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Host check of parse_id_from_uri (main/api_rules.cpp, cut out by sed): review F34.
#   sh run.sh        build and run; exit code = number of mismatches
set -e
H=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$H/../../../main" && pwd)
W=$(mktemp -d "${TMPDIR:-/tmp}/wired-host-ruleid.XXXXXX")
trap 'rm -rf "$W"' EXIT
sed -n '/^static uint16_t parse_id_from_uri/,/^}/p' "$R/api_rules.cpp" > "$W/slice.inc"
test -s "$W/slice.inc"
export PATH=/usr/bin:$PATH   # the ESP32-ULP `as` on PATH poisons g++
g++ -std=c++17 -g -fsanitize=address,undefined -Wall -Wextra -Wno-unused-function -I"$W" "$H/harness.cpp" -o "$W/harness"
set +e
"$W/harness"
