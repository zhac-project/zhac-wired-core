// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// mono_bridge — local in-process replacement for the dual-chip SPI
// HAP link. Implementation lives in mono_bridge.cpp; the hap_master
// and hap_slave shim components delegate here.
//
// Phase 1 surface — all routing is stubbed (logged + dropped). Phase 2
// will replace the dispatch bodies with direct calls into the local
// services (zhac_adapter, simple_rules, device_shadow, ws_server,
// mqtt_gw, lua_engine, …).
#pragma once
#include "hap_protocol.h"
#include <functional>

using HapFrameCallback = std::function<void(const HapFrame&)>;

// Lifecycle — called from the hap_master / hap_slave shim init.
void mono_bridge_init_master();
void mono_bridge_init_slave();

// Callback registration. The two callbacks correspond to the
// dual-chip producer endpoints:
//   - master callback fired when an in-process notification needs to
//     reach the S3-side handlers (api / ws / mqtt forwarder);
//   - slave  callback fired when an in-process request needs to reach
//     the P4-side handlers (zigbee / lua / rules dispatch).
void mono_bridge_set_master_callback(HapFrameCallback cb);
void mono_bridge_set_slave_callback(HapFrameCallback cb);

// Outbound = "master → slave": something that used to travel over SPI
// from S3 to P4 (e.g. ZIGBEE_SET, RULE_UPDATE_DSL, SCRIPT_RUN). The
// bridge forwards the frame to the registered slave callback so the
// existing P4-side dispatch (now linked into this mono binary) sees
// it exactly as before.
void mono_bridge_dispatch_outbound(const HapFrame& frame);

// Inbound = "slave → master": something that used to travel over SPI
// from P4 to S3 (e.g. ATTR_UPDATE, DEVICE_JOIN, LOG_EVENT). The
// bridge forwards the frame to the registered master callback so the
// existing S3-side dispatch (WS broadcast, MQTT publish) sees it.
void mono_bridge_dispatch_inbound(const HapFrame& frame);
