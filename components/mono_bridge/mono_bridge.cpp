// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// mono_bridge — compile-shim for the dual-chip HAP symbols.
//
// In dual-chip ZHAC the HAP master (S3) and HAP slave (P4) talked over an
// SPI link. In the single-chip mono build there is no link: the gateway
// (`main/`) calls the local services directly (device_shadow, zhac_adapter,
// simple_rules, lua_engine, ws_server, mqtt_gw, …) — see PORT_PLAN.md. So
// the request/response and push paths do NOT route through here.
//
// This shim exists only so that any sibling component which still references
// the `hap_master_*` / `hap_slave_*` symbols links cleanly. The two rails
// below are inert relays kept for that compatibility (and the Phase-1 link
// probe in main.cpp); they log at debug and forward to a callback if one was
// registered. Nothing in the mono gateway depends on them.

#include "mono_bridge.h"
#include "esp_log.h"

static const char* TAG = "mono_bridge";

static HapFrameCallback s_master_cb;
static HapFrameCallback s_slave_cb;

void mono_bridge_init_master() {
    ESP_LOGI(TAG, "master endpoint ready (compile-shim; gateway uses direct calls)");
}

void mono_bridge_init_slave() {
    ESP_LOGI(TAG, "slave  endpoint ready (compile-shim; gateway uses direct calls)");
}

void mono_bridge_set_master_callback(HapFrameCallback cb) {
    s_master_cb = std::move(cb);
}

void mono_bridge_set_slave_callback(HapFrameCallback cb) {
    s_slave_cb = std::move(cb);
}

void mono_bridge_dispatch_outbound(const HapFrame& frame) {
    ESP_LOGD(TAG, "outbound type=%u seq=%u len=%u (shim)",
             (unsigned)frame.type, frame.seq, frame.payload_len);
    if (s_slave_cb) s_slave_cb(frame);
}

void mono_bridge_dispatch_inbound(const HapFrame& frame) {
    ESP_LOGD(TAG, "inbound  type=%u seq=%u len=%u (shim)",
             (unsigned)frame.type, frame.seq, frame.payload_len);
    if (s_master_cb) s_master_cb(frame);
}
