# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# zigbee_pool -- the protocol-neutral half of the sibling `zigbee_mgr`
# component, compiled without any of its ZNP-bound half.
#
# WHY THIS EXISTS
# ---------------
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

if(NOT EXISTS "${ZM_DIR}/zigbee_pool.cpp")
    message(FATAL_ERROR
        "zigbee_pool: cannot find the sibling zigbee_mgr sources at ${ZM_DIR}. "
        "zhac-wired-core expects the ZHAC repos checked out side by side.")
endif()

idf_component_register(
    SRCS
        "${ZM_DIR}/zigbee_pool.cpp"
        "${ZM_DIR}/zigbee_diagnostics.cpp"
    INCLUDE_DIRS
        "${ZM_DIR}/include"
    REQUIRES
        freertos
        esp_timer
        zap_common
        zap_store
        zhc_adapter
)
