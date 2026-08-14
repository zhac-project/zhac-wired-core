# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# mono_bridge — central in-process dispatch for the dual-chip HAP
# frame contract. Substitutes for the SPI link by routing outbound
# (master→slave) frames straight to local services, and synthesising
# inbound (slave→master) callbacks from local subsystem events.
#
# Phase 1: stubs that log and return. Routing wires up in Phase 2.
idf_component_register(
    SRCS "mono_bridge.cpp"
    INCLUDE_DIRS "include"
    REQUIRES hap_protocol
)
