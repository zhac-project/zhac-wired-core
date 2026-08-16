// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// esp_zb_interview -- Phase 2 join handling for the esp-zigbee backend.
//
// Internal seam between esp_zigbee_backend.cpp (stack lifecycle, APS ingress)
// and the interview engine. Not a public component header: nothing outside
// this component should call these.
#pragma once

#include <cstddef>
#include <cstdint>

// Create the join queue and the interview task. Call once, after the pool and
// zap_store are up and the coordinator has a PAN.
void zb_interview_init();

// Queue a freshly-announced device. Safe to call from the stack's signal
// context -- it only posts to a queue. Duplicate announces for a device
// already queued or in progress are dropped by the task, not here.
void zb_interview_enqueue(uint64_t ieee, uint16_t nwk);

// Re-run the interview for a device already in the pool (REST
// /api/device/reinterview). Returns false if the device is unknown or the
// queue is full.
bool zb_interview_trigger(uint64_t ieee);

// Handle a device announcement. Decides between the rejoin fast path (device
// already fully interviewed -- just refresh its address) and a full interview.
// Routers announce on every power cycle, so this is the difference between a
// mains outage costing one pool update per device and costing a full 4-stage
// ZDO interview per device, serialised on one radio.
void zb_interview_on_announce(uint64_t ieee, uint16_t nwk);

// Soft-remove: the device told us it is leaving (or was told to). Keeps the
// record so friendly name, interview state and shadow survive a rejoin.
void zb_interview_on_leave(uint64_t ieee);

// Hard-remove: user-initiated delete. Drops the pool entry and the adapter's
// cached definition. Does NOT send the ZDO leave -- the caller does that.
bool zb_interview_forget(uint64_t ieee);

// Feed every inbound ZCL frame to the interview engine so it can catch the
// Basic-cluster Read Attributes Response it is waiting for.
//
// Returns true only when the frame was consumed as an interview response. The
// caller still forwards everything to the adapter regardless -- a device may
// report Basic attributes unprompted, and ZHAC observes rather than swallows.
bool zb_interview_feed_zcl(uint16_t nwk, uint16_t cluster_id, uint8_t src_ep,
                           const uint8_t* zcl, uint8_t zcl_len);

// Register the configure transports (bind / report / cmd / read / write) with
// zhc_adapter. Call once at init, before any interview can complete.
void esp_zb_configure_register();

// ZDO bind/unbind with a caller-chosen destination, for the SPA's Bind tab.
// cfg_bind() above always targets the coordinator; this does not.
bool esp_zb_zdo_bind(uint16_t src_nwk, uint64_t src_ieee, uint8_t src_ep,
                     uint16_t cluster, uint64_t dst_ieee, uint8_t dst_ep,
                     bool unbind);

// Defined in esp_zigbee_backend.cpp -- raw ZCL out over APS. Declared here so
// the interview engine can send its Basic-cluster reads without duplicating
// the apsde request plumbing.
bool esp_zb_af_send(uint16_t nwk_addr, uint8_t dst_ep, uint16_t cluster_id,
                    const uint8_t* zcl_data, size_t zcl_len);
