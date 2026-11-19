// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Host check of the full backup (research 2026-11-18 C1): the file and stage headers (main/backup_format.h), the
// crypto (main/backup_crypto.cpp, on TF-PSA-Crypto built from the IDF tree; vectors from Python `cryptography`), and
// the core (main/backup_core.h): the snapshot, the file writer, the stager and the boot-time apply, on fake NOR flash
// that can fail any erase or write (a power cut) and change under a read (a store writing).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#define MBEDTLS_ALLOW_PRIVATE_ACCESS   // mbedtls_psa_stats_t, to count live keys
#include "backup_core.h"
#include "backup_crypto.h"
#include "backup_format.h"

static int s_fail = 0;
#define CHECK(c, m) do { if (!(c)) { s_fail++; printf("  FAIL: %s\n", m); } else printf("  ok:   %s\n", m); } while (0)

using namespace backup;

static uint32_t le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }

static Header sample_header() {
    Header h{};
    h.n_parts = 4;
    h.kdf_iter = kKdfIter;
    for (int i = 0; i < 16; i++) h.salt[i] = (uint8_t)(0x10 + i);
    for (int i = 0; i < 12; i++) h.nonce[i] = (uint8_t)(0x40 + i);
    h.created = 1795000000;
    h.nwk_fc = 0x01020304;
    for (int i = 0; i < 16; i++) h.key_check[i] = (uint8_t)(0xA0 + i);
    strcpy(h.chip, "esp32p4");
    strcpy(h.fw, "v2026111903-4-gee80888");
    strcpy(h.host, "zhac");
    const char* names[4] = {"nvs", "zb_storage", "zhac_data", "scripts"};
    const uint32_t sizes[4] = {0x10000, 0x20000, 0x100000, 0x200000};
    for (int i = 0; i < 4; i++) { strcpy(h.parts[i].name, names[i]); h.parts[i].size = sizes[i]; }
    return h;
}

static void test_header() {
    printf("file header\n");
    const Header h = sample_header();
    uint8_t b[kHeaderLen];
    memset(b, 0xEE, sizeof b);
    pack(h, b);
    CHECK(memcmp(b, "ZHACBK01", 8) == 0, "magic at 0");
    CHECK(le16(b + 8) == kHeaderLen && kHeaderLen == 228, "header_len 228 at 8");
    CHECK(le16(b + 10) == 4, "n_parts at 10");
    CHECK(le32(b + 12) == 100000, "kdf_iter at 12");
    CHECK(b[16] == 0x10 && b[31] == 0x1F, "salt at 16");
    CHECK(b[32] == 0x40 && b[43] == 0x4B, "nonce at 32");
    CHECK(le32(b + 44) == 1795000000, "created at 44");
    CHECK(le32(b + 48) == 0x01020304, "nwk_fc at 48");
    CHECK(b[52] == 0xA0 && b[67] == 0xAF, "key_check at 52");
    CHECK(strcmp((const char*)b + 68, "esp32p4") == 0 && b[83] == 0, "chip at 68, NUL-padded");
    CHECK(strcmp((const char*)b + 84, "v2026111903-4-gee80888") == 0 && b[115] == 0, "fw at 84, NUL-padded");
    CHECK(strcmp((const char*)b + 116, "zhac") == 0 && b[147] == 0, "host at 116, NUL-padded");
    CHECK(strcmp((const char*)b + 148, "nvs") == 0 && le32(b + 164) == 0x10000, "part 0 at 148");
    CHECK(strcmp((const char*)b + 208, "scripts") == 0 && le32(b + 224) == 0x200000, "part 3 at 208");
    CHECK(total(h) == 0x10000 + 0x20000 + 0x100000 + 0x200000, "total = sum of the part sizes");

    Header r{};
    CHECK(parse(b, &r), "packed header parses");
    CHECK(r.n_parts == 4 && r.kdf_iter == 100000 && r.created == h.created && r.nwk_fc == h.nwk_fc &&
          memcmp(r.salt, h.salt, 16) == 0 && memcmp(r.nonce, h.nonce, 12) == 0 &&
          memcmp(r.key_check, h.key_check, 16) == 0 && strcmp(r.fw, h.fw) == 0 && strcmp(r.host, "zhac") == 0 &&
          strcmp(r.chip, "esp32p4") == 0 && strcmp(r.parts[2].name, "zhac_data") == 0 && r.parts[3].size == 0x200000,
          "every field round-trips");

    Header lng = h;
    memset(lng.fw, 'x', sizeof lng.fw);   // no NUL: pack must cut it
    pack(lng, b);
    CHECK(b[115] == 0 && parse(b, &r) && strlen(r.fw) == 31, "an overlong string is cut to fit with its NUL");

    struct Bad { const char* what; size_t at; uint8_t val; } bad[] = {
        {"wrong magic", 7, '2'},
        {"header_len not 228", 8, 229 & 0xFF},
        {"n_parts 0", 10, 0},
        {"n_parts 5", 10, 5},
        {"part size not a multiple of 4096", 164, 0x01},
        {"empty part name", 148, 0},
    };
    for (const Bad& x : bad) {
        pack(h, b);
        b[x.at] = x.val;
        char m[96];
        snprintf(m, sizeof m, "refused: %s", x.what);
        CHECK(!parse(b, &r), m);
    }
    struct Field { const char* what; size_t at, len; } full[] = {
        {"chip", 68, 16}, {"fw", 84, 32}, {"host", 116, 32}, {"a part name", 148, 16}};
    for (const Field& f : full) {
        pack(h, b);
        memset(b + f.at, 'x', f.len);
        char m[96];
        snprintf(m, sizeof m, "refused: %s without its NUL", f.what);
        CHECK(!parse(b, &r), m);
    }
    Header k = h;
    k.kdf_iter = 9999;     pack(k, b); CHECK(!parse(b, &r), "refused: kdf_iter 9 999");
    k.kdf_iter = 10000;    pack(k, b); CHECK(parse(b, &r), "kdf_iter 10 000 parses");
    k.kdf_iter = 10000001; pack(k, b); CHECK(!parse(b, &r), "refused: kdf_iter above 10 000 000");
    k = h; k.parts[1].size = 0; pack(k, b); CHECK(!parse(b, &r), "refused: a part of size 0");
    k = h; k.n_parts = 2; pack(k, b);
    CHECK(parse(b, &r) && r.n_parts == 2 && total(r) == 0x30000, "two parts: only those count");
}

