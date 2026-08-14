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

check "legacy radio backend" \
      'znp_driver_init\(|zigbee_mgr_init\(|zigbee_backend_register\(|ezsp_[a-z]+_init\(|CONFIG_ZHAC_NCP_' \
      "The radio here is esp-zigbee over ot_rcp (Phase 1). ZNP/EZSP belong to the flagship line. The protocol-neutral device pool comes from components/zigbee_pool."

if [ "$fail" -eq 0 ]; then
    echo "OK: no RainMaker, no WiFi, no legacy radio backend."
fi
exit "$fail"
