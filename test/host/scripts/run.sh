#!/bin/sh
# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Host check of the wired hub's script ops: script.list (mtime), script.read (whole and paged), script.part,
# script.write (src and staged), script.check, script.reload, script.error, the saved-script event, and the REST
# handlers of main/api_scripts.cpp (spec 2026-10-05 §3.3-§3.8).
#   sh run.sh                  build and run; exit code = number of mismatches (ASan/UBSan/LSan abort = non-zero too)
#   ONLY_REST=1 REST_SRC=<old api_scripts.cpp> sh run.sh   only the REST checks, against an older api_scripts.cpp
#                              (a RED run: `git show <rev>:main/api_scripts.cpp`)
#   MUTANT=<name> sh run.sh    same, after breaking ONE line in the work copy (never in the repo), to show the
#                              harness can fail: no_exists no_changed consume_early no_size no_fp name_untrusted
#                              msg_unescaped read_cap read_fixed_buf page_budget mtime_always reload_wait
#                              drop_any no_check no_name_check no_too_many rest_no_413 rest_cap rest_total
#                              rest_break rest_zero_ok rest_408_as_400 rest_no_free rest_chunked
# The code under test is cut verbatim out of main/ws_bridge.cpp by sed (send_err, sha256_hex, reply_ok_or_err,
# the whole script.* section, ws_push_script_saved, on_script_error) and copied from main/api_scripts.cpp, so it
# tests the committed (or working-tree) text, not a copy. Real: JsonWriter, utf8_seq.h, remote_xfer.cpp (the
# staging slot and the page cutter), remote_allow.cpp (remote_script_name_ok), lua_engine_err.h, ArduinoJson, and
# lua_engine_check_syntax on real Lua 5.5. Stubs: allocator, clock, reply sink, push, the script store, Lua
# reload/run, PSA (OpenSSL underneath), esp_http_server (fake/). Expected fingerprints come from sha256sum or are
# fixed digests. Needs the sibling checkouts (zhac-components, zhac-net-core, zhac-main-core with its
# managed_components/georgik__lua, which an IDF configure of zhac-main-core downloads) next to this repo, g++,
# libcrypto, and PATH=/usr/bin first (set below). Builds in a temporary dir (deleted on exit); LUA_OBJ_CACHE=<dir>
# keeps the compiled Lua between runs (and is NOT deleted).
set -e
H=$(cd "$(dirname "$0")" && pwd)
Z=$(cd "$H/../../../.." && pwd)
R=$Z/zhac-wired-core/main
AJ=$Z/zhac-components/components/arduinojson/src
RCD=$Z/zhac-net-core/components/remote_client
RC=$RCD/include
WS=$Z/zhac-net-core/components/ws_server/include
ENG=$Z/zhac-main-core/components/lua_engine
LUA=$Z/zhac-main-core/managed_components/georgik__lua
W=$(mktemp -d "${TMPDIR:-/tmp}/wired-host-scripts.XXXXXX")
LUAOBJ=${LUA_OBJ_CACHE:-$W/luaobj}
export PATH=/usr/bin:$PATH   # the ESP32-ULP `as` on PATH poisons g++
trap 'rm -rf "$W"' EXIT
cp "$H/stubs.h" "$H/slice_tu.cpp" "$H/rest_tu.cpp" "$H/rest.h" "$H/harness.cpp" "$H/lua_check.c" "$W/"
cp -r "$H/fake" "$W/fake"

# ---- the real code under test --------------------------------------------------------------------------------
# one function at a time (single-line signatures, closing brace in column 0)
: > "$W/ws_slice.inc"
for sig in '^static void send_err(' '^static bool sha256_hex(' '^static void reply_ok_or_err(.*{$'; do
    sed -n "/$sig/,/^}/p" "$R/ws_bridge.cpp" > "$W/fn.part"
    test "$(wc -l < "$W/fn.part")" -gt 4          # the slice found the whole function
    test "$(grep -c '^}' "$W/fn.part")" -eq 1     # and only that function
    cat "$W/fn.part" >> "$W/ws_slice.inc"; echo >> "$W/ws_slice.inc"
