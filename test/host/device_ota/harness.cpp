// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Host check of the Zigbee device firmware update core (research 2026-11-18 C2): main/device_ota_core.h, the OTA
// file check, the Query Next Image read and the rules of one update.
#include <cstdio>
#include <cstring>
#include <vector>

#include "device_ota_core.h"

static int s_fail = 0;
#define CHECK(c, m) do { if (!(c)) { s_fail++; printf("  FAIL: %s\n", m); } else printf("  ok:   %s\n", m); } while (0)

using namespace devota;

static void put16(std::vector<uint8_t>& v, size_t at, uint16_t x) { v[at] = (uint8_t)x; v[at + 1] = (uint8_t)(x >> 8); }
static void put32(std::vector<uint8_t>& v, size_t at, uint32_t x) { for (int i = 0; i < 4; i++) v[at + i] = (uint8_t)(x >> (8 * i)); }

// `prefix` bytes, a header of `hlen` bytes, `payload` bytes, `tail` bytes; total image size `total` (0: hlen + payload).
static std::vector<uint8_t> ota_file(size_t prefix = 0, size_t payload = 100, size_t tail = 0, uint16_t hlen = 56,
                                     uint32_t total = 0, uint16_t hver = 0x0100) {
    std::vector<uint8_t> v(prefix + hlen + payload + tail, 0xA5);
    put32(v, prefix, kMagic);
    put16(v, prefix + 4, hver);
    put16(v, prefix + 6, hlen);
    put16(v, prefix + 8, 0);            // field control
    put16(v, prefix + 10, 0x117C);      // manufacturer
    put16(v, prefix + 12, 0x2101);      // image type
    put32(v, prefix + 14, 0x23086631);  // file version
    put16(v, prefix + 18, 2);           // stack version
    put32(v, prefix + 52, total ? total : (uint32_t)(hlen + payload));
    return v;
}

static void test_file() {
    Header h{};
    auto f = ota_file();
    CHECK(parse(f.data(), f.size(), &h) == FileErr::None && h.start == 0 && h.header_len == 56 && h.manuf == 0x117C &&
          h.image_type == 0x2101 && h.file_version == 0x23086631 && h.total == 156, "a plain file: its header");
    f = ota_file(3);
    CHECK(parse(f.data(), f.size(), &h) == FileErr::None && h.start == 3 && h.total == 156, "a vendor prefix before the magic");
    f = ota_file(0, 100, 40);
    CHECK(parse(f.data(), f.size(), &h) == FileErr::None && h.total == 156, "trailing bytes are not served");
    f = ota_file(0, 100, 0, 69);
    CHECK(parse(f.data(), f.size(), &h) == FileErr::None && h.header_len == 69 && h.total == 169, "a longer header");
    f = ota_file();
    put32(f, 0, 0x0BEEF11F);
    CHECK(parse(f.data(), f.size(), &h) == FileErr::NotOta, "no magic");
    f = ota_file(0, 100, 0, 56, 0, 0x0200);
    CHECK(parse(f.data(), f.size(), &h) == FileErr::NotOta, "another header version");
    f = ota_file(0, 100, 0, 55);
    CHECK(parse(f.data(), f.size(), &h) == FileErr::NotOta, "a header shorter than 56 bytes");
    f = ota_file(0, 100, 0, 56, 50);
    CHECK(parse(f.data(), f.size(), &h) == FileErr::NotOta, "a total size inside the header");
    f = ota_file(0, 100, 0, 56, 157);
    CHECK(parse(f.data(), f.size(), &h) == FileErr::NotOta, "a total size past the end of the file");
    f = ota_file(5, 100, 0, 56, 157);
    CHECK(parse(f.data(), f.size(), &h) == FileErr::NotOta, "a total size past the end, counted from the magic");
    std::vector<uint8_t> t(200, 0);
    put32(t, 190, kMagic);
    CHECK(parse(t.data(), t.size(), &h) == FileErr::NotOta, "a magic in the last bytes");
    CHECK(parse(t.data(), 0, &h) == FileErr::NotOta, "an empty file");
    f = ota_file(0, kMaxFile - 56);
    CHECK(parse(f.data(), f.size(), &h) == FileErr::None && h.total == kMaxFile, "a file of exactly the limit");
    f = ota_file(0, kMaxFile - 55);
    CHECK(parse(f.data(), f.size(), &h) == FileErr::TooLarge, "one byte over the limit");
}

// A Query Next Image request: frame control, sequence, command, then its payload (manufacturer 0x117C, image type
// 0x2101, version 0x23086631), and the hardware version when `hw`.
static std::vector<uint8_t> qni(uint8_t fc = 0x01, uint8_t cmd = 0x01, bool hw = false) {
    std::vector<uint8_t> v = {fc, 0x42, cmd, (uint8_t)(hw ? 1 : 0), 0x7C, 0x11, 0x01, 0x21, 0x31, 0x66, 0x08, 0x23};
    if (hw) { v.push_back(0x01); v.push_back(0x00); }
    return v;
}