static Stage sample_stage() {
    Stage s{};
    s.applied = 0xFFFFFFFF;
    s.fc_target = 0x00ABCDEF;
    s.n_parts = 4;
    for (int i = 0; i < 32; i++) s.sha256[i] = (uint8_t)i;
    const Header h = sample_header();
    uint32_t off = kStageDataOffset;
    for (int i = 0; i < 4; i++) {
        strcpy(s.parts[i].name, h.parts[i].name);
        s.parts[i].size = h.parts[i].size;
        s.parts[i].offset = off;
        off += h.parts[i].size;
    }
    strcpy(s.fw, h.fw);
    strcpy(s.host, h.host);
    s.created = h.created;
    return s;
}

static void test_stage() {
    printf("stage header\n");
    const Stage s = sample_stage();
    uint8_t b[kStageLen];
    pack(s, b);
    CHECK(kStageLen == 216 && memcmp(b, "ZHACRS01", 8) == 0, "magic at 0, 216 bytes");
    CHECK(le32(b + 8) == 0xFFFFFFFF, "applied at 8, all ones (written to 0 later without an erase)");
    CHECK(le32(b + 12) == 0x00ABCDEF && le16(b + 16) == 4, "fc_target at 12, n_parts at 16");
    CHECK(b[20] == 0 && b[51] == 31, "sha256 at 20");
    CHECK(strcmp((const char*)b + 52, "nvs") == 0 && le32(b + 68) == 0x10000 && le32(b + 72) == 4096, "part 0 at 52");
    CHECK(strcmp((const char*)b + 124, "scripts") == 0 && le32(b + 144) == 4096 + 0x130000, "part 3 at 124");
    CHECK(strcmp((const char*)b + 148, s.fw) == 0 && strcmp((const char*)b + 180, "zhac") == 0 &&
          le32(b + 212) == 1795000000, "fw at 148, host at 180, created at 212");
    Stage r{};
    CHECK(parse(b, &r) && r.applied == 0xFFFFFFFF && r.fc_target == 0x00ABCDEF && r.n_parts == 4 &&
          memcmp(r.sha256, s.sha256, 32) == 0 && r.parts[3].offset == 4096 + 0x130000 &&
          strcmp(r.parts[1].name, "zb_storage") == 0 && r.created == s.created, "every field round-trips");
    b[8] = b[9] = b[10] = b[11] = 0;
    CHECK(parse(b, &r) && r.applied == 0, "applied 0 parses");
    pack(s, b); b[0] = 0xFF;
    CHECK(!parse(b, &r), "refused: no magic (a stage cut short before its magic was written)");
    pack(s, b); memset(b, 0xFF, sizeof b);
    CHECK(!parse(b, &r), "refused: an erased sector");
    Stage o = s; o.parts[1].offset = 4096 + 0x10000 + 1; pack(o, b);
    CHECK(!parse(b, &r), "refused: an offset not on a 4096 boundary");
    o = s; o.parts[0].offset = 0; pack(o, b);
    CHECK(!parse(b, &r), "refused: data over the stage header");
    o = s; o.n_parts = 0; pack(o, b);
    CHECK(!parse(b, &r), "refused: n_parts 0");
}

static void test_fc_target() {
    printf("frame counter target\n");
    const uint32_t t0 = 1795000000;
    // The stack saves its counter (+1024) only when the counter reaches a multiple of 1024 (esp-zigbee-lib 2.1.0
    // nwk_secur_inc_frame_cntr): a target on one is saved with the first frame after the raise (review P5 I1).
    const auto up = [](uint64_t v) { return (uint32_t)((v + 1023) / 1024 * 1024); };
    CHECK(fc_target(0, t0, t0 + 60, 5000) == 0, "no network in the backup: nothing to raise");
    CHECK(fc_target(1000, t0, t0 + 3600, 0) == up(1000 + 100000 + 20 * 3600), "an hour later: +100 000 +20/s");
    CHECK(fc_target(1000, t0, t0 + 3600, 500000) == up(600000), "this hub's own counter is higher: it +100 000");
    CHECK(fc_target(1000, 0, t0, 0) == up(1000 + 100000 + 20u * 90 * 86400), "backup clock unknown: 90 days");
    CHECK(fc_target(1000, t0, 0, 0) == up(1000 + 100000 + 20u * 90 * 86400), "hub clock unknown: 90 days");
    CHECK(fc_target(1000, t0, t0 - 10, 0) == up(1000 + 100000 + 20u * 90 * 86400), "clock behind the backup: 90 days");
    CHECK(fc_target(1000, 1600000000, t0, 0) == up(1000 + 100000 + 20u * 90 * 86400),
          "a backup stamped before the clock could be right (2020): 90 days");
    CHECK(fc_target(0xEFFFFFFF, t0, t0 + 1, 0) == 0xF0000000u, "capped at 0xF0000000");
    CHECK(fc_target(1000, t0, t0 + 400000000u, 0) == 0xF0000000u, "a very old backup: capped");
    CHECK(fc_target(1000, t0, t0 + 3600, 0xFFFFFFF0u) == 0xF0000000u, "a huge counter here: capped too");
    bool all = true;
    for (uint32_t fc : {1u, 1000u, 123456u, 0x7FFFFu, 0xEFFFF000u})
        for (uint32_t dt : {0u, 1u, 3600u, 86400u * 30}) all = all && fc_target(fc, t0, t0 + dt, fc / 2) % 1024 == 0;
    CHECK(all, "always a multiple of 1024, so the stack saves it with its first frame");
}