done
# the whole script.* section: from its banner up to, not including, the alerts.get banner
sed -n '/^\/\/ .* script\.\* (the hub page and the cloud relay/,/^\/\/ .* alerts\.get/p' "$R/ws_bridge.cpp" | sed '$d' >> "$W/ws_slice.inc"
test "$(grep -c '^static void cmd_script_' "$W/ws_slice.inc")" -eq 8     # list read delete run part write check reload
for sig in '^void ws_push_script_saved(' '^static void on_script_error('; do
    sed -n "/$sig/,/^}/p" "$R/ws_bridge.cpp" > "$W/fn.part"
    test "$(wc -l < "$W/fn.part")" -gt 4
    test "$(grep -c '^}' "$W/fn.part")" -eq 1
    cat "$W/fn.part" >> "$W/ws_slice.inc"; echo >> "$W/ws_slice.inc"
done
rm "$W/fn.part"
grep -q 'staged_take' "$W/ws_slice.inc"
cp "${REST_SRC:-$R/api_scripts.cpp}" "$W/api_scripts_real.cpp"   # REST_SRC=<file>: run the REST checks against another version (the RED run)
sed -n '/^bool lua_engine_check_syntax(const char\* name, const char\* src,$/,/^}/p' "$ENG/src/lua_engine.c" > "$W/check_fn.inc"
test "$(wc -l < "$W/check_fn.inc")" -gt 30

# ---- test data and expected fingerprints (sha256sum) ----------------------------------------------------------------
python3 - "$W" <<'PY'
import sys
w = sys.argv[1]
# big_cyr.txt: exactly 16,384 bytes of valid Lua (comments): Cyrillic (2 bytes), euro (3), emoji (4), quotes, backslashes, tabs
line = '-- Привіт, світе! "quoted" \\back\\slash €ñ \U0001F600\tend\n'
out = b''
while len(out) + len(line.encode()) <= 16384 - 8:
    out += line.encode()
r = 16384 - len(out)
out += (b'-- ' + b'x' * (r - 4) + b'\n') if r >= 4 else b'\n' * r
assert len(out) == 16384 and out.endswith(b'\n')
out.decode('utf-8')
open(w + '/big_cyr.txt', 'wb').write(out)
# big_ctl.txt: 16,384 bytes, a long comment of control bytes: 6 encoded bytes each
ctl = b'--[==[' + b'\x01' * (16384 - 6 - 4) + b']==]'
assert len(ctl) == 16384
open(w + '/big_ctl.txt', 'wb').write(ctl)
PY
{
    printf '#define FP_BIG_CYR "%s"\n' "$(sha256sum "$W/big_cyr.txt" | cut -d' ' -f1)"
    printf '#define FP_BIG_CTL "%s"\n' "$(sha256sum "$W/big_ctl.txt" | cut -d' ' -f1)"
} > "$W/expect.h"

