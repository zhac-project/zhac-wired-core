#!/bin/sh
# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# zhac-wired-core ships without RainMaker, without WiFi, and without the
# legacy radio backends. Those are decisions, not accidents, so they are
# enforced here instead of living as comments nobody reads.
#
# Matching is on USAGE (includes, symbol calls, CONFIG_ symbols), not on the
# bare words -- otherwise every comment explaining why something is absent
# would trip the guard it is explaining. tools/ is excluded so this file's own
# patterns do not match themselves.
#
# Run from the repo root:  ./tools/check_sku_invariants.sh
set -e

SCAN="main components CMakeLists.txt sdkconfig.defaults"
fail=0

# Comment lines are dropped before matching. Without this the guard trips on
# the comments that document each absence -- "// Mono continues here with
# znp_driver_init() ..." is precisely the sort of note worth keeping.
#
#   `//` anywhere at the start of the content  -> C/C++ comment, drop
#   `#` followed by whitespace                 -> sh/CMake/Kconfig comment, drop
#   `#include`                                 -> no space after #, KEPT
#
# Trailing and block comments are not handled; this guard is aimed at real
# usage, not at being a parser.
strip_comments() {
    grep -vE '^[^:]+:[0-9]+:[[:space:]]*(//|#[[:space:]])'
}

check() {
    label="$1"
    pattern="$2"
    hint="$3"
    if hits=$(grep -rnE "$pattern" $SCAN 2>/dev/null | strip_comments) && [ -n "$hits" ]; then
        echo "FAIL: $label" >&2
        echo "$hits" >&2
        echo "  -> $hint" >&2
        echo >&2
        fail=1
    fi
}

check "RainMaker reference" \
      '#include .*rainmaker|esp_rmaker_[a-z_]+|CONFIG_ZHAC_RAINMAKER' \
      "This SKU ships without RainMaker; provisioning assumes WiFi/BLE, and it has neither."

check "WiFi usage" \
      '#include "esp_wifi\.h"|esp_wifi_[a-z_]+\(|WIFI_MODE_|wifi_config_t' \
      "Ethernet-only build. Network state comes from eth.h / NetStatus."

# Note the (^|[^_[:alnum:]]) prefix on zigbee_backend_register. Without it the
# pattern also matches THIS SKU's own esp_zigbee_backend_register() -- the
# legacy P4 component is `zigbee_backend`, ours is `esp_zigbee_backend`, and one
# name is a suffix of the other. The guard failed the moment Phase 1 landed.
check "legacy radio backend" \
      'znp_driver_init\(|zigbee_mgr_init\(|(^|[^_[:alnum:]])zigbee_backend_register\(|ezsp_[a-z]+_init\(|CONFIG_ZHAC_NCP_' \
      "The radio here is esp-zigbee (native 802.15.4 on S31, ot_rcp on P4) via components/esp_zigbee_backend. ZNP/EZSP belong to the flagship line. The protocol-neutral device pool comes from the local components/zigbee_mgr override."

# Source greps are necessary but NOT sufficient: a component can be dragged
# into the firmware entirely through SIBLING requires, with nothing in our tree
# mentioning it. That is exactly what happened -- main -> lua_engine +
# simple_rules -> zigbee_mgr -> znp_driver quietly compiled the TI ZNP driver
# into every build while this script reported "no legacy radio backend".
#
# So when a configured build tree exists, assert the actual build GRAPH.
# Build dir is an argument because this repo builds TWO targets from one tree
# (build/ = esp32p4, build.s31/ = esp32s31) and the graph differs between them:
# the P4 build pulls in ieee802154/openthread-over-spinel, the S31 build the
# native radio. Checking only one target's graph would miss a regression in the
# other, so CI runs this once per build dir.
GRAPH="${1:-build}/project_description.json"
if [ -f "$GRAPH" ]; then
    for comp in znp_driver ezsp_driver zigbee_backend; do
        if python3 -c "import json,sys; sys.exit(0 if '$comp' in json.load(open('$GRAPH'))['build_components'] else 1)" 2>/dev/null; then
            echo "FAIL: '$comp' is in the build graph" >&2
            echo "  -> Something requires it transitively. Find the path with:" >&2
            echo "     python3 -c \"import json;i=json.load(open('$GRAPH'))['build_component_info'];print([n for n,v in i.items() if '$comp' in (v.get('reqs') or [])+(v.get('priv_reqs') or [])])\"" >&2
            echo "     The fix is usually a local components/<name>/ override, as with zigbee_mgr." >&2
            fail=1
        fi
    done
else
    echo "note: no build/ tree -- graph check skipped (source checks still ran)." >&2
fi

if [ "$fail" -eq 0 ]; then
    echo "OK: no RainMaker, no WiFi, no legacy radio backend (sources + build graph)."
fi
exit "$fail"
