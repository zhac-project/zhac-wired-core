// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Full backup (research 2026-11-18 C1): the file header, the stage header the restore leaves in the inactive OTA
// slot, and the rules around them. Pure C++, no IDF: host check test/host/backup/. tools/zhac_backup.py reads and
// writes the same file; change both together. Design: extra/docs/superpowers/specs/2026-11-19-full-backup-design.md.
//
// File (little-endian): this 228-byte header (also the AES-GCM associated data), the partitions back to back
// encrypted with AES-256-GCM, the 16-byte tag. Key = PBKDF2-HMAC-SHA256(password, salt, kdf_iter).
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace backup {

constexpr size_t   kMaxParts = 4;
constexpr size_t   kHeaderLen = 228;
constexpr size_t   kStageLen = 216;
constexpr size_t   kTagLen = 16;
constexpr uint32_t kSector = 4096;
constexpr uint32_t kStageDataOffset = kSector;   // stage header in sector 0, data after it
constexpr uint32_t kKdfIter = 100000, kKdfIterMin = 10000, kKdfIterMax = 10000000;
constexpr size_t   kPwMin = 8, kPwMax = 63;      // bytes, as the hub's sign-in password
// The storage partitions a full backup holds, in file order (partitions.csv, both targets).
constexpr const char* kPartNames[kMaxParts] = {"nvs", "zb_storage", "zhac_data", "scripts"};

struct Part { char name[16]; uint32_t size; };

struct Header {
    uint16_t n_parts;
    uint32_t kdf_iter;
    uint8_t  salt[16];
    uint8_t  nonce[12];
    uint32_t created;        // unix seconds; 0 = the hub's clock was not set
    uint32_t nwk_fc;         // the network's outgoing NWK frame counter; 0 = no network
    uint8_t  key_check[16];  // tells a wrong password before anything is written
    char     chip[16];
    char     fw[32];
    char     host[32];
    Part     parts[kMaxParts];
};

struct StagePart { char name[16]; uint32_t size; uint32_t offset; };

struct Stage {
    uint32_t  applied;       // 0xFFFFFFFF until the partitions are written, then 0 (no erase needed)
    uint32_t  fc_target;     // raise the NWK frame counter to at least this once the network resumes
    uint16_t  n_parts;
    uint8_t   sha256[32];    // of the staged data, all parts
    StagePart parts[kMaxParts];
    char      fw[32];
    char      host[32];
    uint32_t  created;
};

namespace detail {
inline void put32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
inline void put16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
inline uint32_t get32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
inline uint16_t get16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }
inline void put_str(uint8_t* p, const char* s, size_t n) {   // NUL-padded, cut to n - 1
    memset(p, 0, n);
    const size_t l = strnlen(s, n - 1);
    memcpy(p, s, l);
}
inline bool get_str(const uint8_t* p, char* out, size_t n, bool nonempty) {   // false without a NUL in n
    const size_t l = strnlen(reinterpret_cast<const char*>(p), n);
    if (l == n || (nonempty && l == 0)) return false;
    memcpy(out, p, n);
    return true;
}
inline bool size_ok(uint32_t s) { return s && s % kSector == 0; }
}  // namespace detail

inline uint32_t total(const Header& h) {
    uint32_t t = 0;
    for (uint16_t i = 0; i < h.n_parts && i < kMaxParts; i++) t += h.parts[i].size;
    return t;
}

inline void pack(const Header& h, uint8_t* b) {
    using namespace detail;
    memset(b, 0, kHeaderLen);
    memcpy(b, "ZHACBK01", 8);
    put16(b + 8, kHeaderLen);
    put16(b + 10, h.n_parts);
    put32(b + 12, h.kdf_iter);
    memcpy(b + 16, h.salt, 16);
    memcpy(b + 32, h.nonce, 12);
    put32(b + 44, h.created);
    put32(b + 48, h.nwk_fc);
    memcpy(b + 52, h.key_check, 16);
    put_str(b + 68, h.chip, 16);
    put_str(b + 84, h.fw, 32);
    put_str(b + 116, h.host, 32);
    for (size_t i = 0; i < kMaxParts; i++) {
        if (i >= h.n_parts) break;
        put_str(b + 148 + 20 * i, h.parts[i].name, 16);
        put32(b + 164 + 20 * i, h.parts[i].size);
    }
}

inline bool parse(const uint8_t* b, Header* h) {
    using namespace detail;
    Header r{};
    if (memcmp(b, "ZHACBK01", 8) != 0 || get16(b + 8) != kHeaderLen) return false;
    r.n_parts = get16(b + 10);
    r.kdf_iter = get32(b + 12);
    if (r.n_parts < 1 || r.n_parts > kMaxParts || r.kdf_iter < kKdfIterMin || r.kdf_iter > kKdfIterMax) return false;
    memcpy(r.salt, b + 16, 16);
    memcpy(r.nonce, b + 32, 12);
    r.created = get32(b + 44);
    r.nwk_fc = get32(b + 48);
    memcpy(r.key_check, b + 52, 16);
    if (!get_str(b + 68, r.chip, 16, false) || !get_str(b + 84, r.fw, 32, false) ||
        !get_str(b + 116, r.host, 32, false)) return false;
    for (size_t i = 0; i < r.n_parts; i++) {
        if (!get_str(b + 148 + 20 * i, r.parts[i].name, 16, true)) return false;
        r.parts[i].size = get32(b + 164 + 20 * i);
        if (!size_ok(r.parts[i].size)) return false;
    }
    *h = r;
    return true;
}

