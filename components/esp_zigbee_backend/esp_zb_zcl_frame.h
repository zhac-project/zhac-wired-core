// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// esp_zb_zcl_frame -- pure ZCL frame builders, no ESP dependencies.
//
// Split out of esp_zb_configure.cpp for one reason: these are byte-exact wire
// formats and they are the highest-risk code in the backend. A reportable-
// change field that is one byte too wide or a manufacturer code written in the
// wrong position does not fail loudly -- it shifts every following byte, and
// the device simply rejects the record or, worse, accepts a garbage interval.
// On-air testing needs hardware; these functions do not. See test/host/.
//
// Nothing here allocates, logs, or touches the radio. Callers supply the
// buffer and the transaction sequence number.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace zhc_zcl {

// Reportable-change field width by ZCL attribute data type.
//   0    = discrete type; the field is OMITTED entirely
//   1..4 = analog, width matches the type
//   0xFF = unsupported -> caller must fail rather than truncate
//
// Table matches zhac-components' ZNP path (zcl_commands.cpp) so both radios
// put identical bytes on air -- a precondition for the design doc's §6.6
// parity requirement.
inline uint8_t change_width(uint8_t t) {
    switch (t) {
        case 0x10:                                      // Bool
        case 0x18: case 0x19: case 0x1A: case 0x1B:
        case 0x1C: case 0x1D: case 0x1E: case 0x1F:     // bitmap8..bitmap64
        case 0x30: case 0x31:                           // ENUM8 / ENUM16
            return 0;
        case 0x20: case 0x28: return 1;                 // u8, s8
        case 0x21: case 0x29: case 0x38: return 2;      // u16, s16, semi-float
        case 0x22: case 0x2A: return 3;                 // u24, s24
        case 0x23: case 0x2B: case 0x39: return 4;      // u32, s32, float32
        default: return 0xFF;
    }
}

// ZCL frame header:
//
//   FC | [manu_lo manu_hi] | TSN | CMD
//
// FC bits: 0 = cluster-specific, 2 = manufacturer-specific,
//          3 = direction (0 = client->server), 4 = disable default response.
//
// The manufacturer code sits BETWEEN the frame control and the TSN, not after
// the command -- getting that wrong is the classic way to make Aqara 0xFCC0
// writes fail in a way that looks like a device problem.
inline size_t hdr(uint8_t* buf, uint16_t manu, uint8_t tsn, uint8_t cmd,
                  bool cluster_specific, bool disable_default_rsp) {
    uint8_t fc = 0;
    if (cluster_specific)    fc |= 0x01;
    if (manu)                fc |= 0x04;
    if (disable_default_rsp) fc |= 0x10;

    size_t p = 0;
    buf[p++] = fc;
    if (manu) {
        buf[p++] = static_cast<uint8_t>(manu & 0xFF);
        buf[p++] = static_cast<uint8_t>((manu >> 8) & 0xFF);
    }
    buf[p++] = tsn;
    buf[p++] = cmd;
    return p;
}

// Configure Reporting (cmd 0x06), one attribute record, ZCL §2.5.7:
//
//   header | direction(0) | attr_id LE | type | min LE | max LE | [change]
//
// direction 0 means "device sends reports TO us".
// Returns bytes written, or 0 on unsupported type / insufficient capacity.
inline size_t configure_report(uint8_t* buf, size_t cap, uint8_t tsn,
                               uint16_t attr_id, uint8_t attr_type,
                               uint16_t min_interval, uint16_t max_interval,
                               uint32_t reportable_change, uint16_t manu) {
    const uint8_t w = change_width(attr_type);
    if (w == 0xFF) return 0;
    const size_t need = (manu ? 5u : 3u) + 8u + w;
    if (cap < need) return 0;

    size_t p = hdr(buf, manu, tsn, 0x06, /*cluster_specific=*/false,
                   /*disable_default_rsp=*/false);
    buf[p++] = 0x00;
    buf[p++] = static_cast<uint8_t>(attr_id & 0xFF);
    buf[p++] = static_cast<uint8_t>((attr_id >> 8) & 0xFF);
    buf[p++] = attr_type;
    buf[p++] = static_cast<uint8_t>(min_interval & 0xFF);
    buf[p++] = static_cast<uint8_t>((min_interval >> 8) & 0xFF);
    buf[p++] = static_cast<uint8_t>(max_interval & 0xFF);
    buf[p++] = static_cast<uint8_t>((max_interval >> 8) & 0xFF);
    for (uint8_t i = 0; i < w; i++) {
        buf[p++] = static_cast<uint8_t>((reportable_change >> (8 * i)) & 0xFF);
    }
    return p;
}

// Read Attributes (cmd 0x00). `attr_ids_le` is already little-endian.
inline size_t read_attributes(uint8_t* buf, size_t cap, uint8_t tsn,
                              const uint8_t* attr_ids_le, uint8_t count,
                              uint16_t manu) {
    if (!attr_ids_le || count == 0) return 0;
    const size_t n = static_cast<size_t>(count) * 2;
    if (cap < (manu ? 5u : 3u) + n) return 0;
    size_t p = hdr(buf, manu, tsn, 0x00, false, false);
    std::memcpy(buf + p, attr_ids_le, n);
    return p + n;
}

// Write Attributes (cmd 0x02), single record:
//   header | attr_id LE | type | value
inline size_t write_attribute(uint8_t* buf, size_t cap, uint8_t tsn,
                              uint16_t attr_id, uint8_t attr_type,
                              const uint8_t* val, uint8_t len, uint16_t manu) {
    if (len && !val) return 0;
    if (cap < (manu ? 5u : 3u) + 3u + len) return 0;
    size_t p = hdr(buf, manu, tsn, 0x02, false, false);
    buf[p++] = static_cast<uint8_t>(attr_id & 0xFF);
    buf[p++] = static_cast<uint8_t>((attr_id >> 8) & 0xFF);
    buf[p++] = attr_type;
    if (len) std::memcpy(buf + p, val, len);
    return p + len;
}

// Cluster-specific command. `flags` bit0 = disable default response, matching
// the ZNP config-step encoding (kStepFlagDisableDefaultResponse).
inline size_t cluster_command(uint8_t* buf, size_t cap, uint8_t tsn,
                              uint8_t cmd_id, const uint8_t* payload,
                              uint8_t payload_len, uint8_t flags) {
    if (payload_len && !payload) return 0;
    if (cap < 3u + payload_len) return 0;
    size_t p = hdr(buf, /*manu=*/0, tsn, cmd_id, /*cluster_specific=*/true,
                   (flags & 0x01) != 0);
    if (payload_len) std::memcpy(buf + p, payload, payload_len);
    return p + payload_len;
}

}  // namespace zhc_zcl
