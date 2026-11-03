#!/bin/sh
# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# An installed hub keeps the partition table it was flashed with, so an app image that outgrows its OTA slot
# can no longer be installed over the network (review 2026-10-24 PB-07). Fail while there is room to act:
# past 90 % of the smallest app partition in partitions.csv.
#   tools/check_app_fits.sh build [build.s31 ...]      build directories, relative to the repo root
set -e
cd "$(dirname "$0")/.."
python3 - "$@" <<'PY'
import csv, os, sys

def size(s):
    s = s.strip()
    if s.lower().startswith("0x"):
        return int(s, 16)
    mul = {"K": 1024, "M": 1024 * 1024}.get(s[-1:].upper(), 1)
    return int(s[:-1] if mul > 1 else s) * mul

rows = [r for r in csv.reader(open("partitions.csv")) if r and not r[0].lstrip().startswith("#")]
slot = min(size(r[4]) for r in rows if r[1].strip() == "app")
fails = 0
for d in sys.argv[1:]:
    img = os.path.join(d, "zhac-wired-core.bin")
    n = os.path.getsize(img)
    pct = 100.0 * n / slot
    ok = pct <= 90.0
    print(f"{'OK' if ok else 'FAIL'}: {img} is {n:,} B, {pct:.1f} % of the {slot // 1024} KB app slot (limit 90 %)",
          file=sys.stdout if ok else sys.stderr)
    fails += not ok
sys.exit(fails)
PY