static void test_versions() {
    printf("firmware versions\n");
    CHECK(newer_fw("v2026112001", "v2026111703-12-gabc1234"), "a later release is newer");
    CHECK(newer_fw("v2026112001-3-gdeadbee", "v2026111703"), "a later release with commits is newer");
    CHECK(!newer_fw("v2026111703", "v2026111703-12-gabc1234"), "the same release is not");
    CHECK(!newer_fw("v2026111001", "v2026111703"), "an older release is not");
    CHECK(!newer_fw("abc1234-dirty", "v2026111703"), "a dev build backup: not compared");
    CHECK(!newer_fw("v2026112001", "ee80888-dirty"), "a dev build hub: not compared");
    CHECK(!newer_fw("v20261120", "v2026111703"), "a short tag: not compared");
    CHECK(!newer_fw("v2026112x01", "v2026111703"), "a non-digit: not compared");
}

static void test_parts_match() {
    printf("parts against this hub\n");
    const Part hub[4] = {{"nvs", 0x10000}, {"zb_storage", 0x20000}, {"zhac_data", 0x100000}, {"scripts", 0x200000}};
    const Header h = sample_header();
    const char* why = nullptr;
    CHECK(parts_match(h, hub, 4, &why), "the same four partitions match");
    // Hubs installed before 2026-10-25 have a 16 KB zb_storage, later ones 128 KB (wired 2d23a0e): an NVS
    // partition may be larger here, never smaller; scripts (SPIFFS) only the same size (review P5 I2).
    Header x = h; x.parts[1].size = 0x4000;
    CHECK(parts_match(x, hub, 4, &why), "a smaller NVS partition in the file (old 16 KB zb_storage): fits");
    x = h; x.parts[2].size = 0x200000;
    CHECK(!parts_match(x, hub, 4, &why) && why && strcmp(why, "zhac_data") == 0, "a larger one in the file: refused, named");
    x = h; x.parts[3].size = 0x100000;
    CHECK(!parts_match(x, hub, 4, &why) && why && strcmp(why, "scripts") == 0, "scripts of another size: refused");
    x = h; strcpy(x.parts[0].name, "phy_init");
    CHECK(!parts_match(x, hub, 4, &why) && why && strcmp(why, "phy_init") == 0, "a partition this hub lacks: refused");
    x = h; strcpy(x.parts[3].name, "nvs"); x.parts[3].size = 0x10000;
    CHECK(!parts_match(x, hub, 4, &why) && why && strcmp(why, "nvs") == 0, "a partition twice: refused");
    x = h; x.n_parts = 3;
    CHECK(parts_match(x, hub, 4, &why), "fewer partitions: the others stay as they are");
}

static std::vector<uint8_t> hex(const char* h) {
    std::vector<uint8_t> v;
    for (; h[0] && h[1]; h += 2) { unsigned x; sscanf(h, "%2x", &x); v.push_back((uint8_t)x); }
    return v;
}

static void test_kdf() {
    printf("key derivation\n");
    uint8_t salt[16], key[32], chk[16];
    for (int i = 0; i < 16; i++) salt[i] = (uint8_t)(0x10 + i);
    const char* pw = "correct horse battery";
    CHECK(derive_key(pw, strlen(pw), salt, 10000, key) &&
          memcmp(key, hex("f0541e7cbfd06b18ed260e0c55b6a0502b52c1dd726b70ba240e96523d621ce0").data(), 32) == 0,
          "PBKDF2-HMAC-SHA256 10 000 matches Python");
    CHECK(key_check(key, chk) && memcmp(chk, hex("42d8431eea7a6d17cf6c9282295bd546").data(), 16) == 0,
          "key check = SHA-256(label | key)[0..16) matches Python");
    const char* pw2 = "\xd0\xbf\xd0\xb0\xd1\x80\xd0\xbe\xd0\xbb\xd1\x8c-\xc5\xbb\xc3\x93\xc5\x81W-\xe5\xaf\x86\xe7\xa0\x81-1234567890";
    CHECK(strlen(pw2) == 38 && derive_key(pw2, strlen(pw2), salt, 10000, key) &&
          memcmp(key, hex("b11d7c36fdd16f1bc4c2c22ae48ddd507a6e66261c2eaa974aa609f9d71cf343").data(), 32) == 0,
          "a UTF-8 password: its bytes, as Python");
}

