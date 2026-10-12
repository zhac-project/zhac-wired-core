// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The network map walk (esp_zb_netmap_walk) against scripted meshes: paging,
// discovery, retries, filters, the pool fallback, caps and loop guards.

#include "../../esp_zb_netmap_json.h"
#include "../../esp_zb_netmap_walk.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace zhc_zb::netmap;

static int g_fail = 0;

static void expect(const char* what, bool ok) {
    if (!ok) { g_fail++; std::printf("FAIL %s\n", what); }
    else     { std::printf("ok   %s\n", what); }
}

constexpr uint64_t kPan = 0xDDDDDDDDDDDDDDDDull;
constexpr uint64_t C  = 0x00124B0000000001ull;   // coordinator
constexpr uint64_t RA = 0xA1A1A1A1A1A1A1A1ull, RB = 0xB2B2B2B2B2B2B2B2ull, RC = 0xC3C3C3C3C3C3C3C3ull;
constexpr uint64_t RD = 0xD4D4D4D4D4D4D4D4ull;   // a router that never answers
constexpr uint64_t RP = 0xE5E5E5E5E5E5E5E5ull;   // a router only the pool knows
constexpr uint64_t RQ = 0xF6F6F6F6F6F6F6F6ull;   // a pool-only router that is off
constexpr uint64_t E1 = 0x1111, E2 = 0x2222, E3 = 0x3333, E4 = 0x4444, E5 = 0x5555, EZ = 0x9999;

static Neighbor nb(uint64_t ieee, uint16_t nwk, uint8_t type, uint8_t rel, uint8_t lqi, uint64_t pan = kPan) {
    return Neighbor{pan, ieee, nwk, type, rel, 1, lqi};
}

static Known known(uint64_t ieee, uint16_t nwk, uint8_t type, const char* name) {
    Known k{};
    k.ieee = ieee; k.nwk = nwk; k.type = type; k.last_seen = 1760000000;
    std::snprintf(k.name, sizeof(k.name), "%s", name);
    return k;
}

// A scripted mesh, keyed by short address.
struct Mesh {
    std::map<uint16_t, std::vector<Neighbor>> tables;
    std::set<uint16_t> dead;          // never answer
    std::set<uint16_t> flaky;         // fail the first try of every page
    std::set<uint16_t> ignore_start;  // answer page 0 whatever was asked
    size_t page = 3;                  // records per answer
    std::vector<Ask> asks;
};

static void run(Walk& w, Mesh& m) {
    std::set<std::pair<uint16_t, uint8_t>> flaked;
    Ask a{};
    int guard = 0;
    while (w.next_ask(&a) && ++guard < 20000) {
        m.asks.push_back(a);
        if (m.dead.count(a.nwk) || !m.tables.count(a.nwk)) { w.on_fail(a); continue; }
        if (m.flaky.count(a.nwk) && flaked.insert({a.nwk, a.start_index}).second) { w.on_fail(a); continue; }
        const auto& t = m.tables[a.nwk];
        const size_t start = m.ignore_start.count(a.nwk) ? 0 : a.start_index;
        const size_t n = start < t.size() ? std::min(m.page, t.size() - start) : 0;
        w.on_page(a, static_cast<uint8_t>(std::min<size_t>(t.size(), 255)), static_cast<uint8_t>(start),
                  t.data() + start, n);
    }
    expect("walk ends", guard < 20000);
}

static size_t asks_to(const Mesh& m, uint16_t nwk) {
    return static_cast<size_t>(std::count_if(m.asks.begin(), m.asks.end(),
                                             [&](const Ask& a) { return a.nwk == nwk; }));
}

static size_t links_between(const Walk& w, uint64_t src, uint64_t dst) {
    size_t n = 0;
    for (size_t i = 0; i < w.link_count(); ++i)
        if (w.links()[i].src == src && w.links()[i].dst == dst) ++n;
    return n;
}

static const Node* node(const Walk& w, uint64_t ieee) {
    for (size_t i = 0; i < w.node_count(); ++i)
        if (w.nodes()[i].ieee == ieee) return &w.nodes()[i];
    return nullptr;
}

