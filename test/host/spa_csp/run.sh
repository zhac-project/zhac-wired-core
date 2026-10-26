#!/bin/sh
# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Host check of the web UI's Content-Security-Policy (review HS-14): main/spa_csp.h as spa_serve.cpp uses it.
#   sh run.sh        build and run; exit code = number of mismatches
# Needs g++ and PATH=/usr/bin first.
set -e
H=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$H/../../../main" && pwd)
W=$(mktemp -d "${TMPDIR:-/tmp}/wired-host-spa-csp.XXXXXX")
trap 'rm -rf "$W"' EXIT

cd "$W"
export PATH=/usr/bin:$PATH   # the ESP32-ULP `as` on PATH poisons g++
g++ -std=c++17 -g -fsanitize=address,undefined -Wall -Wextra -I"$R" "$H/harness.cpp" -o harness
set +e
./harness
