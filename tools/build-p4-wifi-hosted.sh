#!/bin/sh
# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Build the Wi-Fi test variant (ZHAC_WIFI_HOSTED), each image merged for
# offset 0:
#
#   tools/build-p4-wifi-hosted.sh          P4 silicon v1.x -> build.wifi/zhac-wired-p4-wifi.bin
#   tools/build-p4-wifi-hosted.sh rev3x    P4 silicon v3.x -> build.wifi-rev3x/zhac-wired-p4-rev3x-wifi.bin
#   tools/build-p4-wifi-hosted.sh s31      ESP32-S31       -> build.wifi-s31/zhac-wired-s31-wifi.bin
#   tools/build-p4-wifi-hosted.sh s31-native
#       ESP32-S31, own radio for Wi-Fi, Zigbee on an external ot_rcp (no C5 image)
#                                          -> build.wifi-s31-native/zhac-wired-s31-wifi-native.bin
#
# and, for the two P4 builds, the ESP32-C5 co-processor:
#   build.c5cp/build/c5-wifi-cp-merged.bin
# The S31 uses the same C5 image (same C5 pins, same esp_hosted), so s31 does
# not rebuild it.
#
# The C5 image is ESP-Hosted's own slave project taken from the esp_hosted the
# hub build just resolved (managed_components/), so both ends always run the
# same protocol version. It is copied out first: building inside
# managed_components would trip the component manager's checksum check.
#
# Needs a built ../../www-spa/dist and the matching ESP-IDF environment:
#   P4:  . ~/.espressif/v6.0/esp-idf/export.sh
#   S31: . tools/s31-env.sh
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
export IDF_TARGET=esp32p4

DEFAULTS="$ROOT/sdkconfig.defaults"
B="$ROOT/build.wifi"
OUT=zhac-wired-p4-wifi.bin
PREVIEW=
if [ "$1" = rev3x ]; then
    DEFAULTS="$DEFAULTS;$ROOT/sdkconfig.defaults.p4rev3x"
    B="$ROOT/build.wifi-rev3x"
    OUT=zhac-wired-p4-rev3x-wifi.bin
elif [ "$1" = s31 ]; then
    export IDF_TARGET=esp32s31
    B="$ROOT/build.wifi-s31"
    OUT=zhac-wired-s31-wifi.bin
    PREVIEW=--preview   # esp32s31 is still a preview target in IDF 6.1
elif [ "$1" = s31-native ]; then
    export IDF_TARGET=esp32s31
    B="$ROOT/build.wifi-s31-native"
    OUT=zhac-wired-s31-wifi-native.bin
    PREVIEW=--preview
fi
# IDF also loads <file>.<target> right after each file named here, so the S31
# picks up sdkconfig.defaults.esp32s31 and sdkconfig.defaults.wifi-hosted.esp32s31.
if [ "$1" = s31-native ]; then
    DEFAULTS="$DEFAULTS;$ROOT/sdkconfig.defaults.wifi-native-s31"
else
    DEFAULTS="$DEFAULTS;$ROOT/sdkconfig.defaults.wifi-hosted"
fi

hub() {
    idf.py $PREVIEW -C "$ROOT" -B "$B" -D SDKCONFIG="$B/sdkconfig" -D SDKCONFIG_DEFAULTS="$DEFAULTS" "$@"
}
hub build
hub merge-bin -o "$OUT"
if [ "$1" = s31-native ]; then
    for c in ZHAC_WIFI_NATIVE=y ZB_RADIO_SPINEL_UART=y LOG_MAXIMUM_LEVEL_VERBOSE=y; do
        grep -qx "CONFIG_$c" "$B/sdkconfig" || { echo "FAIL: $B/sdkconfig did not resolve $c" >&2; exit 1; }
    done
    if grep -q '^CONFIG_ESP_HOSTED' "$B/sdkconfig"; then
        echo "FAIL: esp_hosted is in the native-Wi-Fi build" >&2; exit 1
    fi
    echo "OK: $B/$OUT (Zigbee radio: ot_rcp on UART, not built here)"
    exit 0
fi
grep -qx 'CONFIG_ZHAC_WIFI_HOSTED=y' "$B/sdkconfig" || {
    echo "FAIL: $B/sdkconfig did not resolve ZHAC_WIFI_HOSTED=y" >&2; exit 1; }
grep -qx 'CONFIG_ESP_HOSTED_SPI_HOST_INTERFACE=y' "$B/sdkconfig" || {
    echo "FAIL: esp_hosted is not on SPI -- it would claim the C6 radio's SDIO pins" >&2; exit 1; }
grep -qx 'CONFIG_ESP_HOSTED_MEMPOOL_PREFER_SPIRAM=y' "$B/sdkconfig" || {
    echo "FAIL: esp_hosted buffers would sit in internal RAM" >&2; exit 1; }
if [ "$1" = rev3x ]; then "$ROOT/tools/check_resolved_config.sh" "$B/sdkconfig" rev3x; fi
if [ "$1" = s31 ]; then
    grep -qx 'CONFIG_ESP_HOSTED_SPI_HSPI_GPIO_CLK=24' "$B/sdkconfig" || {
        echo "FAIL: S31 pins not applied (sdkconfig.defaults.wifi-hosted.esp32s31)" >&2; exit 1; }
    echo "OK: $B/$OUT (C5: build.c5cp/build/c5-wifi-cp-merged.bin from a P4 run)"
    exit 0
fi

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