static Walk w;   // ~30 KB: static, reused through begin()

static void test_bfs_paging_dedupe() {
    Mesh m;
    m.tables[0xA] = {nb(C, 0, 0, 0, 210), nb(RB, 0xB, 1, 2, 120), nb(RC, 0xC, 1, 2, 175),
                     nb(E2, 0x12, 2, 1, 168), nb(RB, 0xB, 1, 2, 120), nb(E3, 0x13, 2, 1, 92),
                     nb(E4, 0x14, 2, 1, 61)};                       // 7 records, RB twice
    m.tables[0xB] = {nb(C, 0, 0, 0, 198), nb(RA, 0xA, 1, 2, 112)};
    m.tables[0xC] = {nb(RA, 0xA, 1, 2, 160), nb(E5, 0x15, 2, 1, 155)};
    w.begin(C, kPan, nullptr, 0);
    const Neighbor local[] = {nb(RA, 0xA, 1, 1, 212, 0), nb(RB, 0xB, 1, 1, 186, 0), nb(E1, 0x11, 2, 1, 90, 0)};
    w.add_local(local, 3);
    run(w, m);
    expect("router A paged 3 times (0, 3, 6)", asks_to(m, 0xA) == 3 && m.asks[0].start_index == 0 &&
                                                m.asks[1].start_index == 3 && m.asks[2].start_index == 6);
    expect("router C found through A and asked", asks_to(m, 0xC) == 1);
    expect("9 nodes", w.node_count() == 9);
    expect("13 links (duplicate record dropped)", w.link_count() == 13);
    expect("A->B once", links_between(w, RA, RB) == 1);
    expect("coordinator links from the local table", links_between(w, C, RA) == 1 && links_between(w, C, E1) == 1);
    expect("all routers done", w.routers_done() == 3 && w.routers_total() == 3);
    expect("nothing truncated", !w.truncated());
}

static void test_filters() {
    Mesh m;
    m.tables[0xA] = {nb(C, 0, 0, 0, 200), nb(0x77, 0x77, 2, 1, 80, 0)};   // ext_pan 0 in a reply: ours
    w.begin(C, kPan, nullptr, 0);
    const Neighbor local[] = {nb(RA, 0xA, 1, 1, 200, 0),
                              nb(0xF1, 0x21, 2, 1, 100, 0x1234),            // another network
                              nb(0, 0x22, 2, 1, 100, 0),                    // no IEEE
                              nb(UINT64_MAX, 0x23, 2, 1, 100, 0),           // no IEEE
                              nb(0x99, 0x24, 2, 1, 70, 0)};                 // on our PAN, unknown to the pool
    w.add_local(local, 5);
    run(w, m);
    expect("foreign PAN dropped", !w.seen(0xF1));
    expect("zero / all-FF IEEE dropped", !w.seen(0) && !w.seen(UINT64_MAX));
    expect("unknown device on our PAN kept", w.seen(0x99) && !w.find_known(0x99));
    expect("ext_pan 0 in a reply accepted", w.seen(0x77));
}

static void test_retry_and_dead_router() {
    Mesh m;
    m.tables[0xA] = {nb(C, 0, 0, 0, 200), nb(E2, 0x12, 2, 1, 150), nb(E3, 0x13, 2, 1, 150), nb(E4, 0x14, 2, 1, 150)};
    m.tables[0xB] = {nb(C, 0, 0, 0, 200)};
    m.flaky = {0xA};
    m.dead  = {0xD};
    w.begin(C, kPan, nullptr, 0);
    const Neighbor local[] = {nb(RA, 0xA, 1, 1, 200, 0), nb(RD, 0xD, 1, 1, 48, 0), nb(RB, 0xB, 1, 1, 180, 0)};
    w.add_local(local, 3);
    run(w, m);
    expect("flaky router: every page retried once and read", asks_to(m, 0xA) == 4 && w.seen(E4));
    expect("dead router asked twice", asks_to(m, 0xD) == 2);
    const Node* d = node(w, RD);
    expect("dead router marked failed, link kept", d && d->failed && links_between(w, C, RD) == 1);
    expect("scan went on after it", asks_to(m, 0xB) == 1);
}

