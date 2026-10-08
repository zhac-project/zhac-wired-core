#!/bin/sh
# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Host check of the wired hub's rule ops: escaped error replies, paged rule.list, rule.create's id, rule.update's
# base fingerprint, rule.check, and the rule.added / rule.updated pushes (spec 2026-10-05 §3.3, §3.4, §4.5).
#   sh run.sh                  build and run; exit code = number of mismatches (ASan/UBSan abort = non-zero too)
#   MUTANT=<name> sh run.sh    same, after breaking ONE line in the work copy (never in the repo), to show the
#                              harness can fail: splice_err no_escape_row no_escape_check no_sort cursor_lt
#                              no_budget no_base no_clamp
#   WS_SRC=<file> sh run.sh    the same checks against another ws_bridge.cpp (a RED run: `git show <rev>:main/ws_bridge.cpp`)
# The code under test is cut verbatim out of main/ws_bridge.cpp by sed (send_err, sha256_hex, reply_ok_or_err,
# kRuleRowCap, rule_row_json, cmd_rule_list/create/update/check, on_rule_changed); JsonWriter, kRemotePageBudget,
# utf8_seq.h, ArduinoJson, event_bus.h and RuleSlot are the real headers. Only the neighbours are stubs (stubs.h,
# harness.cpp): allocator, reply sink, pushes, rule_store, simple_rules, PSA (OpenSSL underneath). Expected
# fingerprints come from sha256sum. Needs the sibling checkouts (zhac-components, zhac-net-core) next to this repo,
# g++, libcrypto, and PATH=/usr/bin first (set below).
set -e
H=$(cd "$(dirname "$0")" && pwd)
Z=$(cd "$H/../../../.." && pwd)
R=$Z/zhac-wired-core/main
C=$Z/zhac-components/components
AJ=$C/arduinojson/src
RC=$Z/zhac-net-core/components/remote_client/include
WS=$Z/zhac-net-core/components/ws_server/include
W=$(mktemp -d "${TMPDIR:-/tmp}/wired-host-rules.XXXXXX")
trap 'rm -rf "$W"' EXIT
cp "$H/stubs.h" "$H/slice_tu.cpp" "$H/harness.cpp" "$W/"

# the real RuleSlot
sed -n '/^struct __attribute__((packed)) RuleSlot {/,/^static_assert(sizeof(RuleSlot) == 536);/p' \
    "$C/zap_common/include/zap_common.h" > "$W/rule_slot.inc"
test "$(wc -l < "$W/rule_slot.inc")" -gt 10

# the real rule ops, one function at a time (single-line signatures, closing brace in column 0)
WSB=${WS_SRC:-$R/ws_bridge.cpp}
grep '^static constexpr size_t kRuleRowCap = ' "$WSB" > "$W/ws_slice.inc" || true   # older files keep it inside cmd_rule_list
for f in send_err sha256_hex reply_ok_or_err rule_row_json cmd_rule_list cmd_rule_create cmd_rule_update cmd_rule_check \
         on_rule_changed; do
    sed -n "/^static [a-z_0-9]* $f(.*{\$/,/^}/p" "$WSB" > "$W/$f.part"
    test "$(wc -l < "$W/$f.part")" -gt 4          # the slice found the whole function
    test "$(grep -c '^}' "$W/$f.part")" -eq 1     # and only that function
    cat "$W/$f.part" >> "$W/ws_slice.inc"
    echo >> "$W/ws_slice.inc"
    rm "$W/$f.part"
done

# expected fingerprints, from sha256sum, for texts the harness stores
T_CYR='ON лампа#state=1 DO log "привет" ENDON'
T_PLAIN='ON a#b=1 DO log c ENDON'
esc() { printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g'; }
{
    printf '#define TXT_CYR "%s"\n'   "$(esc "$T_CYR")"
    printf '#define FP_CYR "%s"\n'    "$(printf '%s' "$T_CYR" | sha256sum | cut -d' ' -f1)"
    printf '#define TXT_PLAIN "%s"\n' "$(esc "$T_PLAIN")"
    printf '#define FP_PLAIN "%s"\n'  "$(printf '%s' "$T_PLAIN" | sha256sum | cut -d' ' -f1)"
} > "$W/expect.h"

mutate() {   # mutate <file> <sed expression>: fails unless the file really changed
    cp "$W/$1" "$W/$1.orig"
    sed -i "$2" "$W/$1"
    if cmp -s "$W/$1" "$W/$1.orig"; then echo "mutant did not apply" >&2; exit 99; fi
    rm "$W/$1.orig"
}
case "${MUTANT:-}" in
    "") ;;
    splice_err)       mutate ws_slice.inc 's/\.str(err ? err : "", 160)/.raw(err ? err : "")/' ;;
    no_escape_row)    mutate ws_slice.inc 's/\.str(s\.name, sizeof(s\.name))/.raw(s.name)/' ;;
    no_escape_check)  mutate ws_slice.inc 's/\.str(err, sizeof(err))/.raw(err)/' ;;
    no_sort)          mutate ws_slice.inc 's/^\( *\)std::sort(.*$/\1;/' ;;
    cursor_lt)        mutate ws_slice.inc 's/slots\[i\]\.rule_id <= after/slots[i].rule_id < after/' ;;
    no_budget)        mutate ws_slice.inc 's/used + rn + 1 > kRemotePageBudget/used + rn + 1 > 1000000/' ;;
    no_base)          mutate ws_slice.inc 's/std::strcmp(fp, base) == 0;/true;/' ;;
    no_clamp)         mutate ws_slice.inc 's/cur->src_len <= sizeof(cur->src) ? cur->src_len : sizeof(cur->src)/cur->src_len/' ;;
    *) echo "unknown MUTANT" >&2; exit 98 ;;
esac

cd "$W"
export PATH=/usr/bin:$PATH   # the ESP32-ULP `as` on PATH poisons g++
# -Wno-maybe-uninitialized: GCC 15 false positive in ArduinoJson 7.4.3 (CollectionIterator default constructor)
FLAGS="-std=gnu++23 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -Wall -Wextra -Wno-maybe-uninitialized -I. -isystem $AJ -I$R -I$RC -I$WS -I$C/event_bus/include -I$C/zap_common/include"
g++ $FLAGS -c slice_tu.cpp -o slice_tu.o
g++ $FLAGS -c harness.cpp -o harness.o
g++ $FLAGS slice_tu.o harness.o -lcrypto -o harness
set +e
./harness
rc=$?
exit $rc
