#!/bin/sh
# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Regenerates nvs_pin.bin, test_nvs_raw.cpp's fixture, with IDF's own NVS partition generator (a real NVS image, not
# this repo's idea of the format). 16 KB: "other" fills page 1 (a decoy "ieee" first, then 150 keys), page 2 holds a
# string spanning several entries and then zb_coord/ieee = 0x00124B0012345678.
#   sh gen_nvs_pin.sh [python with esp_idf_nvs_partition_gen]
set -e
cd "$(dirname "$0")"
PY=${1:-$(ls -d ~/.espressif/python_env/idf6.0_py3.14_env/bin/python 2>/dev/null || echo python3)}
CSV=$(mktemp "${TMPDIR:-/tmp}/nvs_pin.XXXXXX.csv")
trap 'rm -f "$CSV"' EXIT
{
  echo "key,type,encoding,value"
  echo "other,namespace,,"
  echo "ieee,data,u64,$((0x1111111111111111))"
  i=0; while [ $i -lt 150 ]; do printf 'k%03d,data,u8,%d\n' $i $((i % 256)); i=$((i + 1)); done
  echo "zb_coord,namespace,,"
  echo "note,data,string,the coordinator address the network first ran with; kept across an erase"
  echo "ieee,data,u64,$((0x00124B0012345678))"
} > "$CSV"
"$PY" -m esp_idf_nvs_partition_gen generate "$CSV" nvs_pin.bin 0x4000