# ---- mutants: ONE line broken in the work copy --------------------------------------------------------------------
mutate() {   # mutate <file> <sed expression>: fails unless the file really changed
    cp "$W/$1" "$W/$1.orig"
    sed -i "$2" "$W/$1"
    if cmp -s "$W/$1" "$W/$1.orig"; then echo "mutant did not apply" >&2; exit 99; fi
    rm "$W/$1.orig"
}
S=ws_slice.inc
case "${MUTANT:-}" in
    "") ;;
    no_exists)       mutate $S 's/if (!base\[0\] && exists)/if (!base[0] \&\& false)/' ;;
    no_changed)      mutate $S 's/std::strcmp(fp, base) == 0;/true;/' ;;
    consume_early)   mutate $S 's/^        char fp\[65\] = "";$/        staged_drop(xfer); char fp[65] = "";/' ;;   # the staged text is used up before size and fp are checked
    no_size)         mutate $S 's/static_cast<long>(len) != (a\["size"\] | -1L) ||/false ||/' ;;
    no_fp)           mutate $S 's/std::strcmp(fp, a\["fp"\] | "") != 0) {/false) {/' ;;
    name_untrusted)  mutate $S 's/if (!remote_script_name_ok(name)) name = "";//' ;;
    msg_unescaped)   mutate $S 's/\.str(message, 120)/.raw(message)/' ;;
    read_cap)        mutate $S 's/const int sn = lua_script_cache_read(name, src, LUA_SCRIPT_SRC_MAX + 1);/const int sn = lua_script_cache_read(name, src, LUA_SCRIPT_SRC_MAX);/' ;;
    read_fixed_buf)  mutate $S 's/const size_t cap = measureJson(d) + 1;/const size_t cap = 20 * 1024;/' ;;
    page_budget)     mutate $S 's/kRemotePageBudget/(kRemotePageBudget * 2)/g' ;;
    mtime_always)    mutate $S 's/mtime >= 1600000000u/mtime >= 0u/g' ;;
    reload_wait)     mutate $S 's/lua_engine_reload(5000,/lua_engine_reload(50,/' ;;
    drop_any)        mutate $S 's/^    remote_xfer_use(xfer, now_ms(), true, \[\](const char\*, size_t, void\*) {}, nullptr);$/    remote_xfer_reset(); (void)xfer;/' ;;
    no_check)        mutate $S 's/if (!lua_engine_check_syntax(name, text, err, sizeof(err), &line)) {/if (false) {/' ;;
    no_name_check)   mutate $S 's/if (!remote_script_name_ok(name)) { send_err(fd, id, "bad_name"); return; }/if (false) {}/' ;;
    no_too_many)     mutate $S 's/>= LUA_SCRIPT_MAX) { send_err(fd, id, "too_many"); return false; }/>= 1000) { send_err(fd, id, "too_many"); return false; }/' ;;
    rest_no_413)     mutate api_scripts_real.cpp 's/if (req->content_len > kScriptCap) {/if (false) {/' ;;
    rest_cap)        mutate api_scripts_real.cpp 's/static constexpr size_t kScriptCap = LUA_SCRIPT_SRC_MAX;/static constexpr size_t kScriptCap = LUA_SCRIPT_SRC_MAX - 1;/' ;;
    rest_total)      mutate api_scripts_real.cpp 's/ws_push_script_saved(name, src, std::strlen(src), existed)/ws_push_script_saved(name, src, std::strlen(src) - 1, existed)/' ;;                # the event's size / fp are not those of the stored text
    rest_break)      mutate api_scripts_real.cpp 's/if (got <= 0) {/if (got <= 0) break; if (false) {/' ;;                # a failed receive ends the body (the old loop)
    rest_zero_ok)    mutate api_scripts_real.cpp 's/if (got <= 0) {/if (got == 0) break; if (got < 0) {/' ;;            # a client that closed mid-upload is taken as the end
    rest_408_as_400) mutate api_scripts_real.cpp 's/if (got == HTTPD_SOCK_ERR_TIMEOUT)/if (false)/' ;;
    rest_no_free)    mutate api_scripts_real.cpp 's/^            heap_caps_free(buf);$/            ;/' ;;                  # the body buffer leaks on a failed receive
    rest_chunked)    mutate api_scripts_real.cpp 's/httpd_req_get_hdr_value_len(req, "Transfer-Encoding") > 0/false/' ;;   # a chunked upload blanks the script
    *) echo "unknown MUTANT" >&2; exit 98 ;;
esac

# ---- real Lua 5.5 (the firmware's georgik__lua managed component), compiled for the host ---------------------------
if [ ! -f "$LUAOBJ/.done" ]; then
    mkdir -p "$LUAOBJ"
    for f in "$LUA"/lua/*.c; do
        case $(basename "$f") in lua.c|luac.c|onelua.c|ltests.c) ;; *) echo "$f";; esac
    done | nice -n 10 xargs -P 4 -I{} sh -c 'gcc -std=gnu99 -O1 -w -DLUA_USE_POSIX -DCONFIG_LUA_MAXSTACK=1000000 -I'"$LUA"'/include -I'"$LUA"'/lua -c {} -o '"$LUAOBJ"'/$(basename {} .c).o'
    touch "$LUAOBJ/.done"
fi

cd "$W"
# -Wno-maybe-uninitialized: GCC 15 false positive in ArduinoJson 7.4.3 (CollectionIterator default constructor)
FLAGS="-std=gnu++23 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -Wall -Wextra -Wno-maybe-uninitialized -pthread -I. -I$W/fake -isystem $AJ -I$R -I$RC -I$WS -I$ENG/include"
g++ $FLAGS -c slice_tu.cpp -o slice_tu.o
g++ $FLAGS -c rest_tu.cpp -o rest_tu.o
g++ $FLAGS -c harness.cpp -o harness.o
g++ $FLAGS -c "$RCD/remote_xfer.cpp" -o remote_xfer.o
g++ $FLAGS -c "$RCD/remote_allow.cpp" -o remote_allow.o
gcc -std=gnu99 -O1 -g -Wall -Wextra -DCONFIG_LUA_MAXSTACK=1000000 -I"$LUA/include" -I"$LUA/lua" -c lua_check.c -o lua_check.o
g++ $FLAGS slice_tu.o rest_tu.o harness.o remote_xfer.o remote_allow.o lua_check.o "$LUAOBJ"/*.o -lm -lcrypto -o harness
set +e
timeout 300 ./harness
rc=$?
exit $rc
