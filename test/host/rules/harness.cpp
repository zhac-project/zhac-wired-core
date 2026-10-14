// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Host check of the wired hub's rule ops: escaped error replies, paged rule.list, rule.create's id,
// rule.update's base fingerprint, rule.check, and the rule.added / rule.updated / rule.deleted pushes. Runs the
// REAL code cut out of main/ws_bridge.cpp (see run.sh) against a fake rule store and scripted simple_rules
// results. Every reply and push is checked as strict JSON and strict UTF-8 by code that is NOT the hub's
// JsonWriter / utf8_seq. Exit code = number of mismatches.
#include "stubs.h"
#include "event_bus.h"   // RULE_CHANGE_* (zhac-components)
#include "expect.h"   // TXT_CYR and FP_CYR (its sha256sum), TXT_PLAIN and FP_PLAIN: written by run.sh

#include <atomic>
#include <chrono>
#include <mutex>
#include <openssl/sha.h>
#include <string>
#include <thread>
#include <vector>

// wrappers around the static originals (slice_tu.cpp)
void   h_send_err(int fd, uint32_t id, const char* err);
void   h_reply_ok_or_err(int fd, uint32_t id, bool ok, const char* err);
bool   h_sha256_hex(const void* data, size_t len, char out[65]);
void   h_rule_list(int fd, uint32_t id, JsonDocument& doc);
void   h_rule_create(int fd, uint32_t id, JsonDocument& doc);
void   h_rule_update(int fd, uint32_t id, JsonDocument& doc);
void   h_rule_check(int fd, uint32_t id, JsonDocument& doc);
size_t h_page_budget();
void   h_rule_changed(uint16_t id, uint8_t change);

