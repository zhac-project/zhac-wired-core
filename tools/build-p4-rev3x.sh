#!/bin/sh
# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Build the ESP32-P4 silicon v3.x images, each merged for offset 0:
#   build.p4rev3x/zhac-wired-p4-rev3x.bin                       the hub
#   tools/c6-rcp-installer/build.rev3x/c6-rcp-installer-merged.bin  the C6 radio installer
# Same sources as the v1.x images; only sdkconfig.defaults.p4rev3x is added,
# last, so it overrides the v1.x gate. A script rather than a workflow line
# because the defaults list needs a quoted ';'.
#
# Needs an ESP-IDF v6.0 environment, and the ot_rcp app the installer embeds
# built first (build.rcp-ota/, see release.yml).
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
INST=$ROOT/tools/c6-rcp-installer
OVERLAY=$ROOT/sdkconfig.defaults.p4rev3x
export IDF_TARGET=esp32p4

hub() {
    idf.py -C "$ROOT" -B "$ROOT/build.p4rev3x" -D SDKCONFIG="$ROOT/build.p4rev3x/sdkconfig" \
        -D SDKCONFIG_DEFAULTS="$ROOT/sdkconfig.defaults;$OVERLAY" "$@"
}
installer() {
    idf.py -C "$INST" -B "$INST/build.rev3x" -D SDKCONFIG="$INST/build.rev3x/sdkconfig" \
        -D SDKCONFIG_DEFAULTS="$INST/sdkconfig.defaults;$OVERLAY" "$@"
}

hub build
hub merge-bin -o zhac-wired-p4-rev3x.bin
"$ROOT/tools/check_resolved_config.sh" "$ROOT/build.p4rev3x/sdkconfig" rev3x

installer build
installer merge-bin -o c6-rcp-installer-merged.bin
grep -qx 'CONFIG_ESP32P4_REV_MIN_FULL=301' "$INST/build.rev3x/sdkconfig" || {
    echo "FAIL: the v3.x installer did not resolve to REV_MIN_FULL=301" >&2; exit 1; }
echo "OK: v3.x installer revision family"
