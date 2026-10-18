#!/bin/sh
# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Build the Wi-Fi test variant (ZHAC_WIFI_HOSTED), each image merged for
# offset 0:
#
#   tools/build-p4-wifi-hosted.sh          P4 silicon v1.x -> build.wifi/zhac-wired-p4-wifi.bin
#   tools/build-p4-wifi-hosted.sh rev3x    P4 silicon v3.x -> build.wifi-rev3x/zhac-wired-p4-rev3x-wifi.bin
#
# and, either way, the ESP32-C5 co-processor:
#   build.c5cp/build/c5-wifi-cp-merged.bin
#
# The C5 image is ESP-Hosted's own slave project taken from the esp_hosted the
# hub build just resolved (managed_components/), so both ends always run the
# same protocol version. It is copied out first: building inside
# managed_components would trip the component manager's checksum check.
#
# Needs an ESP-IDF v6.0 environment and a built ../../www-spa/dist.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
export IDF_TARGET=esp32p4

DEFAULTS="$ROOT/sdkconfig.defaults"
B="$ROOT/build.wifi"
OUT=zhac-wired-p4-wifi.bin
if [ "$1" = rev3x ]; then
    DEFAULTS="$DEFAULTS;$ROOT/sdkconfig.defaults.p4rev3x"
    B="$ROOT/build.wifi-rev3x"
    OUT=zhac-wired-p4-rev3x-wifi.bin
fi
DEFAULTS="$DEFAULTS;$ROOT/sdkconfig.defaults.wifi-hosted"

hub() {
    idf.py -C "$ROOT" -B "$B" -D SDKCONFIG="$B/sdkconfig" -D SDKCONFIG_DEFAULTS="$DEFAULTS" "$@"
}
hub build
hub merge-bin -o "$OUT"
grep -qx 'CONFIG_ZHAC_WIFI_HOSTED=y' "$B/sdkconfig" || {
    echo "FAIL: $B/sdkconfig did not resolve ZHAC_WIFI_HOSTED=y" >&2; exit 1; }
grep -qx 'CONFIG_ESP_HOSTED_SPI_HOST_INTERFACE=y' "$B/sdkconfig" || {
    echo "FAIL: esp_hosted is not on SPI -- it would claim the C6 radio's SDIO pins" >&2; exit 1; }
if [ "$1" = rev3x ]; then "$ROOT/tools/check_resolved_config.sh" "$B/sdkconfig" rev3x; fi

SLAVE="$ROOT/managed_components/espressif__esp_hosted/slave"
CP="$ROOT/build.c5cp"
[ -d "$SLAVE" ] || { echo "FAIL: $SLAVE missing -- the hub build did not fetch esp_hosted" >&2; exit 1; }
rm -rf "$CP/src"
mkdir -p "$CP"
cp -r "$SLAVE" "$CP/src"
IDF_TARGET=esp32c5 idf.py -C "$CP/src" -B "$CP/build" -D SDKCONFIG="$CP/sdkconfig" \
    -D SDKCONFIG_DEFAULTS="$CP/src/sdkconfig.defaults;$ROOT/tools/c5-wifi-cp.defaults" build
IDF_TARGET=esp32c5 idf.py -C "$CP/src" -B "$CP/build" -D SDKCONFIG="$CP/sdkconfig" \
    -D SDKCONFIG_DEFAULTS="$CP/src/sdkconfig.defaults;$ROOT/tools/c5-wifi-cp.defaults" \
    merge-bin -o c5-wifi-cp-merged.bin
grep -qx 'CONFIG_ESP_SPI_HOST_INTERFACE=y' "$CP/sdkconfig" || {
    echo "FAIL: the C5 image is not on SPI" >&2; exit 1; }
echo "OK: $B/$OUT and $CP/build/c5-wifi-cp-merged.bin"
