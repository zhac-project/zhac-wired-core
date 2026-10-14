#!/bin/sh
# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Host check of main/device_options_key.h (review F19).
#   sh run.sh        build and run; exit code = number of mismatches
set -e
H=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$H/../../../main" && pwd)
W=$(mktemp -d "${TMPDIR:-/tmp}/wired-host-optkey.XXXXXX")
trap 'rm -rf "$W"' EXIT
export PATH=/usr/bin:$PATH   # the ESP32-ULP `as` on PATH poisons g++
g++ -std=c++17 -g -fsanitize=address,undefined -Wall -Wextra -I"$R" "$H/harness.cpp" -o "$W/harness"
set +e
"$W/harness"
