# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Mono shim — overrides ../../zhac-net-core/components/hap_master.
# Same public header (include/hap_master.h, byte-for-byte) so any
# consumer compiled against the sibling API links cleanly here.
# Implementation routes through mono_bridge instead of SPI master.
idf_component_register(
    SRCS "hap_master.cpp"
    INCLUDE_DIRS "include"
    REQUIRES hap_protocol mono_bridge
)