static void test_query_frame() {
    Query q{};
    auto f = qni();
    CHECK(parse_query(f.data(), f.size(), &q) && q.manuf == 0x117C && q.image_type == 0x2101 && q.version == 0x23086631,
          "a Query Next Image request");
    f = qni(0x01, 0x01, true);
    CHECK(parse_query(f.data(), f.size(), &q), "one with the hardware version");
    f = qni(0x11);
    CHECK(parse_query(f.data(), f.size(), &q), "one that asks for no default response");
    f = qni();
    f.pop_back();
    CHECK(!parse_query(f.data(), f.size(), &q), "a cut-off request");
    f = qni(0x01, 0x03);
    CHECK(!parse_query(f.data(), f.size(), &q), "another command (Image Block Request)");
    f = qni(0x09);
    CHECK(!parse_query(f.data(), f.size(), &q), "server to client");
    f = qni(0x05);
    CHECK(!parse_query(f.data(), f.size(), &q), "a manufacturer-specific command");
    f = qni(0x00);
    CHECK(!parse_query(f.data(), f.size(), &q), "a profile-wide command");
    CHECK(!parse_query(f.data(), 2, &q), "two bytes");
}

static const uint64_t A = 0x00158D0001020304ull, B = 0x00158D0005060708ull;

static Header hdr() {
    Header h{};
    h.header_len = 56;
    h.manuf = 0x117C;
    h.image_type = 0x2101;
    h.file_version = 0x23086631;
    h.total = 1000;
    return h;
}

static Query qv(uint32_t version, uint16_t manuf = 0x117C, uint16_t type = 0x2101) {
    Query q{};
    q.manuf = manuf;
    q.image_type = type;
    q.version = version;
    return q;
}

static void test_update() {
    Session s;
    CHECK(s.state == State::Idle && !active(s) && pct(s) == 0, "idle at first");
    CHECK(!serves(s, 0), "idle: nobody is served, not even IEEE 0");
    CHECK(begin(s, A, hdr(), 77) && s.state == State::Waiting && s.ieee == A && s.since == 77 && s.h.total == 1000 &&
          active(s) && fits(s), "begin: waiting for the device");
    CHECK(!begin(s, B, hdr(), 99) && s.ieee == A && s.since == 77, "a second update while one runs is refused");
    CHECK(!on_start(s, B, qv(1), 0) && s.state == State::Waiting, "another device of the same model is not served");
    on_query(s, B, qv(5, 0x1234, 0x9999));
    CHECK(!s.has_query, "another device's query is not kept");
    on_query(s, A, qv(0x22000000, 0x117C, 0x2102));
    CHECK(s.has_query && s.dev.version == 0x22000000 && s.dev.image_type == 0x2102 && !fits(s),
          "the target asks for another image type: does not fit");
    on_query(s, A, qv(0x22000000, 0x1002));
    CHECK(!fits(s), "the target asks with another manufacturer: does not fit");
    on_query(s, A, qv(0x22000000));
    CHECK(fits(s), "the target asks for this image: fits");
    CHECK(on_start(s, A, qv(0x22000001), 0) && s.state == State::Sending && s.dev.version == 0x22000001, "the target is served");
    CHECK(serves(s, A) && !serves(s, B), "blocks and the end report are taken from the target only");
    CHECK(!on_block(s, 50, 0) && pct(s) == 5, "5 %");
    CHECK(on_block(s, 100, 0) && pct(s) == 10, "10 %: a new tenth");
    CHECK(!on_block(s, 150, 0) && pct(s) == 15, "15 %: the same tenth");
    CHECK(!on_block(s, 120, 0) && s.offset == 150, "an older block does not move the bar back");
    CHECK(on_block(s, 990, 0) && pct(s) == 99, "99 %");
    CHECK(on_start(s, A, qv(0x22000001), 0) && s.state == State::Sending && s.offset == 0, "the target asks again: starts over");
    on_block(s, 500, 0);
    on_end(s, 0);
    CHECK(s.state == State::Done && pct(s) == 100 && !active(s), "the device took the image");
    on_query(s, A, qv(0x23086631));
    CHECK(s.dev.version == 0x23086631, "after the end the device's new version is still shown");
    fail(s, "device_left");
    CHECK(s.state == State::Done, "a failure after the end changes nothing");
}