static int g_bad = 0;
#define CHECK(cond, ...)                                                                   \
    do {                                                                                   \
        if (!(cond)) {                                                                     \
            g_bad++;                                                                       \
            printf("MISMATCH %s:%d: %s -- ", __FILE__, __LINE__, #cond);                   \
            printf(__VA_ARGS__);                                                           \
            printf("\n");                                                                  \
        }                                                                                  \
    } while (0)

// ---- the fakes ----------------------------------------------------------------------------------------
static int g_alloc_calls = 0, g_alloc_fail_at = 0;
void* heap_caps_malloc(size_t size, uint32_t) {
    if (g_alloc_fail_at && ++g_alloc_calls == g_alloc_fail_at) return nullptr;
    return malloc(size);
}
void heap_caps_free(void* p) { free(p); }

static std::vector<std::string> g_replies;
static std::vector<int> g_reply_fds;
static std::mutex g_reply_mtx;   // two editors reply from two threads in test_concurrent_base
void ws_server_reply(int fd, const char* buf, size_t len) {
    std::lock_guard<std::mutex> l(g_reply_mtx);
    g_replies.emplace_back(buf, len);
    g_reply_fds.push_back(fd);
}

static std::vector<RuleSlot> g_store;   // in the order the NVS iteration would give them: arbitrary
uint16_t rule_store_load_all(RuleSlot* out, uint16_t max_count) {
    uint16_t n = 0;
    for (const RuleSlot& s : g_store) {
        if (n >= max_count) break;
        out[n++] = s;
    }
    return n;
}
// test_concurrent_base: the first editor reads the stored text, then waits here (up to 200 ms) for the
// second editor to finish -- the window between a compare and its write.
static std::atomic<int>  g_park_next_load{0};
static std::atomic<bool> g_second_done{false};
bool rule_store_load(uint16_t rule_id, RuleSlot* out) {
    bool found = false;
    for (const RuleSlot& s : g_store)
        if (s.rule_id == rule_id) { *out = s; found = true; break; }
    if (g_park_next_load.exchange(0) == 1)
        for (int i = 0; i < 40 && !g_second_done; i++) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return found;
}

static bool g_add_ok = true, g_upd_ok = true, g_check_ok = true;
static std::string g_last_err, g_check_msg;
static uint16_t g_new_id = 0;
struct Upd { uint16_t id; std::string name, dsl; };
static std::vector<Upd> g_upd;
static int g_add_calls = 0, g_check_calls = 0;
bool simple_rules_add(const char*, const char*, uint16_t* out) {
    g_add_calls++;
    if (!g_add_ok) return false;
    *out = g_new_id;
    return true;
}
bool simple_rules_update(uint16_t id, const char* name, const char* dsl) {
    if (!g_upd_ok) return false;
    g_upd.push_back({id, name, dsl});
    for (RuleSlot& s : g_store) {   // a real save rewrites the stored text
        if (s.rule_id != id) continue;
        memset(s.src, 0, sizeof(s.src));
        s.src_len = (uint16_t)strlen(dsl);
        memcpy(s.src, dsl, s.src_len);
    }
    return true;
}
bool simple_rules_check(const char*, char* err, size_t cap) {
    g_check_calls++;
    if (err && cap) err[0] = '\0';
    if (!g_check_ok && err && cap) snprintf(err, cap, "%s", g_check_msg.c_str());   // as the real one does
    return g_check_ok;
}
const char* dsl_last_error() { return g_last_err.c_str(); }

static bool g_psa_fail = false;
static size_t g_psa_olen = 32;
psa_status_t psa_hash_compute(uint32_t alg, const uint8_t* in, size_t n, uint8_t* hash, size_t hs, size_t* hl) {
    if (g_psa_fail) return -1;
    if (alg != PSA_ALG_SHA_256 || hs < 32) return -2;
    SHA256(in, n, hash);
    *hl = g_psa_olen;
    return PSA_SUCCESS;
}

// ---- strict checkers, independent of the hub's own ------------------------------------------------------
static bool valid_utf8(const std::string& s) {   // RFC 3629
    size_t i = 0;
    const size_t n = s.size();
    while (i < n) {
        const unsigned char c = (unsigned char)s[i];
        if (c < 0x80) { i++; continue; }
        size_t len;
        uint32_t cp;
        if (c >= 0xC2 && c <= 0xDF) { len = 2; cp = c & 0x1F; }
        else if (c >= 0xE0 && c <= 0xEF) { len = 3; cp = c & 0x0F; }
        else if (c >= 0xF0 && c <= 0xF4) { len = 4; cp = c & 0x07; }
        else return false;
        if (i + len > n) return false;
        for (size_t k = 1; k < len; k++) {
            const unsigned char d = (unsigned char)s[i + k];
            if ((d & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (d & 0x3F);
        }
        if (len == 3 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) return false;
        if (len == 4 && (cp < 0x10000 || cp > 0x10FFFF)) return false;
        i += len;
    }
    return true;
}

// A reply must be JSON a strict parser accepts: valid UTF-8, no raw control byte, and it parses.
static bool parse(const std::string& r, JsonDocument& d) {
    bool ok = valid_utf8(r);
    for (unsigned char c : r) if (c < 0x20) ok = false;
    if (deserializeJson(d, r)) ok = false;
    if (!ok) { g_bad++; printf("MISMATCH: reply is not strict JSON / UTF-8: %.300s\n", r.c_str()); }
    return ok;
}

static std::string one() {   // exactly one reply was sent
    CHECK(g_replies.size() == 1, "%zu replies", g_replies.size());
    std::string r = g_replies.empty() ? std::string() : g_replies.back();
    g_replies.clear();
    return r;
}

static RuleSlot mk_rule(uint16_t id, const std::string& name, const std::string& dsl, bool en = true,
                        uint8_t tt = 1, uint8_t rt = 2) {
    RuleSlot s{};
    s.rule_id = id;
    s.enabled = en;
    s.src_len = (uint16_t)dsl.size();
    s.trigger_type = tt;
    s.rule_type = rt;
    memcpy(s.name, name.data(), std::min(name.size(), sizeof(s.name) - 1));   // a byte cut, like strncpy
    memcpy(s.src, dsl.data(), std::min(dsl.size(), sizeof(s.src)));
    return s;
}

static void reset() {
    g_replies.clear();
    g_store.clear();
    g_upd.clear();
    g_alloc_calls = g_alloc_fail_at = g_add_calls = g_check_calls = 0;
    g_add_ok = g_upd_ok = g_check_ok = true;
    g_last_err.clear(); g_check_msg.clear();
    g_new_id = 0;
    g_psa_fail = false;
    g_psa_olen = 32;
}

// ---- send_err / reply_ok_or_err -------------------------------------------------------------------------
static void test_send_err() {
    reset();
    h_send_err(1, 7, "oom");
    CHECK(one() == R"({"id":7,"ok":false,"err":"oom"})", "plain text keeps the old shape");

    // Review Focus 2: the parser quotes what the user typed
    const char* quoted = "invalid numeric literal 'x\"y'";
    JsonDocument d;
    h_send_err(1, 8, quoted);
    if (parse(one(), d)) {
        CHECK(d["id"] == 8 && d["ok"] == false, "envelope");
        CHECK(std::strcmp(d["err"].as<const char*>(), quoted) == 0, "quote survives");
    }
    const char* tricky = "bad \\ back\\slash \"q\" and \"} ,\"ok\":true tail\n\ttab \x01 ctrl";
    h_send_err(1, 9, tricky);
    if (parse(one(), d)) CHECK(d["err"].as<std::string>() == tricky, "backslash, quotes, newline, ctrl round trip");

    const char* uni = "\xD0\xBF\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82 \xF0\x9F\x98\x80 ok";   // Cyrillic, emoji
    h_send_err(1, 10, uni);
    if (parse(one(), d)) CHECK(d["err"].as<std::string>() == uni, "valid UTF-8 passes through byte-identical");

    h_send_err(1, 11, "cut \xD0");   // a lone lead byte (a name cut mid-character)
    if (parse(one(), d)) CHECK(d["err"].as<std::string>() == "cut \xEF\xBF\xBD", "invalid byte becomes U+FFFD");
    h_send_err(1, 12, "emoji \xF0\x9F\x98");   // a truncated emoji: one replacement per stray byte
    if (parse(one(), d)) CHECK(valid_utf8(d["err"].as<std::string>()), "truncated emoji is healed");

    h_send_err(1, 13, std::string(300, 'a').c_str());   // 160 bytes of message at most
    if (parse(one(), d)) CHECK(d["err"].as<std::string>() == std::string(160, 'a'), "cut at 160 bytes");
    std::string cut160 = std::string(159, 'a') + "\xD0\xB6" + "tail";   // the 160-byte cut lands inside a letter
    h_send_err(1, 14, cut160.c_str());
    if (parse(one(), d)) CHECK(d["err"].as<std::string>() == std::string(159, 'a') + "\xEF\xBF\xBD", "cut mid-character");

    h_send_err(1, 4294967295u, std::string(160, '\x01').c_str());   // the worst case: all \u0001, longest id
    std::string worst = g_replies.empty() ? "" : g_replies.back();
    if (parse(one(), d)) {
        CHECK(d["err"].as<std::string>() == std::string(160, '\x01'), "160 control bytes all survive");
        CHECK(worst.size() < 1100, "worst reply %zu bytes fits the 1100-byte buffer", worst.size());
        CHECK(d["id"].as<uint32_t>() == 4294967295u, "id");
    }
    h_send_err(1, 15, nullptr);
    CHECK(one() == R"({"id":15,"ok":false,"err":""})", "null text");
}

static void test_reply_ok_or_err() {
    reset();
    h_reply_ok_or_err(1, 9, true, nullptr);
    CHECK(one() == R"({"id":9,"ok":true})", "ok reply unchanged");
    h_reply_ok_or_err(1, 9, true, "ignored");
    CHECK(one() == R"({"id":9,"ok":true})", "ok reply ignores the text");
    h_reply_ok_or_err(1, 10, false, nullptr);
    CHECK(one() == R"({"id":10,"ok":false,"err":"failed"})", "failure without text");
    JsonDocument d;
    h_reply_ok_or_err(1, 11, false, "no \"def\" or \\ transport");
    if (parse(one(), d)) CHECK(d["err"].as<std::string>() == "no \"def\" or \\ transport", "failure text is escaped");
}

// ---- sha256_hex -----------------------------------------------------------------------------------------
static void test_sha256_hex() {
    reset();
    char out[65];
    CHECK(h_sha256_hex("", 0, out) && std::strcmp(out, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0,
          "empty: %s", out);
    CHECK(h_sha256_hex("abc", 3, out) && std::strcmp(out, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0,
          "abc: %s", out);
    CHECK(h_sha256_hex(TXT_CYR, strlen(TXT_CYR), out) && std::strcmp(out, FP_CYR) == 0, "Cyrillic text: %s vs sha256sum %s", out, FP_CYR);
    CHECK(strlen(out) == 64, "64 characters");
    for (const char* p = out; *p; p++) CHECK((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f'), "lowercase hex only");
    g_psa_fail = true;
    strcpy(out, "stale");
    CHECK(!h_sha256_hex("abc", 3, out) && out[0] == '\0', "a PSA failure gives false and an empty string");
    g_psa_fail = false;
    g_psa_olen = 31;
    CHECK(!h_sha256_hex("abc", 3, out) && out[0] == '\0', "a short digest gives false");
}

// ---- rule.list ------------------------------------------------------------------------------------------
// The rows of a page, as the raw text between "items":[ and the closing ]
static std::string items_text(const std::string& r, bool has_next) {
    const size_t b = r.find("\"items\":[");
    const size_t e = has_next ? r.rfind("],\"next_cursor\"") : r.size() - 3;   // ...]}}
    if (b == std::string::npos || e == std::string::npos || e < b + 9) { g_bad++; printf("MISMATCH: page shape %.200s\n", r.c_str()); return ""; }
    return r.substr(b + 9, e - (b + 9));
}

struct Page { std::vector<unsigned> ids; std::string next; size_t reply_len = 0, rows_bytes = 0; bool ok = false; };

// A request as the hub's ws_rx sees it: JSON text parsed into a JsonDocument (strings copied out of the text).
static JsonDocument wire(const std::string& json) {
    JsonDocument d;
    if (deserializeJson(d, json)) { g_bad++; printf("MISMATCH: bad request text %s\n", json.c_str()); }
    return d;
}

static Page list_page(uint32_t id, const char* cursor_json /* JSON text of the cursor value, or nullptr */) {
    std::string t = "{\"id\":" + std::to_string(id) + ",\"cmd\":\"rule.list\"";
    if (cursor_json) t += std::string(",\"args\":{\"cursor\":") + cursor_json + "}";
    JsonDocument q = wire(t + "}");
    h_rule_list(1, id, q);
    Page p;
    const std::string r = one();
    p.reply_len = r.size();
    JsonDocument d;
    if (!parse(r, d)) return p;
    JsonVariantConst data = d["data"];
    if (!(d["ok"] == true) || !data["items"].is<JsonArrayConst>()) {
        if (!(d["ok"] == false)) { g_bad++; printf("MISMATCH: not a page: %.200s\n", r.c_str()); }
        return p;
    }
    p.ok = true;
    for (JsonVariantConst row : data["items"].as<JsonArrayConst>()) p.ids.push_back(row["id"].as<unsigned>());
    p.next = data["next_cursor"] | "";
    p.rows_bytes = items_text(r, !p.next.empty()).size();
    return p;
}

static void test_rule_list_bare() {
    reset();
    JsonDocument q, d;
    q["id"] = 3; q["cmd"] = "rule.list";
    h_rule_list(1, 3, q);
    CHECK(one() == R"({"id":3,"ok":true,"data":[]})", "empty store, bare array");

    // shuffled store; names and texts with quotes, backslashes, Cyrillic; a name cut mid-character
    g_store.push_back(mk_rule(9, "nine \"q\"", "ON a#b=1 DO log \"x\\y\" ENDON", false, 3, 4));
    g_store.push_back(mk_rule(2, "\xD0\x9B\xD0\xB0\xD0\xBC\xD0\xBF\xD0\xB0", TXT_CYR));
    g_store.push_back(mk_rule(5, "\xD0\xB0\xD0\xB1\xD0\xB2\xD0\xB3\xD0\xB4\xD0\xB5\xD0\xB6\xD0\xB7\xD0\xB8\xD0\xB9\xD0\xBA\xD0\xBB",   // 12 letters = 24 bytes, name[23] cuts the last
                              "ON x#y=2 DO log z ENDON"));
    h_rule_list(1, 4, q);
    const std::string r = one();
    if (parse(r, d)) {
        JsonArrayConst a = d["data"].as<JsonArrayConst>();
        CHECK(a.size() == 3, "3 rows");
        CHECK(a[0]["id"] == 2 && a[1]["id"] == 5 && a[2]["id"] == 9, "sorted by id");
        CHECK(a[2]["enabled"] == false && a[2]["trigger_type"] == 3 && a[2]["rule_type"] == 4 && a[0]["enabled"] == true, "fields");
        CHECK(a[2]["name"].as<std::string>() == "nine \"q\"", "quoted name");
        CHECK(a[2]["dsl"].as<std::string>() == "ON a#b=1 DO log \"x\\y\" ENDON", "quoted text");
        CHECK(a[0]["name"].as<std::string>() == "\xD0\x9B\xD0\xB0\xD0\xBC\xD0\xBF\xD0\xB0", "Cyrillic name");
        CHECK(a[0]["dsl"].as<std::string>() == TXT_CYR, "Cyrillic text");
        const std::string cutname = a[1]["name"].as<std::string>();
        CHECK(cutname.size() == 22 + 3, "11 whole letters and one U+FFFD: %zu bytes", cutname.size());
    }
    // an explicit null cursor is no cursor
    q["args"]["cursor"] = nullptr;
    h_rule_list(1, 5, q);
    CHECK(one().rfind("{\"id\":5,\"ok\":true,\"data\":[", 0) == 0, "cursor:null answers the bare array");
}

static void test_rule_list_bare_cap() {
    reset();
    for (unsigned i = 1; i <= 256; i++) g_store.push_back(mk_rule((uint16_t)(257 - i), "rule", std::string(499, 'x')));   // reversed
    JsonDocument q;
    q["id"] = 6; q["cmd"] = "rule.list";
    h_rule_list(1, 6, q);
    const std::string r = one();
    JsonDocument d;
    CHECK(r.size() <= 16 * 1024, "bare array stays within 16 KB: %zu", r.size());
    if (parse(r, d)) {
        JsonArrayConst a = d["data"].as<JsonArrayConst>();
        CHECK(a.size() >= 20 && a.size() < 256, "%zu rows fit", a.size());
        unsigned want = 1;
        for (JsonVariantConst row : a) CHECK(row["id"].as<unsigned>() == want++, "rows are the lowest ids, in order");
    }
}

static void test_rule_list_paged() {
    reset();
    // 14 rules of ~480 bytes with sparse ids, stored out of order
    const unsigned ids[] = {40, 3, 17, 5, 6, 99, 21, 8, 12, 30, 31, 32, 250, 1};
    std::vector<unsigned> sorted(std::begin(ids), std::end(ids));
    std::sort(sorted.begin(), sorted.end());
    for (unsigned id : ids) g_store.push_back(mk_rule((uint16_t)id, "rule " + std::to_string(id), std::string(470, 'a' + id % 26) + " \"q\""));
    const size_t budget = h_page_budget();

    std::vector<unsigned> seen;
    std::string cursor = "0";
    int pages = 0;
    for (; pages < 40; pages++) {
        Page p = list_page(100 + pages, ("\"" + cursor + "\"").c_str());
        CHECK(p.ok, "page %d", pages);
        if (!p.ok) break;
        CHECK(p.reply_len < 8192, "page %d is %zu bytes (under the 8 KB frame limit)", pages, p.reply_len);
        CHECK(p.rows_bytes <= budget, "page %d rows are %zu bytes, budget %zu", pages, p.rows_bytes, budget);
        CHECK(!p.ids.empty(), "page %d is empty", pages);
        seen.insert(seen.end(), p.ids.begin(), p.ids.end());
        if (p.next.empty()) { pages++; break; }
        CHECK(p.next == std::to_string(p.ids.back()), "next_cursor is the last id of the page: %s vs %u", p.next.c_str(), p.ids.back());
        cursor = p.next;
    }
    CHECK(pages >= 2, "14 rules of ~480 bytes need at least two pages, got %d", pages);
    CHECK(seen == sorted, "every rule exactly once, in id order (%zu seen)", seen.size());

    // cursor spellings
    Page first = list_page(200, "\"0\"");
    Page num   = list_page(201, "0");
    Page junk  = list_page(202, "\"abc\"");
    CHECK(first.ok && first.ids == num.ids && first.ids == junk.ids, "\"0\", 0 and a non-number all start from the beginning");
    Page n17 = list_page(203, "17");
    CHECK(n17.ok && !n17.ids.empty() && n17.ids.front() == 21, "a numeric cursor 17 continues after id 17 (first %u)", n17.ids.empty() ? 0 : n17.ids.front());
    JsonDocument noargs;   // no args at all: the bare array, not a crash
    h_rule_list(1, 205, noargs);
    CHECK(one().rfind("{\"id\":205,\"ok\":true,\"data\":[", 0) == 0, "no args answers the bare array");
    JsonDocument q = wire(R"({"id":206,"cmd":"rule.list","args":{"cursor":"250"}})");
    h_rule_list(1, 206, q);
    CHECK(one() == R"({"id":206,"ok":true,"data":{"items":[]}})", "a cursor at the last id: no items, no next_cursor");

    // a rule deleted between two pages does not shift the second page
    Page p1 = list_page(300, "\"0\"");
    CHECK(p1.ok && p1.next == std::to_string(p1.ids.back()), "page 1 has more");
    if (p1.ids.empty() || p1.next.empty()) return;   // already reported above; nothing to delete between pages
    const unsigned doomed = *std::upper_bound(sorted.begin(), sorted.end(), p1.ids.back());
    for (size_t i = 0; i < g_store.size(); i++) if (g_store[i].rule_id == doomed) { g_store.erase(g_store.begin() + i); break; }
    Page p2 = list_page(301, ("\"" + p1.next + "\"").c_str());
    CHECK(p2.ok && !p2.ids.empty() && p2.ids.front() > doomed, "after deleting %u the next page starts past it (first %u)", doomed, p2.ids.empty() ? 0 : p2.ids.front());
    std::vector<unsigned> all = p1.ids;
    all.insert(all.end(), p2.ids.begin(), p2.ids.end());
    for (size_t i = 1; i < all.size(); i++) CHECK(all[i] > all[i - 1], "ids strictly increase across pages: no row twice");
}

static void test_rule_list_worst_rows() {
    reset();
    const std::string name(23, '\x01'), dsl(499, '\x02');   // every byte escapes to six
    for (uint16_t id = 1; id <= 5; id++) g_store.push_back(mk_rule(id, name, dsl));
    std::string cursor = "0";
    int pages = 0;
    std::vector<unsigned> seen;
    for (; pages < 20; pages++) {
        JsonDocument q = wire("{\"args\":{\"cursor\":\"" + cursor + "\"}}");
        h_rule_list(1, 400 + pages, q);
        const std::string r = one();
        JsonDocument d;
        if (!parse(r, d)) break;
        JsonArrayConst a = d["data"]["items"].as<JsonArrayConst>();
        CHECK(a.size() >= 1, "a worst-case row is never dropped");
        for (JsonVariantConst row : a) {
            seen.push_back(row["id"].as<unsigned>());
            CHECK(row["name"].as<std::string>() == name && row["dsl"].as<std::string>() == dsl, "worst-case row round trips");
        }
        const std::string next = d["data"]["next_cursor"] | "";
        const size_t ib = items_text(r, !next.empty()).size();
        CHECK(ib <= h_page_budget(), "page rows %zu bytes", ib);
        CHECK(r.size() < 8192, "reply %zu bytes", r.size());
        if (next.empty()) { pages++; break; }
        cursor = next;
    }
    CHECK(seen == (std::vector<unsigned>{1, 2, 3, 4, 5}), "all 5 worst-case rows arrive once (%zu seen in %d pages)", seen.size(), pages);
    CHECK(pages == 5, "3.2 KB rows go one to a page, got %d pages", pages);
}

static void test_rule_list_corrupt_slot() {
    reset();
    RuleSlot bad = mk_rule(7, "x", "ON a#b=1 DO log c ENDON");
    bad.src_len = 0xFFFF;   // corrupt length: clamped to the 500-byte field, text stops at its NUL
    g_store.push_back(bad);
    memset(bad.name, 'n', sizeof(bad.name));   // a name with no NUL at all
    bad.rule_id = 8;
    g_store.push_back(bad);
    Page p = list_page(500, "\"0\"");
    CHECK(p.ok && p.ids.size() == 2, "corrupt slots still list (%zu rows)", p.ids.size());
}

static void test_rule_list_oom() {
    for (int fail_at = 1; fail_at <= 3; fail_at++) {
        reset();
        g_store.push_back(mk_rule(1, "a", "ON a#b=1 DO log c ENDON"));
        g_alloc_fail_at = fail_at;
        JsonDocument q;
        q["id"] = 600;
        h_rule_list(1, 600, q);
        CHECK(one() == R"({"id":600,"ok":false,"err":"oom"})", "allocation %d failing answers oom", fail_at);
    }
}

// ---- rule.create ----------------------------------------------------------------------------------------
static void test_rule_create() {
    reset();
    JsonDocument q, d;
    q["args"]["name"] = "porch";
    q["args"]["dsl"]  = "ON a#b=1 DO log c ENDON";
    g_new_id = 42;
    h_rule_create(1, 5, q);
    CHECK(one() == R"({"id":5,"ok":true,"data":{"id":42}})", "create answers the new id");
    g_new_id = 65535;
    h_rule_create(1, 4294967295u, q);
    CHECK(one() == R"({"id":4294967295,"ok":true,"data":{"id":65535}})", "largest id and request id fit");

    g_add_ok = false;
    g_last_err = "invalid numeric literal 'x\"y'";
    h_rule_create(1, 6, q);
    if (parse(one(), d)) {
        CHECK(d["ok"] == false && d["err"].as<std::string>() == g_last_err, "parser message with a quote reaches the caller");
    }
    g_add_ok = true;
    int calls = g_add_calls;
    JsonDocument none;
    h_rule_create(1, 7, none);
    CHECK(one() == R"({"id":7,"ok":false,"err":"missing dsl"})" && g_add_calls == calls, "no dsl: refused before the parser");
    JsonDocument empty;
    empty["args"]["dsl"] = "";
    h_rule_create(1, 8, empty);
    CHECK(one() == R"({"id":8,"ok":false,"err":"missing dsl"})" && g_add_calls == calls, "empty dsl: refused");
}

// ---- rule.update ----------------------------------------------------------------------------------------
static JsonDocument upd_req(unsigned rid, const std::string& dsl, const std::string* base /* nullptr = absent */) {
    JsonDocument b;   // built with copies, then sent through the wire like the hub's ws_rx gets it
    b["args"]["id"] = rid;
    b["args"]["name"] = std::string("n");
    b["args"]["dsl"] = dsl;
    if (base) b["args"]["base"] = *base;
    std::string t;
    serializeJson(b, t);
    return wire(t);
}
static JsonDocument upd_req(unsigned rid, const std::string& dsl, const std::string& base) { return upd_req(rid, dsl, &base); }
static JsonDocument upd_req(unsigned rid, const std::string& dsl, std::nullptr_t) { return upd_req(rid, dsl, (const std::string*)nullptr); }

static void test_rule_update() {
    reset();
    JsonDocument d;
    g_store.push_back(mk_rule(3, "porch", TXT_PLAIN));
    g_store.push_back(mk_rule(4, "cyr", TXT_CYR));

    // no base: as before
    { JsonDocument q = upd_req(3, "ON a#b=2 DO log c ENDON", nullptr);
      h_rule_update(1, 1, q);
      CHECK(one() == R"({"id":1,"ok":true})" && g_upd.size() == 1 && g_upd[0].id == 3, "no base: saved"); }
    g_store[0] = mk_rule(3, "porch", TXT_PLAIN);   // back to the text the base below names
    g_upd.clear();

    // the right base: saved; the same base again: the text changed, so `changed`
    { JsonDocument q = upd_req(3, "ON a#b=3 DO log c ENDON", FP_PLAIN);
      h_rule_update(1, 2, q);
      CHECK(one() == R"({"id":2,"ok":true})" && g_upd.size() == 1, "base matches: saved");
      JsonDocument q2 = upd_req(3, "ON a#b=4 DO log c ENDON", FP_PLAIN);
      h_rule_update(1, 3, q2);
      CHECK(one() == R"({"id":3,"ok":false,"err":"changed"})" && g_upd.size() == 1, "stale base: changed, nothing written"); }

    // a Cyrillic text with quotes: the fingerprint is over the UTF-8 bytes
    { JsonDocument q = upd_req(4, "ON a#b=5 DO log c ENDON", FP_CYR);
      h_rule_update(1, 4, q);
      CHECK(one() == R"({"id":4,"ok":true})" && g_upd.size() == 2, "Cyrillic text: base matches sha256sum"); }

    // mismatches
    g_upd.clear();
    g_store[0] = mk_rule(3, "porch", TXT_PLAIN);
    { JsonDocument q = upd_req(3, "x", std::string(64, '0').c_str());
      h_rule_update(1, 5, q);
      CHECK(one() == R"({"id":5,"ok":false,"err":"changed"})" && g_upd.empty(), "wrong base"); }
    { std::string up = FP_PLAIN; for (char& c : up) c = (char)toupper((unsigned char)c);
      JsonDocument q = upd_req(3, "x", up.c_str());
      h_rule_update(1, 6, q);
      CHECK(one() == R"({"id":6,"ok":false,"err":"changed"})" && g_upd.empty(), "the fingerprint is lowercase hex: UPPERCASE is not it"); }
    { JsonDocument q = upd_req(3, "x", "");
      h_rule_update(1, 7, q);
      CHECK(one() == R"({"id":7,"ok":false,"err":"changed"})" && g_upd.empty(), "empty base"); }
    { JsonDocument q = upd_req(3, "x", (std::string(FP_PLAIN).substr(0, 63)).c_str());
      h_rule_update(1, 8, q);
      CHECK(one() == R"({"id":8,"ok":false,"err":"changed"})" && g_upd.empty(), "a prefix of the fingerprint is not it"); }

    // F19: the rule is gone on the hub: `changed` (the cloud reloads and sees it gone)
    { JsonDocument q = upd_req(77, "x", FP_PLAIN);
      h_rule_update(1, 9, q);
      CHECK(one() == R"({"id":9,"ok":false,"err":"changed"})" && g_upd.empty(), "deleted rule + base: changed"); }

    // the name is not part of the fingerprint: a renamed rule with the same text still matches
    g_store[0] = mk_rule(3, "renamed on the hub", TXT_PLAIN);
    { JsonDocument q = upd_req(3, "ON a#b=6 DO log c ENDON", FP_PLAIN);
      h_rule_update(1, 10, q);
      CHECK(one() == R"({"id":10,"ok":true})" && g_upd.size() == 1, "a rename on the hub is not a text change"); }

    // PSA failing: cannot prove the text is unchanged, so nothing is written
    g_upd.clear();
    g_store[0] = mk_rule(3, "porch", TXT_PLAIN);
    g_psa_fail = true;
    { JsonDocument q = upd_req(3, "x", FP_PLAIN);
      h_rule_update(1, 11, q);
      CHECK(one() == R"({"id":11,"ok":false,"err":"changed"})" && g_upd.empty(), "PSA failure: changed"); }
    g_psa_fail = false;

    // a corrupt stored length is clamped to the 500-byte field, not read past it
    g_store[0].src_len = 0xFFFF;
    { JsonDocument q = upd_req(3, "x", FP_PLAIN);
      h_rule_update(1, 12, q);
      CHECK(one() == R"({"id":12,"ok":false,"err":"changed"})" && g_upd.empty(), "corrupt length: changed, no overread"); }

    // failures of the parser itself, and the shape errors
    g_store[0] = mk_rule(3, "porch", TXT_PLAIN);
    g_upd_ok = false;
    g_last_err = "invalid numeric literal 'x\"y'";
    { JsonDocument q = upd_req(3, "x", nullptr);
      h_rule_update(1, 13, q);
      if (parse(one(), d)) CHECK(d["err"].as<std::string>() == g_last_err, "parser message with a quote, no base"); }
    { JsonDocument q = upd_req(3, "x", FP_PLAIN);
      h_rule_update(1, 14, q);
      if (parse(one(), d)) CHECK(d["err"].as<std::string>() == g_last_err, "parser message with a quote, base matches"); }
    g_last_err = "rule not found";
    { JsonDocument q = upd_req(77, "x", nullptr);
      h_rule_update(1, 15, q);
      CHECK(one() == R"({"id":15,"ok":false,"err":"rule not found"})", "unknown id without base: the parser layer's own words"); }
    g_upd_ok = true;
    { JsonDocument q = upd_req(0, "x", nullptr);
      h_rule_update(1, 16, q);
      CHECK(one() == R"({"id":16,"ok":false,"err":"missing id/dsl"})", "no id"); }
    { JsonDocument q;
      q["args"]["id"] = 3;
      h_rule_update(1, 17, q);
      CHECK(one() == R"({"id":17,"ok":false,"err":"missing id/dsl"})", "no dsl"); }
    // a base alone must not turn an update into a no-op success
    { JsonDocument q = upd_req(3, "ON a#b=7 DO log c ENDON", FP_PLAIN);
      g_upd.clear();
      h_rule_update(1, 18, q);
      CHECK(one() == R"({"id":18,"ok":true})" && g_upd.size() == 1 && g_upd[0].dsl == "ON a#b=7 DO log c ENDON", "the save runs with the new text"); }
}

// ---- rule.check -----------------------------------------------------------------------------------------
static void test_rule_check() {
    reset();
    JsonDocument d;
    JsonDocument q;
    q["args"]["dsl"] = "ON a#b=1 DO log c ENDON";
    h_rule_check(1, 5, q);
    CHECK(one() == R"({"id":5,"ok":true,"data":{"ok":true,"err":""}})", "good text");
    CHECK(g_add_calls == 0 && g_upd.empty(), "nothing is saved");

    g_check_ok = false;
    g_check_msg = "invalid numeric literal 'x\"y'";   // Review Focus 2
    h_rule_check(1, 6, q);
    if (parse(one(), d)) {
        CHECK(d["ok"] == true && d["data"]["ok"] == false, "envelope ok, verdict false");
        CHECK(d["data"]["err"].as<std::string>() == g_check_msg, "the parser's words survive a quote");
    }
    g_check_msg = "back\\slash and \"both\" \xD0\xBF\xD1\x80\xD0\xB8";
    h_rule_check(1, 7, q);
    if (parse(one(), d)) CHECK(d["data"]["err"].as<std::string>() == g_check_msg, "backslash, quotes, Cyrillic");
    g_check_msg = std::string(94, 'a') + "\xD0\xB6";   // 96 bytes: snprintf into err[96] cuts the letter
    h_rule_check(1, 8, q);
    if (parse(one(), d)) {
        const std::string e = d["data"]["err"].as<std::string>();
        CHECK(e == std::string(94, 'a') + "\xEF\xBF\xBD", "a message cut by the 96-byte buffer is healed (%zu bytes)", e.size());
    }
    g_check_msg = std::string(200, '\x01');   // all control bytes: 95 kept, each \u0001
    h_rule_check(1, 4294967295u, q);
    {
        const std::string r = g_replies.empty() ? "" : g_replies.back();
        if (parse(one(), d)) {
            CHECK(d["data"]["err"].as<std::string>() == std::string(95, '\x01'), "worst case message");
            CHECK(r.size() < 700, "worst reply %zu bytes fits the 700-byte buffer", r.size());
        }
    }
    JsonDocument none;
    int calls = g_check_calls;
    h_rule_check(1, 9, none);
    CHECK(one() == R"({"id":9,"ok":false,"err":"missing dsl"})" && g_check_calls == calls, "no dsl: refused before the parser");
    JsonDocument emp;
    emp["args"]["dsl"] = "";
    g_check_ok = false;
    g_check_msg = "empty rule";
    h_rule_check(1, 10, emp);
    if (parse(one(), d)) CHECK(d["data"]["ok"] == false && d["data"]["err"].as<std::string>() == "empty rule", "an empty text is the parser's call");
}

// rule.added / rule.updated (final fix wave, I-1): the row rule.list carries, so a name cut inside a character
// (the hub page sends names of any length; older firmware stored them cut) cannot close the cloud link or a
// hub-page tab, and a long rule full of quotes is pushed instead of dropped.
static void test_rule_events() {
    reset();
    g_events.clear();
    JsonDocument d;
    g_store.push_back(mk_rule(5, "абвгдежзийкл", TXT_CYR, false, 3, 1));   // 12 letters cut at 23 bytes
    h_rule_changed(5, RULE_CHANGE_UPDATED);
    CHECK(g_events.size() == 1 && g_events[0].event == "rule.updated", "one rule.updated push (%zu)", g_events.size());
    if (!g_events.empty() && parse(g_events[0].payload, d)) {
        CHECK(d["id"] == 5 && d["enabled"] == false && d["dsl"].as<std::string>() == TXT_CYR, "id, state, text");
        CHECK(d["name"].as<std::string>() == "абвгдежзийк\xEF\xBF\xBD", "the half character arrives as U+FFFD");
        CHECK(d["trigger_type"] == 3 && d["rule_type"] == 1, "the same row as rule.list");
    }
    g_events.clear();
    h_rule_changed(5, RULE_CHANGE_ADDED);
    CHECK(g_events.size() == 1 && g_events[0].event == "rule.added", "one rule.added push");
    g_events.clear();
    h_rule_changed(5, RULE_CHANGE_DELETED);
    CHECK(g_events.size() == 1 && g_events[0].event == "rule.deleted" && g_events[0].payload == R"({"id":5})",
          "rule.deleted carries the id");

    // 499 quotes: 1,000 bytes of escaped text, more than the 1 KB push buffer the event used before.
    g_events.clear();
    g_store.push_back(mk_rule(6, "q", std::string(499, '"')));
    h_rule_changed(6, RULE_CHANGE_UPDATED);
    CHECK(g_events.size() == 1, "a long rule is pushed, not dropped (%zu)", g_events.size());
    if (!g_events.empty() && parse(g_events[0].payload, d)) {
        CHECK(d["dsl"].as<std::string>() == std::string(499, '"'), "the whole text");
    }

    // No memory for the row, or no such rule: nothing is pushed, nothing crashes.
    g_events.clear();
    g_alloc_fail_at = 1;
    h_rule_changed(6, RULE_CHANGE_UPDATED);
    g_alloc_fail_at = 0;
    h_rule_changed(77, RULE_CHANGE_UPDATED);
    CHECK(g_events.empty(), "no push without memory or without the rule (%zu)", g_events.size());
}

// review F22: two editors with the same base at the same time -- exactly one saves, the other gets
// `changed`. The first is parked between reading the stored text and comparing it, for as long as the
// second needs to finish (or 200 ms, when the lock makes the second wait for the first).
static void test_concurrent_base() {
    reset();
    g_reply_fds.clear();
    g_store.push_back(mk_rule(3, "porch", TXT_PLAIN));
    JsonDocument qa = upd_req(3, "ON a#b=7 DO log first ENDON", FP_PLAIN);
    JsonDocument qb = upd_req(3, "ON a#b=8 DO log second ENDON", FP_PLAIN);
    g_second_done = false;
    g_park_next_load = 1;
    std::thread a([&] { h_rule_update(1, 11, qa); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));   // a is parked by now
    std::thread b([&] { h_rule_update(2, 22, qb); g_second_done = true; });
    a.join();
    b.join();
    int saved = 0, changed = 0;
    for (const std::string& r : g_replies) {
        if (r.find(R"("ok":true)") != std::string::npos) saved++;
        if (r.find(R"("err":"changed")") != std::string::npos) changed++;
    }
    CHECK(saved == 1 && changed == 1 && g_upd.size() == 1,
          "same base from two editors: %d saved, %d changed, %zu writes", saved, changed, g_upd.size());
    g_replies.clear();
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);   // a mismatch is printed even if a later check crashes
    test_send_err();
    test_concurrent_base();
    test_reply_ok_or_err();
    test_sha256_hex();
    test_rule_list_bare();
    test_rule_list_bare_cap();
    test_rule_list_paged();
    test_rule_list_worst_rows();
    test_rule_list_corrupt_slot();
    test_rule_list_oom();
    test_rule_create();
    test_rule_update();
    test_rule_check();
    test_rule_events();
    if (g_bad) printf("%d MISMATCHES\n", g_bad);
    else printf("ALL OK (0 mismatches)\n");
    return g_bad;
}
