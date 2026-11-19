#!/bin/sh
# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Host check of the full backup (research 2026-11-18 C1): main/backup_format.h (file and stage headers, the frame
# counter target, the firmware version check) and main/backup_crypto.cpp (PBKDF2, key check, streamed AES-256-GCM,
# SHA-256) on the PSA crypto library of the IDF tree, compiled for the host once into $TMPDIR/zhac-tfpsa-host.
#   sh run.sh        build and run; exit code = number of mismatches
# Needs ESP-IDF v6.0 (IDF_PATH, else ~/.espressif/v6.0/esp-idf), g++/gcc and PATH=/usr/bin first.
set -e
H=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$H/../../../main" && pwd)
W=$(mktemp -d "${TMPDIR:-/tmp}/wired-host-backup.XXXXXX")
trap 'rm -rf "$W"' EXIT
export PATH=/usr/bin:$PATH   # the ESP32-ULP `as` on PATH poisons g++
IDF=${IDF_PATH:-$HOME/.espressif/v6.0/esp-idf}
T=$IDF/components/mbedtls/mbedtls/tf-psa-crypto
L=${TMPDIR:-/tmp}/zhac-tfpsa-host
if [ ! -f "$L/libtfpsa.a" ]; then
    # IDF wraps two private headers (port/include) and adds sdkconfig.h to one file: stand-ins for the host.
    mkdir -p "$L/o" "$L/shim/mbedtls"
    : > "$L/shim/sdkconfig.h"
    for x in bignum ecp; do
        printf '#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS\n#include "mbedtls/private/%s.h"\n' $x > "$L/shim/mbedtls/$x.h"
    done
    ls "$T"/core/*.c "$T"/drivers/builtin/src/*.c | xargs -P8 -I{} sh -c \
        'gcc -O2 -c "$1" -I"$2/shim" -I"$3/include" -I"$3/drivers/builtin/include" -I"$3/core" \
             -I"$3/drivers/builtin/src" -o "$2/o/$(basename "$1" .c).o"' _ {} "$L" "$T"
    ar rcs "$L/libtfpsa.a" "$L"/o/*.o
fi
g++ -std=c++17 -g -fsanitize=address,undefined -Wall -Wextra -I"$R" -I"$T/include" -I"$T/drivers/builtin/include" \
    "$H/harness.cpp" "$R/backup_crypto.cpp" "$L/libtfpsa.a" -o "$W/harness"
set +e
"$W/harness"
rc=$?
# The same file on both sides: one the firmware's code made, read by tools/zhac_backup.py, and the reverse.
export ZHAC_BACKUP_PASSWORD="correct horse battery"
TOOL=$(cd "$H/../../../tools" && pwd)/zhac_backup.py
X=$W/x
mkdir -p "$X" "$W/py"
fail() { echo "  FAIL: $1"; rc=$((rc + 1)); }
"$W/harness" export "$X" > /dev/null || fail "harness export"
python3 "$TOOL" decrypt "$X/fw.zhacbak" "$W/py" > "$W/py.log" 2>&1 || { cat "$W/py.log"; fail "python decrypt"; }
for p in nvs zb_storage zhac_data scripts; do
    if cmp -s "$X/$p.bin" "$W/py/$p.bin"; then echo "  ok:   python reads the firmware's $p"; else fail "python reads the firmware's $p"; fi
done
grep -q "0xE20000 $W/py/zb_storage.bin" "$W/py.log" && echo "  ok:   python prints the USB offsets" || fail "USB offsets"
python3 "$TOOL" info "$X/fw.zhacbak" | grep -q "firmware v2026111903, made 2026-.*NWK frame counter 123456" \
    && echo "  ok:   python info" || fail "python info"
ZHAC_BACKUP_PASSWORD="wrong password" python3 "$TOOL" decrypt "$X/fw.zhacbak" "$W/bad" 2>&1 | grep -q "wrong password" \
    && echo "  ok:   python refuses a wrong password" || fail "python wrong password"
python3 "$TOOL" encrypt "$W/py.zhacbak" nvs="$X/nvs.bin" zb_storage="$X/zb_storage.bin" zhac_data="$X/zhac_data.bin" \
    scripts="$X/scripts.bin" --fw v2026111903 --nwk-fc 42 || fail "python encrypt"
if "$W/harness" import "$W/py.zhacbak" "$X"; then echo "  ok:   the firmware restores python's file"; else fail "firmware import"; fi
exit $rc
