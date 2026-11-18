// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The coordinator address pin read straight from NVS partition bytes, for a
// partition NVS cannot mount (P1 review 2026-11-19 #2). The fixture is a real
// image from IDF's nvs_partition_gen.py (fixtures/gen_nvs_pin.sh).
#include "../../nvs_raw.h"

#include <cstdio>
#include <cstring>
#include <vector>

static int g_fail = 0;
static void expect(const char* what, bool ok) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) g_fail++;
}

struct Image { std::vector<uint8_t> b; bool fail = false; };
static bool read_image(void* ctx, size_t off, void* dst, size_t n) {
    auto* im = static_cast<Image*>(ctx);
    if (im->fail || off + n > im->b.size()) return false;
    std::memcpy(dst, im->b.data() + off, n);
    return true;
}
static bool find(Image& im, const char* ns, const char* key, uint64_t* v) {
    static uint8_t page[nvs_raw::kPage];
    *v = 0;
    return nvs_raw::find_u64(read_image, &im, im.b.size(), page, ns, key, v);
}

int main() {
    Image im;
    if (FILE* f = std::fopen(NVS_PIN_BIN, "rb")) {
        im.b.resize(16384);
        im.b.resize(std::fread(im.b.data(), 1, im.b.size(), f));
        std::fclose(f);
    }
    if (im.b.size() != 16384) { std::printf("FAIL fixture %s not read\n", NVS_PIN_BIN); return 1; }
    const std::vector<uint8_t> clean = im.b;
    // The pin's entry: the second "ieee" key in the image (the first is the decoy in "other", page 0).
    const uint8_t want_key[16] = {'i', 'e', 'e', 'e'};
    size_t key_at = 0;
    for (int seen = 0; key_at + 16 <= im.b.size(); key_at++)
        if (!std::memcmp(&im.b[key_at], want_key, 16) && ++seen == 2) break;
    const size_t entry = key_at - 8, page_at = entry / 4096 * 4096;
    const size_t index = (entry - page_at - 64) / 32;

    uint64_t v = 0;
    expect("the pin, on page 2 behind a multi-entry string",
           find(im, "zb_coord", "ieee", &v) && v == 0x00124B0012345678ull);
    expect("the same key in another namespace is that namespace's",
           find(im, "other", "ieee", &v) && v == 0x1111111111111111ull);
    expect("absent key: not found", !find(im, "zb_coord", "nope", &v));
    expect("absent namespace: not found", !find(im, "nons", "ieee", &v));

    im.b[page_at + 32 + index / 4] &= static_cast<uint8_t>(~(3u << (index % 4 * 2)));   // state 00: erased
    expect("an erased entry is not the pin", !find(im, "zb_coord", "ieee", &v));
    im.b = clean;
    im.b[entry + 24] ^= 0x01;
    expect("an entry whose crc does not match is not the pin", !find(im, "zb_coord", "ieee", &v));
    im.b = clean;
    const uint32_t freeing = 0xFFFFFFF8u, corrupt = 0xFFFFFFF0u;
    std::memcpy(&im.b[page_at], &freeing, 4);
    expect("a page being freed is still read", find(im, "zb_coord", "ieee", &v) && v == 0x00124B0012345678ull);
    std::memcpy(&im.b[page_at], &corrupt, 4);
    expect("a corrupt page is not read", !find(im, "zb_coord", "ieee", &v));
    im.b = clean;
    im.fail = true;
    expect("a read error: not found", !find(im, "zb_coord", "ieee", &v));

    std::printf("%s (%d failed)\n", g_fail ? "FAILED" : "passed", g_fail);
    return g_fail;
}
