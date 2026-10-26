#!/bin/sh
# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Host check of main/auth.cpp: turning sign-in on signs out sockets that were let in while it was off
# (review F02), wrong WebSocket tokens lock out only the peer that sent them (F31), and the sign-in
# routes' JSON rule, lockouts and console token (review 2026-10-24 HS-05, HS-06, HS-07).
#   sh run.sh        build and run; exit code = number of mismatches
# auth.cpp, auth.h and req_filter.h are copied verbatim from main/; only their neighbours are stubs (stubs.h,
# harness.cpp). Needs the sibling zhac-components checkout (ArduinoJson), g++ and PATH=/usr/bin first.
set -e
H=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$H/../../../main" && pwd)
AJ=$(cd "$H/../../../../zhac-components/components/arduinojson/src" && pwd)
W=$(mktemp -d "${TMPDIR:-/tmp}/wired-host-auth.XXXXXX")
trap 'rm -rf "$W"' EXIT

cp "$R/auth.cpp" "$R/auth.h" "$R/req_filter.h" "$R/sec_headers.h" "$H/stubs.h" "$H/harness.cpp" "$W/"
mkdir -p "$W/lwip" "$W/freertos" "$W/psa"
for h in esp_http_server.h esp_log.h esp_random.h esp_timer.h net_discovery.h nvs.h sdkconfig.h ws_server.h \
         zap_setup_window.h lwip/sockets.h freertos/FreeRTOS.h freertos/semphr.h psa/crypto.h; do
    echo '#include "stubs.h"' > "$W/$h"
done

cd "$W"
export PATH=/usr/bin:$PATH   # the ESP32-ULP `as` on PATH poisons g++
FLAGS="-std=c++17 -g -fsanitize=address,undefined -Wall -Wextra -I. -I$AJ"
g++ $FLAGS auth.cpp harness.cpp -Wl,--wrap=getpeername -Wl,--wrap=printf -o harness
set +e
./harness
