// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// nvs_raw -- one U64 read straight from the bytes of an NVS partition that NVS
// cannot mount: ESP_ERR_NVS_NO_FREE_PAGES (full) or
// ESP_ERR_NVS_NEW_VERSION_FOUND (written by a newer IDF). The coordinator
// address pin has to survive exactly that case: Settings then offers "Erase
// storage", which keeps the Zigbee network and so needs the address it runs
// under (P1 review 2026-11-19 #2).
//
// Format (IDF nvs_flash, unencrypted): 4096-byte pages, each a 32-byte header
// (state, sequence number, ...), a 32-byte table of 2-bit entry states, then
// 126 entries of 32 bytes: nsIndex, type, span, chunkIndex, crc32, key[16],
// data[8]. A namespace is a U8 entry in namespace 0 whose value is its index;
// the crc covers an entry without its crc field. Checked against an image
// from IDF's nvs_partition_gen.py (test/host/test_nvs_raw.cpp).
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace nvs_raw {

using ReadFn = bool (*)(void* ctx, size_t offset, void* dst, size_t len);

constexpr size_t   kPage = 4096, kEntrySize = 32, kEntries = 126, kStatesAt = 32, kEntriesAt = 64;
constexpr uint32_t kActive = 0xFFFFFFFEu, kFull = 0xFFFFFFFCu, kFreeing = 0xFFFFFFF8u;
constexpr uint8_t  kTypeU8 = 0x01, kTypeU64 = 0x08, kWritten = 0x2;

inline uint32_t crc32_le(uint32_t crc, const uint8_t* p, size_t n) {   // as esp_rom_crc32_le
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

inline bool entry_ok(const uint8_t* e) {
    uint32_t want;
    std::memcpy(&want, e + 4, 4);
    uint32_t c = crc32_le(0xFFFFFFFFu, e, 4);
    c = crc32_le(c, e + 8, 16);
    c = crc32_le(c, e + 24, 8);
    return c == want;
}

inline bool key_is(const uint8_t* e, const char* key) {
    char k[17] = {};
    std::memcpy(k, e + 8, 16);
    return std::strcmp(k, key) == 0;
}

// fn(page sequence number, entry) for every written entry whose crc holds, in
// the pages in use; false on a read error. `page`: kPage bytes of scratch.
template <typename Fn>
bool each_entry(ReadFn read, void* ctx, size_t part_bytes, uint8_t* page, Fn fn) {
    for (size_t at = 0; at + kPage <= part_bytes; at += kPage) {
        if (!read(ctx, at, page, kPage)) return false;
        uint32_t state, seq;
        std::memcpy(&state, page, 4);
        std::memcpy(&seq, page + 4, 4);
        if (state != kActive && state != kFull && state != kFreeing) continue;
        for (size_t i = 0; i < kEntries;) {
            const uint8_t  st = (page[kStatesAt + i / 4] >> (i % 4 * 2)) & 3u;
            const uint8_t* e  = page + kEntriesAt + i * kEntrySize;
            if (st != kWritten || !entry_ok(e)) { i++; continue; }
            fn(seq, e);
            i += e[2] ? e[2] : 1;   // a string or blob spans several entries
        }
    }
    return true;
}

// U64 `key` in namespace `ns` (from the newest page if NVS left two copies);
// false when absent or unreadable. `page`: kPage bytes of scratch.
inline bool find_u64(ReadFn read, void* ctx, size_t part_bytes, uint8_t* page, const char* ns, const char* key,
                     uint64_t* out) {
    int ns_index = -1;
    if (!each_entry(read, ctx, part_bytes, page, [&](uint32_t, const uint8_t* e) {
            if (e[0] == 0 && e[1] == kTypeU8 && key_is(e, ns)) ns_index = e[24];
        }) || ns_index < 1)
        return false;
    bool     found = false;
    uint32_t newest = 0;
    if (!each_entry(read, ctx, part_bytes, page, [&](uint32_t seq, const uint8_t* e) {
            if (e[0] == ns_index && e[1] == kTypeU64 && key_is(e, key) && (!found || seq >= newest)) {
                std::memcpy(out, e + 24, 8);
                found  = true;
                newest = seq;
            }
        }))
        return false;
    return found;
}

}  // namespace nvs_raw