inline void pack(const Stage& s, uint8_t* b) {
    using namespace detail;
    memset(b, 0, kStageLen);
    memcpy(b, "ZHACRS01", 8);
    put32(b + 8, s.applied);
    put32(b + 12, s.fc_target);
    put16(b + 16, s.n_parts);
    memcpy(b + 20, s.sha256, 32);
    for (size_t i = 0; i < kMaxParts; i++) {
        if (i >= s.n_parts) break;
        put_str(b + 52 + 24 * i, s.parts[i].name, 16);
        put32(b + 68 + 24 * i, s.parts[i].size);
        put32(b + 72 + 24 * i, s.parts[i].offset);
    }
    put_str(b + 148, s.fw, 32);
    put_str(b + 180, s.host, 32);
    put32(b + 212, s.created);
}

inline bool parse(const uint8_t* b, Stage* s) {
    using namespace detail;
    Stage r{};
    if (memcmp(b, "ZHACRS01", 8) != 0) return false;
    r.applied = get32(b + 8);
    r.fc_target = get32(b + 12);
    r.n_parts = get16(b + 16);
    if (r.n_parts < 1 || r.n_parts > kMaxParts) return false;
    memcpy(r.sha256, b + 20, 32);
    for (size_t i = 0; i < r.n_parts; i++) {
        if (!get_str(b + 52 + 24 * i, r.parts[i].name, 16, true)) return false;
        r.parts[i].size = get32(b + 68 + 24 * i);
        r.parts[i].offset = get32(b + 72 + 24 * i);
        if (!size_ok(r.parts[i].size) || r.parts[i].offset < kStageDataOffset || r.parts[i].offset % kSector) return false;
    }
    if (!get_str(b + 148, r.fw, 32, false) || !get_str(b + 180, r.host, 32, false)) return false;
    r.created = get32(b + 212);
    *s = r;
    return true;
}

// The NWK frame counter a restored network must start from. Devices drop a frame whose counter is not above the
// last one they saw from the hub, and the old hub may have run on after the backup: allow 20 frames a second since
// then (90 days when either clock was not set), never less than this hub's own counter, plus 100 000. Rounded up to
// a multiple of 1024: the stack saves its counter (+1024) only when the counter reaches one (esp-zigbee-lib 2.1.0
// nwk_secur_inc_frame_cntr), so the raised counter is saved with the first frame after the raise (review P5 I1).
inline uint32_t fc_target(uint32_t nwk_fc, uint32_t created, uint32_t now, uint32_t fc_now) {
    constexpr uint32_t kClockSet = 1700000000;   // 2023-11: a clock below this was never set
    constexpr uint64_t kMargin = 100000, kRate = 20, kUnknown = 90ull * 86400, kCap = 0xF0000000u;
    if (nwk_fc == 0) return 0;
    const uint64_t elapsed = created >= kClockSet && now >= created ? now - created : kUnknown;
    uint64_t t = nwk_fc + kMargin + kRate * elapsed;
    if (fc_now + kMargin > t) t = fc_now + kMargin;
    t = (t + 1023) & ~uint64_t{1023};
    return (uint32_t)(t > kCap ? kCap : t);
}

// True when the backup's firmware is a later release than the hub's: both "vYYYYMMDDVV..." (dev builds are not
// compared). Older firmware may misread the stores of newer firmware.
inline bool newer_fw(const char* backup_fw, const char* hub_fw) {
    const auto tag = [](const char* s) {
        if (s[0] != 'v') return false;
        for (int i = 1; i <= 10; i++) if (s[i] < '0' || s[i] > '9') return false;
        return true;
    };
    return tag(backup_fw) && tag(hub_fw) && strncmp(backup_fw + 1, hub_fw + 1, 10) > 0;
}

// Whether this hub's partition can take the file's: the same size, or an NVS one larger here (NVS reads the erased
// rest as free pages). Hubs installed before 2026-10-25 have a 16 KB zb_storage, later ones 128 KB (wired 2d23a0e).
// SPIFFS (scripts) keeps its geometry in its size: the same only (review P5 I2).
inline bool fits(const char* name, uint32_t file_size, uint32_t hub_size) {
    return hub_size == file_size || (strcmp(name, "scripts") != 0 && hub_size > file_size);
}

// Every partition in the file must exist on this hub, once, and fit. `why` names the first that does not.
inline bool parts_match(const Header& h, const Part* hub, size_t n, const char** why) {
    for (uint16_t i = 0; i < h.n_parts; i++) {
        *why = h.parts[i].name;
        bool found = false;
        for (size_t j = 0; j < n; j++) found = found || (strcmp(hub[j].name, h.parts[i].name) == 0 && fits(h.parts[i].name, h.parts[i].size, hub[j].size));
        for (uint16_t k = 0; k < i; k++) found = found && strcmp(h.parts[k].name, h.parts[i].name) != 0;
        if (!found) return false;
    }
    *why = nullptr;
    return true;
}

}  // namespace backup
