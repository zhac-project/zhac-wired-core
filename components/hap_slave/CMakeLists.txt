# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Mono shim — overrides ../../zhac-main-core/components/hap_slave.
idf_component_register(
    SRCS "hap_slave.cpp"
    INCLUDE_DIRS "include"
    REQUIRES hap_protocol mono_bridge
)
