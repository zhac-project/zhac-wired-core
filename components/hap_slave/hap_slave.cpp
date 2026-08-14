// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Mono shim implementation of hap_slave. In dual-chip ZHAC this
// component lived on P4 and answered SPI master frames coming from S3.
// In mono there is no peer, so:
//   - "send" (slave→master notification) becomes a direct call into
//     the S3-side surfaces (WS broadcast, MQTT publish, etc.) via
//     mono_bridge_dispatch_inbound;
//   - the registered callback is the existing master-frame handler
//     from hap_dispatch.cpp; mono_bridge invokes it directly when an
//     outbound master frame arrives from the local API/WS layer.
#include "hap_slave.h"
#include "mono_bridge.h"
#include "esp_log.h"

static const char* TAG = "hap_slave(mono)";

static HapFrameCallback s_cb;

void hap_slave_init() {
    ESP_LOGI(TAG, "init (stub) — SPI slave replaced by in-process bridge");
    mono_bridge_init_slave();
}

void hap_slave_send(const HapFrame& frame) {
    mono_bridge_dispatch_inbound(frame);
}

void hap_slave_set_callback(HapFrameCallback cb) {
    s_cb = std::move(cb);
    mono_bridge_set_slave_callback(s_cb);
}
