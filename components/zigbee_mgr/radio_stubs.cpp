// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// radio_stubs.cpp -- the radio-operation half of zigbee_mgr, absent in Phase 0.
//
// WHY THESE EXIST
// ---------------
// The ported ZHAC control surface genuinely calls into the radio layer for
// device operations: api_devices.cpp and ws_bridge.cpp want permit-join,
// interview, ZDO bind/unbind and factory recommission. In the sibling
// zigbee_mgr those are implemented against the TI ZNP transport, which this
// SKU does not have -- Phase 1 replaces the whole layer with esp_zigbee_backend
// (native 802.15.4 on S31, ot_rcp on P4).
//
// Without these, the firmware does not link at all. With them it links, boots,
// and answers those specific endpoints with an honest failure while every other
// part of the surface -- REST, WS, SPA, rules, Lua, MQTT, storage -- works.
//
// This is also the correction to an earlier claim. Phase 0 was described as
// having "no radio", but until the local zigbee_mgr override landed the build
// silently linked the sibling zigbee_mgr AND znp_driver by way of
// simple_rules/lua_engine. It only became true once those were cut out; these
// stubs are what makes it link honestly rather than by accident.
//
// PHASE 1: delete this file. esp_zigbee_backend supplies the real
// implementations, and the call sites above need no change.
#include "sdkconfig.h"

// Compiled away when the real backend is present -- it defines the same six
// symbols, so linking both would be a duplicate-symbol error. Gated HERE and
// not in CMake because CONFIG_* is undefined during IDF early expansion.
#if !CONFIG_ZHAC_ESP_ZIGBEE

#include "esp_log.h"
#include "zigbee_mgr.h"

static const char* TAG = "zigbee_mgr";

// Logged at WARN, not silently false: an operator poking "interview" in the UI
// and getting nothing deserves a line saying why.
static inline bool no_radio(const char* op) {
    ESP_LOGW(TAG, "%s: no radio backend in this build (Phase 0) -- request refused", op);
    return false;
}

bool zigbee_permit_join(uint8_t /*duration_s*/) { return no_radio("permit_join"); }

bool zigbee_zdo_bind(uint16_t /*src_nwk*/, uint64_t /*src_ieee*/, uint8_t /*src_ep*/,
                     uint16_t /*cluster*/, uint64_t /*dst_ieee*/, uint8_t /*dst_ep*/) {
    return no_radio("zdo_bind");
}

bool zigbee_zdo_unbind(uint16_t /*src_nwk*/, uint64_t /*src_ieee*/, uint8_t /*src_ep*/,
                       uint16_t /*cluster*/, uint64_t /*dst_ieee*/, uint8_t /*dst_ep*/) {
    return no_radio("zdo_unbind");
}

bool zigbee_interview_trigger(uint64_t /*ieee*/) { return no_radio("interview_trigger"); }

bool zigbee_force_recommission() { return no_radio("force_recommission"); }

// Zero rather than a fabricated address: callers format this as the
// coordinator's IEEE, and an invented value would look like a real device.
uint64_t zigbee_mgr_coordinator_ieee() { return 0; }

#endif  // !CONFIG_ZHAC_ESP_ZIGBEE