static void test_pool_fallback_and_not_seen() {
    Mesh m;
    m.tables[0xA] = {nb(C, 0, 0, 0, 200)};
    m.tables[0xE] = {nb(RA, 0xA, 1, 2, 90)};
    Known pool[] = {known(RA, 0xA, 1, "Kitchen plug"), known(RP, 0xE, 1, "Garage plug"),
                    known(RQ, 0xF, 1, "Shed plug"), known(EZ, 0x99, 2, "Balcony temp")};
    w.begin(C, kPan, pool, 4);
    const Neighbor local[] = {nb(RA, 0xA, 1, 1, 200, 0)};
    w.add_local(local, 1);
    run(w, m);
    expect("pool-only router asked after the tables", asks_to(m, 0xE) == 1 && m.asks.back().nwk == 0xF);
    const Node* p = node(w, RP);
    expect("pool-only router that answered is on the map", p && !p->failed && links_between(w, RP, RA) == 1);
    expect("pool-only router that is off stays not seen", asks_to(m, 0xF) == 2 && !w.seen(RQ));
    expect("end device nobody lists is not seen", !w.seen(EZ));
    expect("listed device is seen", w.seen(RA));
}

static void test_rejoin_takes_table_nwk() {
    Mesh m;
    m.tables[0x2222] = {nb(C, 0, 0, 0, 200)};
    Known pool[] = {known(RA, 0x1111, 1, "Kitchen plug")};   // pool still has the old address
    w.begin(C, kPan, pool, 1);
    const Neighbor local[] = {nb(RA, 0x2222, 1, 1, 200, 0)};
    w.add_local(local, 1);
    run(w, m);
    expect("asked at the address the table gave", asks_to(m, 0x2222) == 1 && asks_to(m, 0x1111) == 0);
}

static void test_loop_guards() {
    Mesh m;
    m.tables[0xA] = {nb(C, 0, 0, 0, 200), nb(E2, 0x12, 2, 1, 150), nb(E3, 0x13, 2, 1, 150),
                     nb(E4, 0x14, 2, 1, 150), nb(E5, 0x15, 2, 1, 150)};
    m.ignore_start = {0xA};
    std::vector<Neighbor> big;
    for (uint64_t i = 0; i < 200; ++i) big.push_back(nb(0x100000 + i, static_cast<uint16_t>(0x1000 + i), 2, 1, 120));
    m.tables[0xB] = big;
    w.begin(C, kPan, nullptr, 0);
    const Neighbor local[] = {nb(RA, 0xA, 1, 1, 200, 0), nb(RB, 0xB, 1, 1, 200, 0)};
    w.add_local(local, 2);
    run(w, m);
    expect("router answering page 0 forever: dropped after the mismatch", asks_to(m, 0xA) == 2);
    expect("huge table: stops at the page cap", asks_to(m, 0xB) == kMaxPages);
}

static void test_caps() {
    Mesh m;   // no tables: every router fails
    std::vector<Neighbor> local;
    for (uint64_t i = 0; i < 300; ++i) local.push_back(nb(0x200000 + i, static_cast<uint16_t>(0x2000 + i), 1, 1, 100, 0));
    w.begin(C, kPan, nullptr, 0);
    w.add_local(local.data(), local.size());
    run(w, m);
    expect("node cap holds", w.node_count() == kMaxNodes);
    expect("router queue cap holds", w.routers_total() == kMaxRouters);
    expect("truncated set", w.truncated());
    expect("links never past the cap", w.link_count() <= kMaxLinks);
}

static std::string to_json(const Walk& walk, uint32_t ts, uint32_t dur) {
    JsonDocument doc;
    build_json(doc, walk, ts, dur);
    std::string s;
    serializeJson(doc, s);
    return s;
}