static void test_gcm() {
    printf("AES-256-GCM, streamed\n");
    const std::vector<uint8_t> key = hex("f0541e7cbfd06b18ed260e0c55b6a0502b52c1dd726b70ba240e96523d621ce0");
    uint8_t nonce[12], aad[228], tag[16];
    for (int i = 0; i < 12; i++) nonce[i] = (uint8_t)(0x40 + i);
    for (int i = 0; i < 228; i++) aad[i] = (uint8_t)(i * 3);
    std::vector<uint8_t> pt(10000), ct(10000 + 64), back(10000 + 64);
    for (size_t i = 0; i < pt.size(); i++) pt[i] = (uint8_t)(i * 7);

    // Odd piece sizes: GCM may hold back a partial block, the totals must still add up.
    const size_t pieces[] = {1, 15, 4096, 1000, 17, 2871, 2000};
    Gcm e;
    size_t in = 0, out = 0, n = 0;
    bool ok = e.begin(key.data(), nonce, aad, sizeof aad, true);
    for (size_t p : pieces) {
        ok = ok && e.update(pt.data() + in, p, ct.data() + out, ct.size() - out, &n);
        in += p; out += n;
    }
    ok = ok && in == pt.size() && e.finish_encrypt(ct.data() + out, ct.size() - out, &n, tag);
    out += n;
    CHECK(ok && out == 10000, "10 000 bytes in odd pieces: 10 000 out");
    CHECK(memcmp(ct.data(), hex("84e85197d3dbbfdc82d2cdd0d66b3a41").data(), 16) == 0 &&
          memcmp(ct.data() + 9984, hex("28d1937fa323e31b4c3709fc24e069c1").data(), 16) == 0 &&
          memcmp(tag, hex("c8543f4e4c332efa128bdf886093ddb7").data(), 16) == 0,
          "ciphertext and tag match Python's AESGCM");

    const auto decrypt = [&](const uint8_t* c, const uint8_t* a, const uint8_t* t, size_t piece) {
        Gcm d;
        size_t o = 0, m = 0;
        bool r = d.begin(key.data(), nonce, a, sizeof aad, false);
        for (size_t i = 0; r && i < 10000; i += piece) {
            const size_t len = i + piece > 10000 ? 10000 - i : piece;
            r = d.update(c + i, len, back.data() + o, back.size() - o, &m);
            o += m;
        }
        r = r && d.finish_decrypt(back.data() + o, back.size() - o, &m, t);
        return r && o + m == 10000 && memcmp(back.data(), pt.data(), 10000) == 0;
    };
    CHECK(decrypt(ct.data(), aad, tag, 333), "decrypted in other pieces: the plaintext, tag accepted");
    ct[5000] ^= 1;
    CHECK(!decrypt(ct.data(), aad, tag, 4096), "one ciphertext bit flipped: refused");
    ct[5000] ^= 1;
    aad[100] ^= 1;
    CHECK(!decrypt(ct.data(), aad, tag, 4096), "one header bit flipped: refused");
    aad[100] ^= 1;
    tag[15] ^= 1;
    CHECK(!decrypt(ct.data(), aad, tag, 4096), "a wrong tag: refused");
    tag[15] ^= 1;
    CHECK(decrypt(ct.data(), aad, tag, 4096), "restored: accepted again");

    // Every Gcm gives its key back (a leak would grow with each backup on the hub).
    mbedtls_psa_stats_t before, after;
    mbedtls_psa_get_stats(&before);
    bool all = true;
    for (int i = 0; i < 100; i++) { Gcm g; all = all && g.begin(key.data(), nonce, aad, sizeof aad, true); }
    { Gcm g; all = all && g.begin(key.data(), nonce, aad, sizeof aad, false); }   // dropped mid-stream
    mbedtls_psa_get_stats(&after);
    CHECK(all && after.MBEDTLS_PRIVATE(volatile_slots) == before.MBEDTLS_PRIVATE(volatile_slots),
          "101 in a row, one dropped unfinished: no key left behind");
    Gcm twice;
    CHECK(twice.begin(key.data(), nonce, aad, sizeof aad, true) && twice.begin(key.data(), nonce, aad, sizeof aad, false),
          "begin again on the same object: starts over");
}

