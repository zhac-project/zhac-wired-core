// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Mono shim implementation of hap_master. In dual-chip ZHAC this
// component owned an SPI master peripheral and pushed binary HAP
// frames to the P4 slave; here, every outbound frame is a request to
// a local in-process subsystem on this same S3 chip, so we hand it to
// mono_bridge for synchronous dispatch.
//
// Phase 1: send routes through mono_bridge::dispatch_outbound which
// at this phase only logs the frame type — wiring to real services
// (zhac_adapter, simple_rules, device_shadow, …) lands in Phase 2.
#include "hap_master.h"
#include "mono_bridge.h"
#include "esp_log.h"

static const char* TAG = "hap_master(mono)";

// Inbound callback — in dual-chip this is invoked by the SPI ISR when
// a frame arrives from P4. In mono there is no inbound channel from
// a peer chip; instead mono_bridge synthesises async "replies" or
// device-update notifications when the underlying service produces
// state changes (e.g. a Zigbee attribute report). The callback gets
// invoked from those code paths.
static HapFrameCallback s_cb;

void hap_master_init() {
    ESP_LOGI(TAG, "init (stub) — SPI master replaced by in-process bridge");
    mono_bridge_init_master();
}

void hap_master_set_task_handle(TaskHandle_t /*h*/) {
    // In dual-chip, this was the task to notify on inbound DMA. The
    // bridge doesn't need it (callback fires directly on the producer
    // task). Kept as a no-op so existing call sites compile.
}

void hap_master_send(const HapFrame& frame) {
    mono_bridge_dispatch_outbound(frame);
}

void hap_master_recv() {
    // Dual-chip: pull a frame out of the SPI RX ring. Mono: nothing to
    // pull — inbound is callback-driven. No-op.
}

void hap_master_set_callback(HapFrameCallback cb) {
    s_cb = std::move(cb);
    mono_bridge_set_master_callback(s_cb);
}
