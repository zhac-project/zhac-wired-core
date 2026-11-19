// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Host check of the full backup (research 2026-11-18 C1): the file and stage headers (main/backup_format.h).
#include <cstdio>
#include <cstring>

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
    CHECK(fc_target(0, t0, t0 + 60, 5000) == 0, "no network in the backup: nothing to raise");
    CHECK(fc_target(1000, t0, t0 + 3600, 0) == 1000 + 100000 + 20 * 3600, "an hour later: +100 000 +20/s");
    CHECK(fc_target(1000, t0, t0 + 3600, 500000) == 600000, "this hub's own counter is higher: it +100 000");
    CHECK(fc_target(1000, 0, t0, 0) == 1000 + 100000 + 20u * 90 * 86400, "backup clock unknown: 90 days");
    CHECK(fc_target(1000, t0, 0, 0) == 1000 + 100000 + 20u * 90 * 86400, "hub clock unknown: 90 days");
    CHECK(fc_target(1000, t0, t0 - 10, 0) == 1000 + 100000 + 20u * 90 * 86400, "clock behind the backup: 90 days");
    CHECK(fc_target(1000, 1600000000, t0, 0) == 1000 + 100000 + 20u * 90 * 86400,
          "a backup stamped before the clock could be right (2020): 90 days");
    CHECK(fc_target(0xEFFFFFFF, t0, t0 + 1, 0) == 0xF0000000u, "capped at 0xF0000000");
    CHECK(fc_target(1000, t0, t0 + 400000000u, 0) == 0xF0000000u, "a very old backup: capped");
    CHECK(fc_target(1000, t0, t0 + 3600, 0xFFFFFFF0u) == 0xF0000000u, "a huge counter here: capped too");
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
    Header x = h; x.parts[2].size = 0x80000;
    CHECK(!parts_match(x, hub, 4, &why) && why && strcmp(why, "zhac_data") == 0, "a size differs: refused, named");
    x = h; strcpy(x.parts[0].name, "phy_init");
    CHECK(!parts_match(x, hub, 4, &why) && why && strcmp(why, "phy_init") == 0, "a partition this hub lacks: refused");
    x = h; strcpy(x.parts[3].name, "nvs"); x.parts[3].size = 0x10000;
    CHECK(!parts_match(x, hub, 4, &why) && why && strcmp(why, "nvs") == 0, "a partition twice: refused");
    x = h; x.n_parts = 3;
    CHECK(parts_match(x, hub, 4, &why), "fewer partitions: the others stay as they are");
}

int main() {
    test_header();
    test_stage();
    test_fc_target();
    test_versions();
    test_parts_match();
    printf("%s (%d failed)\n", s_fail ? "FAILED" : "all ok", s_fail);
    return s_fail;
}