static void test_sha() {
    printf("SHA-256, streamed\n");
    Sha256 h;
    uint8_t d[32];
    CHECK(h.begin() && h.update((const uint8_t*)"a", 1) && h.update((const uint8_t*)"bc", 2) && h.finish(d) &&
          memcmp(d, hex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad").data(), 32) == 0,
          "\"abc\" in two pieces");
}

// ── core ────────────────────────────────────────────────────────────────────
// NOR flash: erase sets 4096-byte sectors to 0xFF, a write can only clear bits. fail_after = the erases and writes
// that still succeed (a power cut after them); on_read runs before each read (a store writing meanwhile).
struct Fake : Flash {
    std::vector<uint8_t> m;
    int fail_after = -1, ops = 0, erases = 0, writes = 0, reads = 0;
    std::vector<std::pair<uint32_t, size_t>> wlog;
    std::function<void(Fake&)> on_read;
    Fake(size_t n, uint8_t fill) : m(n, fill) {}
    uint32_t size() const override { return (uint32_t)m.size(); }
    bool cut() { return fail_after >= 0 && ops++ >= fail_after; }
    bool read(uint32_t off, void* d, size_t n) override {
        if (off + n > m.size()) return false;
        reads++;
        if (on_read) on_read(*this);
        memcpy(d, m.data() + off, n);
        return true;
    }
    bool write(uint32_t off, const void* s, size_t n) override {
        if (off + n > m.size() || cut()) return false;
        for (size_t i = 0; i < n; i++) m[off + i] &= static_cast<const uint8_t*>(s)[i];
        writes++;
        wlog.push_back({off, n});
        return true;
    }
    bool erase(uint32_t off, size_t n) override {
        if (off % kSector || n % kSector || off + n > m.size() || cut()) return false;
        memset(m.data() + off, 0xFF, n);
        erases++;
        return true;
    }
};

struct MemSink : Sink {
    std::vector<uint8_t> b;
    size_t fail_at = SIZE_MAX;
    bool put(const uint8_t* p, size_t n) override {
        if (b.size() + n > fail_at) return false;
        b.insert(b.end(), p, p + n);
        return true;
    }
};

static const uint32_t kSizes[4] = {0x1000, 0x2000, 0x10000, 0x20000};   // small stand-ins for the four partitions
static const char kPw[] = "correct horse battery";

struct Hub {   // this hub's partitions (the backup's source, or the restore's target)
    Fake p[4] = {Fake(kSizes[0], 0), Fake(kSizes[1], 0), Fake(kSizes[2], 0), Fake(kSizes[3], 0)};
    Part list[4];
    Hub(uint8_t seed) {
        for (int i = 0; i < 4; i++) {
            for (size_t j = 0; j < p[i].m.size(); j++) p[i].m[j] = (uint8_t)(j * (i + 3) + seed);
            strcpy(list[i].name, kPartNames[i]);
            list[i].size = kSizes[i];
        }
    }
    static Flash* find(const char* name, void* ctx) {
        Hub* h = static_cast<Hub*>(ctx);
        for (int i = 0; i < 4; i++) if (strcmp(kPartNames[i], name) == 0) return &h->p[i];
        return nullptr;
    }
};

static Header file_header(const Hub& hub) {
    Header h{};
    h.n_parts = 4;
    h.kdf_iter = kKdfIterMin;   // fast enough for a test
    for (int i = 0; i < 16; i++) h.salt[i] = (uint8_t)(0x10 + i);
    for (int i = 0; i < 12; i++) h.nonce[i] = (uint8_t)(0x40 + i);
    h.created = 1795000000;
    h.nwk_fc = 123456;
    strcpy(h.chip, "esp32p4");
    strcpy(h.fw, "v2026111903");
    strcpy(h.host, "zhac");
    memcpy(h.parts, hub.list, sizeof hub.list);
    return h;
}

struct Bufs {   // one snapshot buffer per partition, as the download handler allocates them
    std::vector<uint8_t> v[4];
    uint8_t* p[4];
    explicit Bufs(const Hub& h) {
        for (int i = 0; i < 4; i++) { v[i].resize(h.list[i].size); p[i] = v[i].data(); }
    }
};

static Err write_hub(Hub& src, Sink& out) {
    Bufs b(src);
    std::vector<uint8_t> io(kPiece + 16);
    Flash* parts[4] = {&src.p[0], &src.p[1], &src.p[2], &src.p[3]};
    return write_file(file_header(src), kPw, strlen(kPw), parts, b.p, out, io.data(), nullptr);
}

static std::vector<uint8_t> make_file(Hub& src) {
    MemSink out;
    return write_hub(src, out) == Err::None ? out.b : std::vector<uint8_t>{};
}

// Upload `file` into `slot` the way the restore handler does, in `piece`-byte reads. Returns the last error.
static Err stage(const std::vector<uint8_t>& file, Fake& slot, const Hub& target, const char* pw, size_t piece,
                 uint32_t fc = 777) {
    Header h{};
    const char* why = nullptr;
    Err e = check(file.data(), &h, target.list, 4, "v2026111903", &why);
    if (e != Err::None) return e;
    if (file.size() != kHeaderLen + total(h) + kTagLen) return Err::Length;
    uint8_t key[32];
    if ((e = unlock(h, pw, strlen(pw), key)) != Err::None) return e;
    Stager st;
    if ((e = st.begin(slot, h, file.data(), key)) != Err::None) return e;
    const size_t ct_end = file.size() - kTagLen;
    for (size_t off = kHeaderLen; off < ct_end; off += piece) {
        const size_t n = off + piece > ct_end ? ct_end - off : piece;
        if ((e = st.feed(file.data() + off, n)) != Err::None) return e;
    }
    return st.finish(file.data() + ct_end, fc);
}

static bool same(const Hub& a, const Hub& b) {
    for (int i = 0; i < 4; i++) if (a.p[i].m != b.p[i].m) return false;
    return true;
}

static void test_snapshot() {
    printf("snapshot\n");
    Fake f(0x3000, 0), g(0x1000, 0);
    for (size_t i = 0; i < f.m.size(); i++) f.m[i] = (uint8_t)i;
    std::vector<uint8_t> buf(f.m.size()), gbuf(g.m.size()), tmp(kPiece);
    Flash* parts[2] = {&f, &g};
    uint8_t* bufs[2] = {buf.data(), gbuf.data()};
    CHECK(snapshot(parts, bufs, 2, tmp.data(), 5, nullptr) && buf == f.m && gbuf == g.m, "quiet partitions: copied as they are");
    int n = 0;
    f.on_read = [&n](Fake& x) { if (++n == 2) x.m[0x2100] ^= 0xFF; };   // a store writes during the first pass
    CHECK(snapshot(parts, bufs, 2, tmp.data(), 5, nullptr) && buf == f.m, "written during the copy: copied again, the new state");
    // One instant for all of them: a write to the first while the second is copied redoes both.
    f.on_read = nullptr;
    int k = 0;
    g.on_read = [&](Fake&) { if (++k == 1) f.m[0x10] ^= 0xFF; };
    CHECK(snapshot(parts, bufs, 2, tmp.data(), 5, nullptr) && buf == f.m && k > 2,
          "a write to one while another is copied: all copied again");
    g.on_read = nullptr;
    f.on_read = [](Fake& x) { x.m[0x10]++; };                              // never settles
    CHECK(!snapshot(parts, bufs, 2, tmp.data(), 5, nullptr), "never quiet: refused after the tries");
}

static void test_round_trip() {
    printf("backup and restore, end to end\n");
    Hub src(1), dst(9);
    const std::vector<uint8_t> file = make_file(src);
    CHECK(file.size() == kHeaderLen + 0x33000 + kTagLen, "file = header + the partitions + tag");
    Header h{};
    CHECK(file.size() > kHeaderLen && parse(file.data(), &h) && h.nwk_fc == 123456 && h.kdf_iter == kKdfIterMin,
          "its header parses");
    uint8_t key[32], chk[16];
    CHECK(derive_key(kPw, strlen(kPw), h.salt, h.kdf_iter, key) && key_check(key, chk) && memcmp(chk, h.key_check, 16) == 0,
          "the key check in the file is the password's");

    Fake slot(0x40000 + 0x10000, 0x00);   // the old app image there: nothing erased
    CHECK(stage(file, slot, dst, kPw, 1000) == Err::None, "staged in 1000-byte pieces");
    Stage st{};
    CHECK(parse(slot.m.data(), &st) && st.applied == 0xFFFFFFFF && st.fc_target == 777 && st.n_parts == 4 &&
          st.parts[0].offset == kStageDataOffset && st.parts[3].offset == kStageDataOffset + 0x13000 &&
          strcmp(st.host, "zhac") == 0, "stage header: armed, not applied, offsets after sector 0");
    CHECK(memcmp(slot.m.data() + st.parts[2].offset, src.p[2].m.data(), kSizes[2]) == 0, "the plaintext is in the slot");
    CHECK(!slot.wlog.empty() && slot.wlog.back().first == 0 && slot.wlog.back().second == 8, "the magic was written last");
    CHECK(!same(src, dst), "(the target still holds its own data)");

    Stage got{};
    std::vector<uint8_t> tmp(kPiece);
    CHECK(apply(slot, Hub::find, &dst, tmp.data(), &got) == Applied::Done && same(src, dst) && got.fc_target == 777,
          "boot: applied, the target equals the source");
    CHECK(parse(slot.m.data(), &st) && st.applied == 0, "marked applied");
    const int erases = dst.p[0].erases + dst.p[1].erases + dst.p[2].erases + dst.p[3].erases;
    CHECK(apply(slot, Hub::find, &dst, tmp.data(), &got) == Applied::AlreadyDone && got.fc_target == 777 &&
          dst.p[0].erases + dst.p[1].erases + dst.p[2].erases + dst.p[3].erases == erases,
          "next boot: nothing written again, the frame counter target still handed out");
    Fake empty(0x50000, 0xFF);
    CHECK(apply(empty, Hub::find, &dst, tmp.data(), &got) == Applied::Nothing, "an erased slot: nothing to do");
}

static void test_larger_target() {
    printf("restore onto a hub with a larger NVS partition\n");
    Hub src(1), big(9);
    big.p[1] = Fake(kSizes[1] * 4, 0x5A);   // a new install's zb_storage, holding its own network
    big.list[1].size = kSizes[1] * 4;
    const std::vector<uint8_t> file = make_file(src);
    Fake slot(0x50000, 0x00);
    CHECK(stage(file, slot, big, kPw, 4096) == Err::None, "staged");
    Stage got{};
    std::vector<uint8_t> tmp(kPiece);
    CHECK(apply(slot, Hub::find, &big, tmp.data(), &got) == Applied::Done, "applied");
    const std::vector<uint8_t>& z = big.p[1].m;
    CHECK(memcmp(z.data(), src.p[1].m.data(), kSizes[1]) == 0, "the image at the start");
    bool erased = true;
    for (size_t i = kSizes[1]; i < z.size(); i++) erased = erased && z[i] == 0xFF;
    CHECK(erased, "the rest erased: free NVS pages, nothing of the old network");
    Hub old(9);
    Hub big_src(3);
    big_src.p[1] = Fake(kSizes[1] * 4, 0x33);
    big_src.list[1].size = kSizes[1] * 4;
    Fake slot2(0x60000, 0x00);
    CHECK(stage(make_file(big_src), slot2, old, kPw, 4096) == Err::Parts && slot2.erases == 0,
          "the other way (a larger zb_storage in the file): refused, untouched");
}

static void test_refusals() {
    printf("restore refusals\n");
    Hub src(1), dst(9);
    const Hub orig(9);
    std::vector<uint8_t> file = make_file(src);
    Fake slot(0x50000, 0x00);
    CHECK(stage(file, slot, dst, "wrong password", 4096) == Err::Password && slot.erases == 0 && slot.writes == 0,
          "wrong password: refused before the slot is touched");
    {
        Header hk{};
        parse(file.data(), &hk);
        hk.key_check[15] ^= 1;   // all but the last byte the password's
        uint8_t key[32];
        CHECK(unlock(hk, kPw, strlen(kPw), key) == Err::Password, "a key check off in its last byte: refused");
    }
    Hub small(9);
    small.list[2].size = 0x8000;
    CHECK(stage(file, slot, small, kPw, 4096) == Err::Parts && slot.erases == 0, "a partition size differs: refused");
    Header h{};
    parse(file.data(), &h);
    const char* why = nullptr;
    CHECK(check(file.data(), &h, dst.list, 4, "v2026111001", &why) == Err::Newer, "newer firmware's backup: refused");
    std::vector<uint8_t> junk = file;
    junk[0] = 'X';
    CHECK(check(junk.data(), &h, dst.list, 4, "v2026111903", &why) == Err::Format, "not a backup: refused");

    std::vector<uint8_t> bad = file;
    bad[kHeaderLen + 0x5000] ^= 1;
    Stage st{};
    std::vector<uint8_t> tmp(kPiece);
    CHECK(stage(bad, slot, dst, kPw, 4096) == Err::Damaged && !parse(slot.m.data(), &st), "a flipped bit: damaged, not armed");
    CHECK(apply(slot, Hub::find, &dst, tmp.data(), &st) == Applied::Nothing && same(dst, orig), "and boot keeps the storage");

    // An earlier restore's applied header is gone as soon as a new staging starts, even if that one then fails.
    CHECK(stage(file, slot, dst, kPw, 4096) == Err::None && apply(slot, Hub::find, &dst, tmp.data(), &st) == Applied::Done,
          "(an earlier restore, applied)");
    Hub dst2(5);
    bad = file;
    bad[kHeaderLen + 10] ^= 1;
    CHECK(stage(bad, slot, dst2, kPw, 4096) == Err::Damaged && !parse(slot.m.data(), &st),
          "a later damaged upload leaves no header at all, not the old one");

    Header hh{};
    parse(file.data(), &hh);
    uint8_t key[32];
    unlock(hh, kPw, strlen(kPw), key);
    Stager s1;
    Fake tiny(0x20000, 0);
    CHECK(s1.begin(tiny, hh, file.data(), key) == Err::Space && tiny.erases == 0, "a slot too small: refused, untouched");
    Fake slot3(0x50000, 0);
    Stager s2;
    CHECK(s2.begin(slot3, hh, file.data(), key) == Err::None && s2.feed(file.data() + kHeaderLen, 4096) == Err::None &&
          s2.finish(file.data() + file.size() - kTagLen, 1) == Err::Length && !parse(slot3.m.data(), &st),
          "finished before all the data came: refused, not armed");
    Stager s3;
    CHECK(s3.begin(slot3, hh, file.data(), key) == Err::None && s3.feed(file.data(), kPiece + 1) == Err::Length,
          "a piece larger than kPiece: refused");
}

static void test_power_cuts() {
    printf("power cuts\n");
    Hub src(1);
    const std::vector<uint8_t> file = make_file(src);
    std::vector<uint8_t> tmp(kPiece);
    Stage st{};
    // During the upload: whatever was written, no header is armed.
    for (int k : {0, 1, 5, 30, 55}) {   // a whole staging here takes 57 erases and writes
        Hub dst(9);
        const Hub orig(9);
        Fake slot(0x50000, 0x00);
        slot.fail_after = k;
        const Err e = stage(file, slot, dst, kPw, 4096);
        slot.fail_after = -1;
        char m[96];
        snprintf(m, sizeof m, "cut after %d slot writes: upload fails, boot keeps the storage", k);
        CHECK(e == Err::Flash && apply(slot, Hub::find, &dst, tmp.data(), &st) == Applied::Nothing && same(dst, orig), m);
    }
    // The header's magic is its last write: a cut just before it leaves no header.
    {
        Hub dst(9);
        Fake count(0x50000, 0x00);
        stage(file, count, dst, kPw, 4096);
        Fake slot(0x50000, 0x00);
        slot.fail_after = count.erases + count.writes - 1;
        CHECK(stage(file, slot, dst, kPw, 4096) == Err::Flash && !parse(slot.m.data(), &st), "cut before the magic: not armed");
    }
    // During the boot-time apply: the next boot applies it again, from the start, and completes.
    for (int k : {0, 1, 3, 10, 20}) {   // ops a partition takes here: 2, 3, 17, 33
        Hub dst(9);
        Fake slot(0x50000, 0x00);
        stage(file, slot, dst, kPw, 4096);
        for (Fake& p : dst.p) { p.fail_after = k; p.ops = 0; }   // each partition: k erases/writes, then the cut
        const Applied first = apply(slot, Hub::find, &dst, tmp.data(), &st);
        for (Fake& p : dst.p) p.fail_after = -1;
        const Applied second = apply(slot, Hub::find, &dst, tmp.data(), &st);
        char m[96];
        snprintf(m, sizeof m, "cut during apply (%d ops per partition): next boot applies it all", k);
        CHECK(first == Applied::Flash && second == Applied::Done && same(src, dst), m);
    }
}

static void test_boot_checks() {
    printf("boot checks\n");
    Hub src(1);
    const std::vector<uint8_t> file = make_file(src);
    std::vector<uint8_t> tmp(kPiece);
    Stage st{};
    {
        Hub dst(9);
        const Hub orig(9);
        Fake slot(0x50000, 0x00);
        stage(file, slot, dst, kPw, 4096);
        slot.m[kStageDataOffset + 0x2345] ^= 0x10;   // the staged copy went bad
        CHECK(apply(slot, Hub::find, &dst, tmp.data(), &st) == Applied::BadSha && same(dst, orig) && !parse(slot.m.data(), &st),
              "staged data does not match its SHA-256: storage kept, header disarmed");
    }
    {
        Hub dst(9);
        const Hub orig(9);
        Fake slot(0x50000, 0x00);
        stage(file, slot, dst, kPw, 4096);
        dst.p[3].m.resize(0x10000);   // the last partition is smaller now
        CHECK(apply(slot, Hub::find, &dst, tmp.data(), &st) == Applied::Parts && !parse(slot.m.data(), &st) &&
              dst.p[0].m == orig.p[0].m && dst.p[1].m == orig.p[1].m && dst.p[2].m == orig.p[2].m,
              "a partition no longer fits: nothing written (not even the ones before it), disarmed");
    }
}

static void test_stage_bounds() {
    printf("stage header past the slot\n");
    Hub src(1), dst(9);
    const Hub orig(9);
    const std::vector<uint8_t> file = make_file(src);
    Fake slot(0x50000, 0x00);
    std::vector<uint8_t> tmp(kPiece);
    Stage st{};
    stage(file, slot, dst, kPw, 4096);
    parse(slot.m.data(), &st);
    st.parts[3].offset = 0x48000;   // its 128 KB would run past the 320 KB slot
    uint8_t b[kStageLen];
    pack(st, b);
    slot.erase(0, kSector);
    slot.write(0, b, kStageLen);
    CHECK(apply(slot, Hub::find, &dst, tmp.data(), &st) == Applied::Parts && same(dst, orig) && !parse(slot.m.data(), &st),
          "a stage pointing past the slot: disarmed (not retried at every boot), storage kept");
}

static void test_write_errors() {
    printf("download errors\n");
    Hub src(1);
    MemSink cut;
    cut.fail_at = 5000;
    CHECK(write_hub(src, cut) == Err::Sink, "the browser went away: stops");
    src.p[2].on_read = [](Fake& x) { x.m[7]++; };
    MemSink out;
    CHECK(write_hub(src, out) == Err::Busy && out.b.empty(), "a partition never quiet: busy, nothing sent yet");
    src.p[2].on_read = nullptr;
    src.p[1].m.resize(0x1000);
    CHECK(write_hub(src, out) == Err::Parts && out.b.empty(), "a partition not the size in the header: refused");
}

// For run.sh's check against tools/zhac_backup.py. export DIR: a file the firmware's code made from Hub(1), and its
// partitions. import FILE DIR: restore FILE onto Hub(9) as the hub would; exit 0 when it then holds DIR's partitions.
static std::vector<uint8_t> slurp(const std::string& p) {
    std::vector<uint8_t> v;
    if (FILE* f = fopen(p.c_str(), "rb")) {
        int c;
        while ((c = fgetc(f)) != EOF) v.push_back((uint8_t)c);
        fclose(f);
    }
    return v;
}

static int export_to(const std::string& dir) {
    Hub src(1);
    const std::vector<uint8_t> file = make_file(src);
    bool ok = !file.empty();
    const auto put = [&ok](const std::string& p, const std::vector<uint8_t>& v) {
        FILE* f = fopen(p.c_str(), "wb");
        ok = ok && f && fwrite(v.data(), 1, v.size(), f) == v.size();
        if (f) fclose(f);
    };
    put(dir + "/fw.zhacbak", file);
    for (int i = 0; i < 4; i++) put(dir + "/" + kPartNames[i] + ".bin", src.p[i].m);
    return ok ? 0 : 1;
}

static int import_from(const std::string& file, const std::string& dir) {
    const std::vector<uint8_t> f = slurp(file);
    const char* pw = getenv("ZHAC_BACKUP_PASSWORD");
    Hub dst(9);
    Fake slot(0x50000, 0x00);
    std::vector<uint8_t> tmp(kPiece);
    Stage st{};
    Header h{};
    // run.sh made it with --fw v2026111903 --nwk-fc 42 and the default host.
    if (f.size() < kHeaderLen || !parse(f.data(), &h) || strcmp(h.fw, "v2026111903") != 0 || strcmp(h.host, "zhac") != 0 ||
        h.nwk_fc != 42 || h.kdf_iter != kKdfIter) return 5;
    if (!pw || stage(f, slot, dst, pw, 1500, 99) != Err::None) return 2;
    if (apply(slot, Hub::find, &dst, tmp.data(), &st) != Applied::Done || st.fc_target != 99) return 3;
    for (int i = 0; i < 4; i++) if (dst.p[i].m != slurp(dir + "/" + kPartNames[i] + ".bin")) return 4;
    return 0;
}

int main(int argc, char** argv) {
    if (!crypto_init()) { printf("crypto_init failed\n"); return 99; }
    if (argc == 3 && strcmp(argv[1], "export") == 0) return export_to(argv[2]);
    if (argc == 4 && strcmp(argv[1], "import") == 0) return import_from(argv[2], argv[3]);
    if (argc != 1) { printf("usage: harness [export DIR | import FILE DIR]\n"); return 98; }
    test_header();
    test_stage();
    test_fc_target();
    test_versions();
    test_parts_match();
    test_kdf();
    test_gcm();
    test_sha();
    test_snapshot();
    test_round_trip();
    test_larger_target();
    test_refusals();
    test_power_cuts();
    test_boot_checks();
    test_stage_bounds();
    test_write_errors();
    printf("%s (%d failed)\n", s_fail ? "FAILED" : "all ok", s_fail);
    return s_fail;
}
