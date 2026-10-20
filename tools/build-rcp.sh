#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Build the ESP32-C6 radio image the P4 board needs: Espressif's stock
# examples/openthread/ot_rcp with rcp/sdkconfig.defaults layered on top,
# merged into ONE file for offset 0x0.
#
#   . ~/.espressif/v6.0/esp-idf/export.sh
#   tools/build-rcp.sh build.rcp/zhac-rcp-c6.bin
#
# RCP_TARGET=esp32c5 builds the C5 radio for the S31 test variant S-B instead
# (pins in rcp/sdkconfig.defaults.esp32c5, which IDF loads after the base file;
# build it under the S31's IDF, tools/s31-env.sh, so both ends share one
# OpenThread).
#
# RCP_EXTRA_DEFAULTS=<abs path> layers one more defaults file on top (used by
# tools/c6-rcp-installer for its slave-OTA variant).
#
# The example is copied into the build directory first, so neither the build
# nor the component manager writes into the ESP-IDF tree.
set -euo pipefail
: "${IDF_PATH:?source ESP-IDF v6.0 export.sh first}"
IDF_PY=${IDF_PY:-idf.py}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=$(realpath -m "${1:?usage: tools/build-rcp.sh <out.bin> [work-dir]}")
WORK=$(realpath -m "${2:-$ROOT/build.rcp}")
SRC="$WORK/ot_rcp"
TARGET=${RCP_TARGET:-esp32c6}
case $TARGET in
    esp32c6) TX=20 RX=21 ;;
    esp32c5) TX=9 RX=8 ;;
    *) echo "build-rcp: no pins for $TARGET" >&2; exit 1 ;;
esac

rm -rf "$SRC" && mkdir -p "$WORK"
cp -r "$IDF_PATH/examples/openthread/ot_rcp" "$SRC"
$IDF_PY -C "$SRC" -B "$WORK/build" \
    -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;$ROOT/rcp/sdkconfig.defaults${RCP_EXTRA_DEFAULTS:+;$RCP_EXTRA_DEFAULTS}" \
    set-target "$TARGET" build
# The pins are the whole point of this image: refuse to publish one without them.
grep -qx "CONFIG_OPENTHREAD_UART_TX_PIN=$TX" "$SRC/sdkconfig" &&
grep -qx "CONFIG_OPENTHREAD_UART_RX_PIN=$RX" "$SRC/sdkconfig" ||
    { echo "build-rcp: UART pins did not apply" >&2; exit 1; }
$IDF_PY -C "$SRC" -B "$WORK/build" merge-bin -o "$OUT"
echo "build-rcp: $OUT"
