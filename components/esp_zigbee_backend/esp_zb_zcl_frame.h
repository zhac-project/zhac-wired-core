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
        case 0x24: case 0x2C: return 5;                 // u40, s40
        case 0x25: case 0x2D: return 6;                 // u48, s48 (seMetering energy)
        case 0x26: case 0x2E: return 7;                 // u56, s56
        case 0x27: case 0x2F: case 0x3A: return 8;      // u64, s64, double
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
        buf[p++] = static_cast<uint8_t>((static_cast<uint64_t>(reportable_change) >> (8 * i)) & 0xFF);
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

// An incoming frame's ZCL header (§2.4.1). Radio input: every ingress helper
// goes through this rather than indexing the frame itself, because the TSN and
// command id move by two bytes when the manufacturer-specific bit is set and a
// 3-byte frame with that bit has neither.
struct Header {
    uint8_t  fc;                // frame control
    bool     cluster_specific;  // frame type 01; else profile-wide
    bool     mfg;
    uint16_t mfg_code;
    uint8_t  tsn;
    uint8_t  cmd;
    uint8_t  hdr_len;           // 3, or 5 with a manufacturer code
};

inline bool parse_header(const uint8_t* zcl, size_t len, Header* out) {
    if (!zcl || len < 3) return false;
    const bool mfg = (zcl[0] & 0x04) != 0;
    const uint8_t n = mfg ? 5 : 3;
    if (len < n) return false;
    out->fc               = zcl[0];
    out->cluster_specific = (zcl[0] & 0x03) == 0x01;
    out->mfg              = mfg;
    out->mfg_code         = mfg ? static_cast<uint16_t>(zcl[1] | (zcl[2] << 8)) : 0;
    out->tsn              = zcl[n - 2];
    out->cmd              = zcl[n - 1];
    out->hdr_len          = n;
    return true;
}

// IAS Zone (0x0500) enrollment, the ZNP path's handshake (zigbee_mgr.cpp
// zcl_maybe_respond_ias_enroll): a sensor that is not enrolled sends Zone
// Enroll Request (cluster command 0x01) and stays silent until it gets a Zone
// Enroll Response; a few also want IAS_CIE_Address (0x0010) written first.
inline bool is_zone_enroll_request(uint16_t cluster, const uint8_t* zcl, size_t len) {
    Header h;
    return cluster == 0x0500 && parse_header(zcl, len, &h) && h.cluster_specific && h.cmd == 0x01;
}

// Zone Enroll Response (cmd 0x00): [code 0x00 = success, zone id].
inline size_t zone_enroll_response(uint8_t* buf, size_t cap, uint8_t tsn, uint8_t zone_id) {
    const uint8_t payload[] = {0x00, zone_id};
    return cluster_command(buf, cap, tsn, 0x00, payload, sizeof(payload), 0);
}

// Write IAS_CIE_Address (attr 0x0010, type 0xF0 IEEE address, little-endian).
inline size_t write_cie_address(uint8_t* buf, size_t cap, uint8_t tsn, uint64_t ieee) {
    uint8_t le[8];
    for (int i = 0; i < 8; i++) le[i] = static_cast<uint8_t>(ieee >> (i * 8));
    return write_attribute(buf, cap, tsn, 0x0010, 0xF0, le, sizeof(le), 0);
}

// Get Group Membership Response body (past the ZCL header): capacity(1) |
// count(1) | count group ids, LE. The whole advertised list must be there --
// a frame that claims more ids than it carries is malformed, not "in fewer
// groups", and must not replace what the hub knows (review F14). More ids
// than the caller can hold is fine: the first `max` are kept.
inline bool parse_group_membership(const uint8_t* body, size_t len,
                                   uint16_t* out, uint8_t max, uint8_t* out_count) {
    if (!body || len < 2) return false;
    const uint8_t count = body[1];
    if (len < 2u + 2u * count) return false;
    uint8_t w = 0;
    for (uint8_t i = 0; i < count && w < max; i++)
        out[w++] = static_cast<uint16_t>(body[2 + 2 * i] | (body[3 + 2 * i] << 8));
    *out_count = w;
    return true;
}

}  // namespace zhc_zcl