static void test_ends() {
    Session s;
    CHECK(!on_start(s, A, qv(1), 0) && s.state == State::Idle, "no update: nothing is served");
    on_query(s, A, qv(1));
    CHECK(!s.has_query, "no update: no query is kept");
    on_query(s, 0, qv(1));
    CHECK(!s.has_query, "no update: a query of a device the hub could not name (IEEE 0) is not kept");
    begin(s, A, hdr(), 1);
    CHECK(!on_start(s, A, qv(0x23086631), 0) && s.state == State::Done && !strcmp(s.reason, "same_version") && !active(s),
          "the device already runs this version");
    CHECK(begin(s, A, hdr(), 2) && s.state == State::Waiting && !s.has_query && s.reason[0] == 0, "a new update after an end");
    on_end(s, 0);
    CHECK(s.state == State::Waiting, "an end report before the device was served is ignored");
    on_start(s, A, qv(1), 0);
    on_end(s, 0x96);
    CHECK(s.state == State::Failed && s.device_status == 0x96 && !strcmp(s.reason, "device_status") && !active(s),
          "the device rejects the image");
    begin(s, A, hdr(), 3);
    on_start(s, A, qv(1), 0);
    on_block(s, 300, 0);
    fail(s, "device_left");
    CHECK(s.state == State::Failed && !strcmp(s.reason, "device_left") && !active(s), "a failure while sending");
    CHECK(!on_block(s, 500, 0) && s.offset == 300, "no progress after the end");
    begin(s, A, hdr(), 4);
    cancel(s);
    CHECK(s.state == State::Cancelled && !active(s), "cancel while waiting");
    begin(s, A, hdr(), 5);
    on_start(s, A, qv(1), 0);
    cancel(s);
    CHECK(s.state == State::Cancelled && !on_start(s, A, qv(1), 0), "cancel while sending; the device is not served again");
    CHECK(!serves(s, A), "after cancel the target gets no more blocks");
    Session idle;
    cancel(idle);
    CHECK(idle.state == State::Idle, "cancel with no update changes nothing");
    CHECK(!strcmp(state_name(State::Idle), "idle") && !strcmp(state_name(State::Waiting), "waiting") &&
          !strcmp(state_name(State::Sending), "sending") && !strcmp(state_name(State::Done), "done") &&
          !strcmp(state_name(State::Failed), "failed") && !strcmp(state_name(State::Cancelled), "cancelled"),
          "state names");
}

// The device's running version against the file's, judged on the target's own query as well as at START, so the answer
// does not depend on whether the stack offers an image it would not upgrade to (P6 review, 2026-11-19).
static void test_versions() {
    Session s;
    begin(s, A, hdr(), 1);
    on_query(s, A, qv(0x23086631));
    CHECK(s.state == State::Done && !strcmp(s.reason, "same_version"), "the target's query settles the same version");
    begin(s, A, hdr(), 2);
    on_query(s, A, qv(0x23090000));
    CHECK(s.state == State::Failed && !strcmp(s.reason, "device_newer") && !active(s),
          "no downgrade: the device runs a newer version and keeps it");
    begin(s, A, hdr(), 3);
    CHECK(!on_start(s, A, qv(0x23090000), 0) && !strcmp(s.reason, "device_newer"), "no downgrade at START either");
    begin(s, A, hdr(), 4);
    on_query(s, A, qv(0x23090000, 0x117C, 0x2102));
    CHECK(s.state == State::Waiting, "a query for another image type is not judged by its version");
    on_query(s, B, qv(0x23090000));
    CHECK(s.state == State::Waiting, "another device's newer version ends nothing");
    on_query(s, A, qv(0x23086630));
    CHECK(s.state == State::Waiting, "an older version waits for the download");
    on_start(s, A, qv(0x23086630), 0);
    on_query(s, A, qv(0x23086630));
    CHECK(s.state == State::Sending, "the target asking again while sending goes on");
    on_end(s, 0);
    on_query(s, A, qv(0x23086631));
    CHECK(s.state == State::Done && s.reason[0] == 0, "after the update its new version does not read as 'already ran it'");
    begin(s, A, hdr(), 5);
    cancel(s);
    on_query(s, A, qv(0x23090000));
    CHECK(s.state == State::Cancelled, "a query after a cancel changes nothing");
}

// A download that stops without an Upgrade End (the device lost power or range, or gave up) ends after kSilentMax
// with no block request; waiting has no limit (P6 review, 2026-11-19).
static void test_silent() {
    Session s;
    begin(s, A, hdr(), 0);
    CHECK(!silent(s, 100000) && s.state == State::Waiting, "waiting has no time limit: a sleepy device asks when it wakes");
    on_start(s, A, qv(1), 100);
    CHECK(!silent(s, 100 + kSilentMax - 1) && s.state == State::Sending, "sending: quiet for less than the limit");
    on_block(s, 64, 650);
    CHECK(!silent(s, 650 + kSilentMax - 1), "a block request restarts the clock");
    on_block(s, 64, 900);
    CHECK(!silent(s, 900 + kSilentMax - 1), "a repeated block request also shows the device is there");
    CHECK(silent(s, 900 + kSilentMax) && s.state == State::Failed && !strcmp(s.reason, "device_silent") && !active(s),
          "no block request for the limit: the update fails");
    CHECK(!silent(s, 900 + 2 * kSilentMax) && !strcmp(s.reason, "device_silent"), "an ended update is not failed again");
    CHECK(kSilentMax == 600, "the limit is 10 minutes");
}

int main() {
    test_file();
    test_query_frame();
    test_update();
    test_ends();
    test_versions();
    test_silent();
    printf("%s (%d failed)\n", s_fail ? "FAILED" : "all ok", s_fail);
    return s_fail;
}
