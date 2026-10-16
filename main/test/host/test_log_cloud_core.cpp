// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Host tests for log_cloud_core: the line filter, the level names, crc32 and the record ring
// (Task 2), the JSON builders and the names (Task 3).
#include "log_cloud_core.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace logcloud;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

namespace {
constexpr uint32_t kMagic = 0x5A4C5431;

struct Block {                      // a ring block like the firmware's: 4 KB, 8-byte aligned
    alignas(8) uint8_t bytes[4096];
};

std::vector<Line> lines_of(const Ring& r) {
    std::vector<Line> out;
    r.for_each([&](const Line& l) { out.push_back(l); });
    return out;
}

void test_crc32() {
    CHECK(crc32("123456789", 9) == 0xCBF43926u);
    CHECK(crc32("", 0) == 0u);
}

void test_levels() {
    Level l{};
    CHECK(parse_level("off", &l) && l == Level::Off);
    CHECK(parse_level("errors", &l) && l == Level::Errors);
    CHECK(parse_level("warnings", &l) && l == Level::Warnings);
    CHECK(parse_level("everything", &l) && l == Level::Everything);
    CHECK(!parse_level("Warnings", &l));
    CHECK(!parse_level("", &l));
    CHECK(!parse_level(nullptr, &l));
    CHECK(std::strcmp(level_name(Level::Warnings), "warnings") == 0);
    CHECK(std::strcmp(level_name(Level::Off), "off") == 0);
    CHECK(level_from_u8(2) == Level::Warnings);
    CHECK(level_from_u8(9) == Level::Off);
}

void test_wants() {
    CHECK(!wants(Level::Off, 'E', "zhc_adapter"));
    CHECK(wants(Level::Errors, 'E', "zhc_adapter"));
    CHECK(!wants(Level::Errors, 'W', "zhc_adapter"));
    CHECK(wants(Level::Warnings, 'W', "zhc_adapter"));
    CHECK(!wants(Level::Warnings, 'I', "zhc_adapter"));
    CHECK(wants(Level::Everything, 'I', "zhc_adapter"));
    CHECK(!wants(Level::Everything, 'D', "zhc_adapter"));
    for (const char* t : {"remote_client", "websocket_client", "transport_ws", "transport_base", "transport",
                          "esp-tls", "esp-tls-mbedtls", "log_cloud"}) {
        CHECK(!wants(Level::Everything, 'E', t));
    }
    CHECK(wants(Level::Everything, 'I', "cloud"));    // the remote-edit audit line stays
    CHECK(wants(Level::Everything, 'I', ""));
    CHECK(wants(Level::Everything, 'I', nullptr));
}

void test_ring_fifo() {
    Block b;
    Ring r(b.bytes, sizeof(b.bytes));
    r.format(kMagic, 77);
    CHECK(r.valid(kMagic));
    r.push(1, 1000, 'I', "zhac-wired", "alive");
    r.push(2, 2000, 'W', "event_bus", "queue full");
    r.push(3, 3000, 'E', "zhc_adapter", "decode_frame failed");
    CHECK(r.count() == 3);
    Line l{};
    CHECK(r.peek(&l) && l.seq == 1 && l.up_ms == 1000 && l.level == 'I');
    CHECK(std::strcmp(l.tag, "zhac-wired") == 0 && std::strcmp(l.msg, "alive") == 0);
    CHECK(!r.pop_if(2));                      // not the oldest
    CHECK(r.pop_if(1));
    CHECK(r.peek(&l) && l.seq == 2);
    CHECK(r.valid(kMagic));
    CHECK(r.hdr().run_id == 77);
}

void test_ring_drops_oldest() {
    alignas(8) uint8_t small[sizeof(Ring::Header) + 100];
    Ring r(small, sizeof(small));
    r.format(kMagic, 1);
    for (uint32_t i = 1; i <= 10; i++) r.push(i, i, 'I', "t", "0123456789");   // 16 + 1 + 10 = 27 bytes each
    CHECK(r.count() == 3);                    // 81 of 100 bytes
    CHECK(r.take_dropped() == 7);
    CHECK(r.take_dropped() == 0);
    Line l{};
    CHECK(r.peek(&l) && l.seq == 8);
    CHECK(r.valid(kMagic));
}

void test_ring_wraps() {
    alignas(8) uint8_t small[sizeof(Ring::Header) + 200];
    Ring r(small, sizeof(small));
    r.format(kMagic, 1);
    char msg[64];
    uint32_t next = 1;
    for (int round = 0; round < 50; round++) {
        std::snprintf(msg, sizeof(msg), "line %d with some text", round);
        r.push(next++, 100u + round, 'W', "wrap", msg);
        if (r.count() > 2) {                  // keep two, so records straddle the end of the block
            Line l{};
            CHECK(r.peek(&l));
            CHECK(r.pop_if(l.seq));
        }
    }
    const std::vector<Line> v = lines_of(r);
    CHECK(v.size() == 2);
    CHECK(v.size() == 2 && v[1].seq == 50 && std::strcmp(v[1].msg, "line 49 with some text") == 0);
    CHECK(r.valid(kMagic));
}

void test_ring_truncates_and_ignores_other_levels() {
    Block b;
    Ring r(b.bytes, sizeof(b.bytes));
    r.format(kMagic, 1);
    const std::string long_tag(50, 't'), long_msg(400, 'm');
    r.push(1, 1, 'I', long_tag.c_str(), long_msg.c_str());
    Line l{};
    CHECK(r.peek(&l));
    CHECK(std::strlen(l.tag) == kTagMax && std::strlen(l.msg) == kMsgMax);
    r.push(2, 2, 'D', "x", "debug lines are not kept");
    CHECK(r.count() == 1);
}

void test_ring_survives_a_copy_and_rejects_garbage() {
    Block b;
    Ring r(b.bytes, sizeof(b.bytes));
    r.format(kMagic, 42);
    r.push(5, 500, 'E', "zigbee", "about to crash");
    r.push(0, 600, 'I', "zhac-wired", "never queued");
    r.hdr().sent = 5;
    r.hdr().planned = 2;
    r.seal();

    Block copy = b;                           // what the next run finds in no-init RAM
    Ring again(copy.bytes, sizeof(copy.bytes));
    CHECK(again.valid(kMagic));
    CHECK(again.hdr().run_id == 42 && again.hdr().sent == 5 && again.hdr().planned == 2);
    const std::vector<Line> v = lines_of(again);
    CHECK(v.size() == 2 && v[0].seq == 5 && v[1].seq == 0 && std::strcmp(v[1].msg, "never queued") == 0);

    CHECK(!again.valid(kMagic + 1));          // another layout or owner
    Block garbage;
    std::memset(garbage.bytes, 0xA5, sizeof(garbage.bytes));        // RAM after power-on
    CHECK(!Ring(garbage.bytes, sizeof(garbage.bytes)).valid(kMagic));
    Block flipped = b;
    reinterpret_cast<Ring::Header*>(flipped.bytes)->used ^= 1;      // the header no longer matches its crc
    CHECK(!Ring(flipped.bytes, sizeof(flipped.bytes)).valid(kMagic));
    Block torn = b;                                                 // a record cut by the crash
    torn.bytes[sizeof(Ring::Header)] = 0xFF;
    torn.bytes[sizeof(Ring::Header) + 1] = 0xFF;
    CHECK(!Ring(torn.bytes, sizeof(torn.bytes)).valid(kMagic));
}

void test_ring_clear_keeps_identity() {
    Block b;
    Ring r(b.bytes, sizeof(b.bytes));
    r.format(kMagic, 9);
    r.push(1, 1, 'W', "a", "b");
    r.hdr().sent = 1;
    r.seal();
    r.clear();
    CHECK(r.count() == 0 && r.used() == 0 && r.hdr().run_id == 9 && r.hdr().sent == 1);
    CHECK(r.valid(kMagic));
    Line l{};
    CHECK(!r.peek(&l));
}
void test_ring_unsealed() {
    Block b;
    Ring r(b.bytes, sizeof(b.bytes), false);
    r.format(kMagic, 3);
    CHECK(r.hdr().crc == 0);                  // never computed
    r.push(1, 10, 'I', "a", "one");
    r.push(2, 20, 'E', "b", "two");
    CHECK(r.hdr().crc == 0 && r.count() == 2);
    Line l{};
    CHECK(r.peek(&l) && l.seq == 1 && std::strcmp(l.msg, "one") == 0);
    CHECK(r.pop_if(1));
    CHECK(r.peek(&l) && l.seq == 2 && r.count() == 1);
    CHECK(lines_of(r).size() == 1);
}

std::string as_string(const char* buf, size_t n) { return std::string(buf, n); }

void test_line_json() {
    char out[512];
    Line l{};
    l.seq = 7; l.up_ms = 1500; l.level = 'W';
    std::strcpy(l.tag, "event_bus");
    std::strcpy(l.msg, "queue full");
    CHECK(as_string(out, line_json(out, sizeof(out), l, false)) == "[1500,\"W\",\"event_bus\",\"queue full\"]");
    CHECK(as_string(out, line_json(out, sizeof(out), l, true)) == "[1500,\"W\",\"event_bus\",\"queue full\",7]");
    std::strcpy(l.msg, "say \"hi\"\tnow\\\x01");
    CHECK(as_string(out, line_json(out, sizeof(out), l, false)) ==
          "[1500,\"W\",\"event_bus\",\"say \\\"hi\\\"\\tnow\\\\\\u0001\"]");
    CHECK(line_json(out, 10, l, false) == 0);              // no room: nothing usable
}

void batch_repairs_cut_utf8() {                            // Review Focus 1
    char out[256];
    Line l{};
    l.seq = 1; l.up_ms = 1; l.level = 'I';
    std::strcpy(l.tag, "lua");
    std::strcpy(l.msg, "\xD0\x9F\xD1\x80\xD0");            // "Пр" then half of "и": the hook's 191-byte cut
    CHECK(as_string(out, line_json(out, sizeof(out), l, false)) == "[1,\"I\",\"lua\",\"\xD0\x9F\xD1\x80\\ufffd\"]");
}

void test_batch_writer() {
    char buf[BatchWriter::kBudget];
    BatchWriter bw(buf, sizeof(buf));
    CHECK(bw.finish() == 0);                               // never begun: nothing to send
    bw.begin(7, 3, 9000, 0, 1);
    Line a{}, b{};
    a.seq = 3; a.up_ms = 1; a.level = 'I'; std::strcpy(a.tag, "a"); std::strcpy(a.msg, "x");
    b.seq = 4; b.up_ms = 2; b.level = 'E'; std::strcpy(b.tag, "b"); std::strcpy(b.msg, "y");
    CHECK(bw.add(a) && bw.add(b) && bw.lines() == 2);
    const size_t n = bw.finish();
    CHECK(as_string(buf, n) ==
          "{\"boot\":7,\"seq\":3,\"up\":9000,\"clock\":0,\"dropped\":1,\"lines\":[[1,\"I\",\"a\",\"x\"],[2,\"E\",\"b\",\"y\"]]}");

    bw.begin(7, 10, 9000, 1791300000000, 0);
    Line big{};
    big.level = 'W'; std::strcpy(big.tag, "zhc_adapter");
    std::memset(big.msg, 'm', kMsgMax); big.msg[kMsgMax] = '\0';
    size_t added = 0;
    for (uint32_t i = 0; i < 100; i++) { big.seq = 10 + i; if (!bw.add(big)) break; added++; }
    CHECK(added > 20 && added < 100);                      // about 28 lines of ~215 bytes fit in 6144
    const size_t m = bw.finish();
    CHECK(m > 0 && m <= BatchWriter::kBudget);
    CHECK(as_string(buf, m).substr(m - 2) == "]}");
    CHECK(bw.lines() == added);
}

void test_names() {
    CHECK(std::strcmp(reset_name(Reset::PowerOn), "power_on") == 0);
    CHECK(std::strcmp(reset_name(Reset::Panic), "panic") == 0);
    CHECK(std::strcmp(reset_name(Reset::IntWdt), "int_wdt") == 0);
    CHECK(std::strcmp(reset_name(Reset::TaskWdt), "task_wdt") == 0);
    CHECK(std::strcmp(reset_name(Reset::Wdt), "wdt") == 0);
    CHECK(std::strcmp(reset_name(Reset::Brownout), "brownout") == 0);
    CHECK(std::strcmp(reset_name(Reset::Sw), "sw") == 0);
    CHECK(std::strcmp(reset_name(Reset::Ext), "ext") == 0);
    CHECK(std::strcmp(reset_name(Reset::Other), "other") == 0);
    CHECK(planned_name(0) == nullptr);
    CHECK(std::strcmp(planned_name(1), "update") == 0);
    CHECK(std::strcmp(planned_name(2), "restart") == 0);
    CHECK(std::strcmp(planned_name(3), "rollback") == 0);
    CHECK(planned_name(9) == nullptr);
    CHECK(std::strcmp(riscv_cause_name(5), "load access fault") == 0);
    CHECK(std::strcmp(riscv_cause_name(7), "store access fault") == 0);
    CHECK(std::strcmp(riscv_cause_name(0x80000007u), "interrupt") == 0);
    CHECK(std::strcmp(riscv_cause_name(20), "unknown cause") == 0);
    char out[200];
    const size_t n = crash_json(out, sizeof(out), "zigbee", 0x4200abcdu, "load access fault", "3f2a9c1b9");
    CHECK(as_string(out, n) == "{\"task\":\"zigbee\",\"pc\":\"0x4200abcd\",\"cause\":\"load access fault\",\"elf\":\"3f2a9c1b9\"}");
    CHECK(std::strlen(out) == n);                          // usable as a C string
    CHECK(crash_json(out, 20, "zigbee", 1, "x", "y") == 0 && out[0] == '\0');
}

void test_tail_json() {
    char out[BatchWriter::kBudget];
    TailHead h{};
    h.boot = 2345678; h.prev_boot = 0; h.up_ms = 9000; h.clock_ms = 0;
    h.reason = "power_on"; h.planned = nullptr; h.prev_clock_base_ms = 0; h.prev_sent = 0; h.crash_json = nullptr;
    CHECK(as_string(out, tail_json(out, sizeof(out), h, nullptr)) ==
          "{\"boot\":2345678,\"prev_boot\":0,\"up\":9000,\"clock\":0,\"reason\":\"power_on\",\"planned\":null,"
          "\"prev_clock_base\":0,\"prev_sent\":0,\"crash\":null,\"lines\":[]}");

    // Lines the cloud already has (1..prev_sent) stay out; never-queued (0) and unsent ones go.
    Block b;
    Ring prev(b.bytes, sizeof(b.bytes));
    prev.format(kMagic, 1234567);
    prev.push(3, 100, 'I', "a", "sent before");
    prev.push(4, 200, 'W', "b", "sent too");
    prev.push(5, 300, 'E', "c", "still in the backlog");
    prev.push(0, 400, 'I', "d", "below the level");
    h.reason = "panic"; h.planned = "restart"; h.prev_boot = 1234567; h.prev_sent = 4; h.prev_clock_base_ms = 1791290000000;
    h.crash_json = "{\"task\":\"zigbee\"}";
    CHECK(as_string(out, tail_json(out, sizeof(out), h, &prev)) ==
          "{\"boot\":2345678,\"prev_boot\":1234567,\"up\":9000,\"clock\":0,\"reason\":\"panic\",\"planned\":\"restart\","
          "\"prev_clock_base\":1791290000000,\"prev_sent\":4,\"crash\":{\"task\":\"zigbee\"},"
          "\"lines\":[[300,\"E\",\"c\",\"still in the backlog\",5],[400,\"I\",\"d\",\"below the level\",0]]}");

    // More than fits: the newest lines win, the result stays within the budget.
    Block b2;
    Ring many(b2.bytes, sizeof(b2.bytes));
    many.format(kMagic, 1);
    const std::string ctrl(150, '\x01');                   // 900 bytes once escaped
    for (uint32_t s = 101; s <= 115; s++) many.push(s, s, 'I', "t", ctrl.c_str());
    h.prev_sent = 0; h.crash_json = nullptr;
    const size_t n = tail_json(out, sizeof(out), h, &many);
    const std::string got = as_string(out, n);
    CHECK(n > 0 && n <= BatchWriter::kBudget);
    CHECK(got.find(",115]") != std::string::npos);
    CHECK(got.find(",101]") == std::string::npos);
    CHECK(got.substr(n - 2) == "]}");
}

}  // namespace

int main() {
    test_crc32();
    test_levels();
    test_wants();
    test_ring_fifo();
    test_ring_drops_oldest();
    test_ring_wraps();
    test_ring_truncates_and_ignores_other_levels();
    test_ring_survives_a_copy_and_rejects_garbage();
    test_ring_clear_keeps_identity();
    test_ring_unsealed();
    test_line_json();
    batch_repairs_cut_utf8();
    test_batch_writer();
    test_names();
    test_tail_json();
    if (g_fail) { std::fprintf(stderr, "log_cloud_core_tests: %d failed\n", g_fail); return 1; }
    std::printf("log_cloud_core_tests: ok\n");
    return 0;
}
