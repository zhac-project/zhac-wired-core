# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# zigbee_mgr -- a LOCAL OVERRIDE of the sibling component of the same name,
# compiling only its protocol-neutral half and dropping the ZNP-bound half.
#
# Being named `zigbee_mgr` is the whole point: ESP-IDF resolves components by
# first match, and a project's own components/ wins. So every existing
# `REQUIRES zigbee_mgr` in the tree -- sibling `simple_rules`, our local
# `lua_engine` -- silently resolves HERE instead of to the sibling, and
# znp_driver never enters the build graph at all. Same trick mono_bridge uses
# to displace hap_master / hap_slave.
#
# This replaced an earlier `zigbee_pool`-named component, which did not work:
# under that name the sibling zigbee_mgr was still reachable via
# simple_rules/lua_engine, so znp_driver was compiled into the firmware
# regardless. It happened to build on esp32p4 and nobody noticed until esp32s31
# rejected it (no GPIO_NUM_29; IDF v6.1 added a uart_config_t field the
# driver's initialiser omits). tools/check_sku_invariants.sh now asserts the
# build GRAPH, not just our sources, so that cannot regress silently again.
#
# WHY ONLY HALF
# -------------
# `zigbee_mgr` in zhac-components is described elsewhere as a "backend-agnostic
# lifecycle" layer. It is not: its CMakeLists REQUIRES znp_driver, and
# zigbee_mgr.cpp / zigbee_interview.cpp / zcl_commands.cpp make ~41 direct
# znp_*() calls between them. Pulling the whole component in would drag the TI
# MT transport into a firmware whose radio is going to be esp-zigbee over
# ot_rcp (Phase 1) -- dead code that also invites someone to wire it up.
#
# The split is clean where it matters. zigbee_pool.cpp and
# zigbee_diagnostics.cpp contain ZERO znp_ references; between them they call
# only zap_store_is_ready() and zap_store_load_devices(). zigbee_mgr.h (which
# zigbee_pool.cpp includes) mentions znp only in a comment and itself includes
# just zap_common.h. So these two translation units compile and link against
# the sibling's include/ with no ZNP anything present.
#
# Result: api_devices.cpp gets a REAL device pool and api_system.cpp a REAL
# diagnostics ring, with no radio in the binary. In Phase 0 the pool is simply
# empty, which is the correct answer rather than a stub's fiction.
#
# The sibling include/ directory also exposes zigbee_mgr.h, zcl_commands.h and
# friends. Their declarations are visible but their definitions are absent, so
# any accidental use of the ZNP layer fails loudly at link time. That is a
# feature; do not "fix" it by adding znp_driver.
#
# See extra/docs/ESP_ZIGBEE_BACKEND_DESIGN.md (correction dated 2026-08-14) --
# splitting zigbee_mgr upstream is added Phase-1 scope.
set(ZM_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../zhac-components/components/zigbee_mgr")

# radio_stubs.cpp is ALWAYS compiled; its contents are gated inside the file
# with #if !CONFIG_ZHAC_ESP_ZIGBEE.
#
# It must not be gated here: ESP-IDF's early-expansion pass processes this file
# with CONFIG_* undefined, so an `if(CONFIG_...)` around SRCS resolves against
# an empty value and silently picks the wrong branch. Source-level #if sees the
# real symbol. (Gating on IDF_TARGET here would be fine -- that is available.)
set(RADIO_STUBS_SRC "radio_stubs.cpp")

if(NOT EXISTS "${ZM_DIR}/zigbee_pool.cpp")
    message(FATAL_ERROR
        "zigbee_pool: cannot find the sibling zigbee_mgr sources at ${ZM_DIR}. "
        "zhac-wired-core expects the ZHAC repos checked out side by side.")
endif()

idf_component_register(
    SRCS
        "${ZM_DIR}/zigbee_pool.cpp"
        "${ZM_DIR}/zigbee_diagnostics.cpp"
        # ZCL Basic-attribute parsing + endpoint probe ordering. Verified
        # znp-free (0 znp_ references), so the esp-zigbee interview reuses the
        # exact parser the ZNP path uses rather than reimplementing it.
        "${ZM_DIR}/zigbee_interview_utils.cpp"
        # Shared ZCL transaction-sequence allocator (znp-free). Reused so the
        # interview and configure paths draw TSNs from one counter.
        "${ZM_DIR}/zcl_seq.cpp"
        # THE decode->shadow sink. zhac_adapter_try_decode() hands every decoded
        # key to whatever zhac_adapter_register_shadow() installed, and this file
        # is the only thing in the tree that installs one. Without it a device
        # decodes perfectly, logs "matched, N keys", and the values go nowhere --
        # device_shadow stays empty and the SPA shows no states. znp-free.
        "${ZM_DIR}/zhc_shadow_bridge.cpp"
        # Radio operations the control surface calls. esp_zigbee_backend
        # defines the SAME six symbols, so exactly one of the two may be
        # compiled -- linking both is a duplicate-symbol error.
        ${RADIO_STUBS_SRC}
    INCLUDE_DIRS
        "${ZM_DIR}/include"
    # Mirrors the sibling's public REQUIRES exactly, MINUS znp_driver. Being a
    # drop-in means matching its public interface, not just its symbols: IDF
    # propagates REQUIRES transitively for include paths, so anything that got
    # device_shadow.h or event_bus.h "for free" by requiring zigbee_mgr must
    # still get them. Trimming this list to only what the two compiled files
    # need broke sibling simple_rules with a missing device_shadow.h.
    REQUIRES
        freertos
        esp_timer
        zap_common
        zap_store
        event_bus
        device_shadow
        zhc_adapter
)