static void test_json_shape() {
    Mesh m;
    m.tables[0xA] = {nb(C, 0, 0, 0, 198), nb(E2, 0x12, 2, 4, 92), nb(E3, 0x13, 3, 7, 70)};
    m.dead = {0xD};
    Known pool[] = {known(RA, 0xA, 1, "Kitchen \"x\" \\ Кухня"), known(EZ, 0x99, 2, "Balcony temp")};
    std::snprintf(pool[0].model, sizeof(pool[0].model), "%s", "TS011F");
    w.begin(C, kPan, pool, 2);
    const Neighbor local[] = {nb(RA, 0xA, 1, 1, 212, 0), nb(RD, 0xD, 1, 1, 48, 0)};
    w.add_local(local, 2);
    run(w, m);

    const std::string s = to_json(w, 1760300000, 8400);
    JsonDocument back;
    expect("map JSON parses", deserializeJson(back, s) == DeserializationError::Ok);
    expect("ts / duration / truncated", back["ts"] == 1760300000 && back["duration_ms"] == 8400 &&
                                        back["truncated"] == false);
    JsonArray nodes = back["nodes"].as<JsonArray>();
    expect("coordinator first, named Hub", nodes[0]["type"] == "coordinator" && nodes[0]["name"] == "Hub" &&
                                           nodes[0]["ieee"] == "0x00124B0000000001" && nodes[0]["known"] == true);
    bool name_ok = false, model_ok = false, failed_ok = false, unknown_ok = false;
    for (JsonObject n : nodes) {
        if (n["ieee"] == "0xA1A1A1A1A1A1A1A1") {
            name_ok  = n["name"] == "Kitchen \"x\" \\ Кухня";
            model_ok = n["model"] == "TS011F" && n["type"] == "router" && n["nwk"] == 0xA;
        }
        if (n["ieee"] == "0xD4D4D4D4D4D4D4D4") failed_ok = n["failed"] == true && n["known"] == false;
        if (n["ieee"] == "0x0000000000003333") unknown_ok = n["type"] == "unknown";
    }
    expect("name with quote, backslash, Cyrillic round-trips", name_ok);
    expect("model, type, nwk", model_ok);
    expect("dead router failed, not known to the pool", failed_ok);
    expect("device_type 3 -> unknown", unknown_ok);
    bool prev_child = false, bad_rel_none = false, child = false;
    for (JsonObject l : back["links"].as<JsonArray>()) {
        if (l["src"] == "0x00124B0000000001" && l["dst"] == "0xA1A1A1A1A1A1A1A1") child = l["rel"] == "child" && l["lqi"] == 212;
        if (l["dst"] == "0x0000000000002222") prev_child = l["rel"] == "prev_child";
        if (l["dst"] == "0x0000000000003333") bad_rel_none = l["rel"] == "none";
    }
    expect("rel child + lqi", child);
    expect("relationship 4 -> prev_child", prev_child);
    expect("relationship 7 -> none", bad_rel_none);
    JsonArray ns = back["not_seen"].as<JsonArray>();
    expect("not_seen lists the unlisted pool device", ns.size() == 1 && ns[0]["ieee"] == "0x0000000000009999" &&
                                                      ns[0]["name"] == "Balcony temp" && ns[0]["last_seen"] == 1760000000);
}

static void test_json_empty_mesh() {
    w.begin(C, kPan, nullptr, 0);
    Mesh m;
    run(w, m);
    JsonDocument back;
    deserializeJson(back, to_json(w, 0, 3));
    expect("coordinator alone: one node, no links, nothing unseen",
           back["nodes"].size() == 1 && back["links"].size() == 0 && back["not_seen"].size() == 0 && back["ts"] == 0);
}

int main() {
    test_bfs_paging_dedupe();
    test_filters();
    test_retry_and_dead_router();
    test_pool_fallback_and_not_seen();
    test_rejoin_takes_table_nwk();
    test_loop_guards();
    test_caps();
    test_json_shape();
    test_json_empty_mesh();
    if (g_fail) { std::printf("\n%d FAILURE(S)\n", g_fail); return 1; }
    std::printf("\nnetmap walk OK\n");
    return 0;
}
