// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Zigbee device firmware update (research 2026-11-18 C2): the OTA file check, the read of a device's Query Next Image
// and the rules of one update. Free of IDF so the host harness (test/host/device_ota) runs it as is; device_ota.cpp
// holds it under its lock and does the I/O.
#pragma once
#include <cstddef>
#include <cstdint>

namespace devota {

constexpr uint32_t kMagic = 0x0BEEF11E;     // OTA upgrade file identifier (ZCL 11.4.2)
constexpr uint16_t kHeaderVersion = 0x0100;
constexpr size_t kHeaderMin = 56;           // up to and including the total image size
constexpr size_t kMaxFile = 4u << 20;

inline uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }
inline uint32_t rd32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

struct Header {
    uint32_t start = 0;         // where the magic is in the uploaded file (some vendors put a prefix before it)
    uint32_t total = 0;         // total image size from the header: what the device gets, from the magic on
    uint32_t file_version = 0;
    uint16_t header_len = 0, manuf = 0, image_type = 0;
};

enum class FileErr : uint8_t { None, NotOta, TooLarge };

// The first magic in the file must start a whole image: header version 1.0, a header of 56 bytes or more, and the
// total image size within the file. Bytes after the image are not served.
inline FileErr parse(const uint8_t* b, size_t n, Header* h) {
    if (n > kMaxFile) return FileErr::TooLarge;
    for (size_t i = 0; i + kHeaderMin <= n; i++) {
        if (rd32(b + i) != kMagic) continue;
        const uint8_t* p = b + i;
        const uint16_t hlen = rd16(p + 6);
        const uint32_t total = rd32(p + 52);
        if (rd16(p + 4) != kHeaderVersion || hlen < kHeaderMin || total < hlen || total > n - i) return FileErr::NotOta;
        h->start = (uint32_t)i;
        h->total = total;
        h->header_len = hlen;
        h->manuf = rd16(p + 10);
        h->image_type = rd16(p + 12);
        h->file_version = rd32(p + 14);
        return FileErr::None;
    }
    return FileErr::NotOta;
}

struct Query { uint16_t manuf = 0, image_type = 0; uint32_t version = 0; };

// A ZCL frame on cluster 0x0019: true for a Query Next Image Request (cluster-specific, client to server, command 1:
// field control, manufacturer, image type, current version[, hardware version]).
inline bool parse_query(const uint8_t* z, size_t n, Query* q) {
    if (n < 3 + 9 || (z[0] & 0x0F) != 0x01 || z[2] != 0x01) return false;
    q->manuf = rd16(z + 4);
    q->image_type = rd16(z + 6);
    q->version = rd32(z + 8);
    return true;
}

enum class State : uint8_t { Idle, Waiting, Sending, Done, Failed, Cancelled };

// One update at a time: one image for one device. `reason` is a code the page puts in words.
struct Session {
    State state = State::Idle;
    uint64_t ieee = 0;
    Header h{};
    uint32_t offset = 0;        // the last block sent
    uint32_t since = 0;         // when the update began (seconds of uptime)
    bool has_query = false;     // `dev` holds the device's last Query Next Image
    Query dev{};
    uint8_t device_status = 0;  // the device's Upgrade End status when reason is "device_status"
    const char* reason = "";    // same_version, device_newer, device_status, or what fail() was given
};

inline bool active(const Session& s) { return s.state == State::Waiting || s.state == State::Sending; }

// Image blocks and the Upgrade End are answered for the target of a running update only; any other device that asks
// for blocks of this image without having been served (a resumed download) is turned away.
inline bool serves(const Session& s, uint64_t ieee) { return active(s) && ieee == s.ieee; }

inline bool begin(Session& s, uint64_t ieee, const Header& h, uint32_t now) {
    if (active(s)) return false;
    s = Session{};
    s.state = State::Waiting;
    s.ieee = ieee;
    s.h = h;
    s.since = now;
    return true;
}

// False once the device asked for another manufacturer or image type: the stack will never offer it this file.
inline bool fits(const Session& s) {
    return !s.has_query || (s.dev.manuf == s.h.manuf && s.dev.image_type == s.h.image_type);
}

// The device already runs the file's version (done), or a newer one (no downgrades: it keeps its firmware).
inline bool ends_on_version(Session& s) {
    if (s.dev.version < s.h.file_version) return false;
    const bool same = s.dev.version == s.h.file_version;
    s.state = same ? State::Done : State::Failed;
    s.reason = same ? "same_version" : "device_newer";
    return true;
}

// Every Query Next Image of the target, matching the image or not; kept after the end to show the new version. A
// matching query is judged by its version here too, so the answer does not hang on whether the stack offers an
// image it would not upgrade to.
inline void on_query(Session& s, uint64_t ieee, const Query& q) {
    if (s.state == State::Idle || ieee != s.ieee) return;
    s.dev = q;
    s.has_query = true;
    if (s.state == State::Waiting && fits(s)) ends_on_version(s);
}

// The stack found the image for a device's query: serve it only to the target, and only when it runs an older version.
inline bool on_start(Session& s, uint64_t ieee, const Query& q) {
    if (!active(s) || ieee != s.ieee) return false;
    on_query(s, ieee, q);
    if (!active(s)) return false;   // on_query ended it: same or newer version
    s.state = State::Sending;
    s.offset = 0;
    return true;
}

inline int pct(const Session& s) {
    if (!s.h.total) return 0;
    const uint64_t p = (uint64_t)s.offset * 100 / s.h.total;
    return p > 100 ? 100 : (int)p;
}

// A block went out; true when it starts a new tenth (for the log).
inline bool on_block(Session& s, uint32_t offset) {
    if (s.state != State::Sending || offset <= s.offset) return false;
    const int before = pct(s) / 10;
    s.offset = offset;
    return pct(s) / 10 > before;
}

// The device's Upgrade End: 0 = it has the whole image and applies it.
inline void on_end(Session& s, uint8_t status) {
    if (s.state != State::Sending) return;
    if (status == 0) {
        s.state = State::Done;
        s.offset = s.h.total;
    } else {
        s.state = State::Failed;
        s.reason = "device_status";
        s.device_status = status;
    }
}

inline void fail(Session& s, const char* reason) {
    if (!active(s)) return;
    s.state = State::Failed;
    s.reason = reason;
}

inline void cancel(Session& s) {
    if (active(s)) s.state = State::Cancelled;
}

inline const char* state_name(State st) {
    switch (st) {
        case State::Waiting: return "waiting";
        case State::Sending: return "sending";
        case State::Done: return "done";
        case State::Failed: return "failed";
        case State::Cancelled: return "cancelled";
        default: return "idle";
    }
}

}  // namespace devota
