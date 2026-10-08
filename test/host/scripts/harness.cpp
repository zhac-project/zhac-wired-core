// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Host check of the wired hub's script ops: script.list (mtime), script.read (whole file, paged with size
// and fp), script.part, script.write (src and staged; base; the hub's own syntax check; limits; events),
// script.check (src and xfer), script.reload, script.error, the saved-script event, and the REST handlers
// (413, full-size read and check, the saved event with size and fp). It runs the REAL code cut out of
// zhac-wired-core/main/ws_bridge.cpp and api_scripts.cpp (see run.sh) against a fake script store, the real
// remote_xfer staging slot, and the real Lua 5.5 syntax check. Every reply is checked as strict JSON and strict
// UTF-8 by code that is NOT the hub's JsonWriter / utf8_seq. Exit code = number of mismatches.
#include "stubs.h"
#include "expect.h"   // FP_BIG_CYR, FP_BIG_CTL: sha256sum of the generated texts (run.sh)
#include "rest.h"

#include <openssl/sha.h>
#include <cctype>
#include <fstream>
#include <sstream>

// wrappers around the static originals (slice_tu.cpp)
void     h_send_err(int fd, uint32_t id, const char* err);
bool     h_sha256_hex(const void* data, size_t len, char out[65]);
void     h_list(int fd, uint32_t id);
void     h_read(int fd, uint32_t id, JsonDocument& doc);
void     h_delete(int fd, uint32_t id, JsonDocument& doc);
void     h_run(int fd, uint32_t id, JsonDocument& doc);
void     h_part(int fd, uint32_t id, JsonDocument& doc);
void     h_write(int fd, uint32_t id, JsonDocument& doc);
void     h_check(int fd, uint32_t id, JsonDocument& doc);
void     h_reload(int fd, uint32_t id);
void     h_script_error(const char* name, int line, const char* message);
uint32_t h_now_ms();
void     ws_push_script_saved(const char* name, const char* src, size_t len, bool existed);   // ws_bridge.h
extern bool g_listeners;
struct Pushed { std::string event, payload; };
extern std::vector<Pushed> g_events;
extern std::string g_order;

static int g_bad = 0, g_checks = 0;
#define CHECK(cond, ...)                                                                   \
    do {                                                                                   \
        g_checks++;                                                                        \
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

static int64_t g_now_us = 100LL * 1000 * 1000;   // 100 s since boot
int64_t esp_timer_get_time() { return g_now_us; }
static void advance_ms(int64_t ms) { g_now_us += ms * 1000; }

static std::vector<std::string> g_replies;
void ws_server_reply(int, const char* buf, size_t len) { g_replies.emplace_back(buf, len); g_order += 'R'; }

static bool g_psa_fail = false;
psa_status_t psa_hash_compute(uint32_t alg, const uint8_t* in, size_t n, uint8_t* hash, size_t hs, size_t* hl) {
    if (g_psa_fail) return -1;
    if (alg != PSA_ALG_SHA_256 || hs < 32) return -2;
    SHA256(in, n, hash);
    *hl = 32;
    return PSA_SUCCESS;
}

// The script store (SPIFFS): the same rules as lua_script_cache.cpp -- name_ok, a write keeps the text up to
// its first NUL, at most 16,384 bytes, at most 16 files, a read refuses a file that does not fit.
struct File { std::string data; uint32_t mtime; };
static std::map<std::string, File> g_fs;   // lists in name order
static bool g_write_fail = false;
static std::function<void()> g_during_write;   // another client acts while the flash write runs
static uint32_t g_save_mtime = 1760000000u;    // the hub clock at the next save (< 1.6e9 = never set)

static bool name_ok(const char* n) {
    if (!n || !*n) return false;
    size_t k = 0;
    for (; *n; ++n) {
        const unsigned char c = static_cast<unsigned char>(*n);
        const bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if (!alnum && c != '_' && c != '-') return false;
        if (++k > LUA_SCRIPT_NAME_MAX) return false;
    }
    return true;
}
extern "C" uint16_t lua_script_cache_list(LuaScriptEntry* out, uint16_t max) {
    uint16_t n = 0;
    for (const auto& kv : g_fs) {
        if (n >= max) break;
        std::memset(&out[n], 0, sizeof(out[n]));
        std::snprintf(out[n].name, sizeof(out[n].name), "%s", kv.first.c_str());
        out[n].size = static_cast<uint16_t>(kv.second.data.size());
        out[n].mtime = kv.second.mtime;
        n++;
    }
    return n;
}
extern "C" int lua_script_cache_read(const char* name, char* out, size_t cap) {
    if (!name_ok(name) || !out || cap == 0) return -1;
    auto it = g_fs.find(name);
    if (it == g_fs.end()) return -1;
    if (it->second.data.size() > cap - 1) return -1;
    std::memcpy(out, it->second.data.data(), it->second.data.size());
    out[it->second.data.size()] = '\0';
    return static_cast<int>(it->second.data.size());
}
extern "C" bool lua_script_cache_exists(const char* name) { return name_ok(name) && g_fs.count(name); }
extern "C" bool lua_script_cache_write(const char* name, const char* src) {
    if (g_during_write) { auto f = std::move(g_during_write); g_during_write = nullptr; f(); }
    if (!name_ok(name) || !src || g_write_fail) return false;
    const size_t n = std::strlen(src);
    if (n > LUA_SCRIPT_SRC_MAX) return false;
    if (!g_fs.count(name) && g_fs.size() >= LUA_SCRIPT_MAX) return false;
    g_fs[name] = File{std::string(src, n), g_save_mtime};
    return true;
}
extern "C" bool lua_script_cache_delete(const char* name) {
    if (!name_ok(name)) return false;
    g_fs.erase(name);
    return true;
}
static bool g_run_ok = true;
static std::string g_run_name;
extern "C" bool lua_engine_run_script(const char* name) { g_run_name = name; return g_run_ok; }
static LuaReloadResult g_reload_res = LUA_RELOAD_DONE;
static uint16_t g_reload_loaded = 3, g_reload_total = 4;
static uint32_t g_reload_wait = 0;
static int g_reload_calls = 0;
extern "C" LuaReloadResult lua_engine_reload(uint32_t wait_ms, uint16_t* loaded, uint16_t* total) {
    g_reload_calls++;
    g_reload_wait = wait_ms;
    if (g_reload_res == LUA_RELOAD_DONE) { *loaded = g_reload_loaded; *total = g_reload_total; }
    return g_reload_res;
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

// A frame must be JSON a strict parser accepts: valid UTF-8, no raw control byte, and it parses.
static bool parse(const std::string& r, JsonDocument& d) {
    bool ok = valid_utf8(r);
    for (unsigned char c : r) if (c < 0x20) ok = false;
    if (deserializeJson(d, r)) ok = false;
    g_checks++;
    if (!ok) { g_bad++; printf("MISMATCH: frame is not strict JSON / UTF-8: %.300s\n", r.c_str()); }
    return ok;
}

static std::string sha(const std::string& s) {
    unsigned char h[32];
    SHA256(reinterpret_cast<const unsigned char*>(s.data()), s.size(), h);
    static const char* kHex = "0123456789abcdef";
    std::string o;
    for (unsigned char b : h) { o += kHex[b >> 4]; o += kHex[b & 15]; }
    return o;
}
static std::string slurp(const char* path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

struct Rep { JsonDocument d; bool ok = false; std::string err, raw; };
static Rep rep() {   // exactly one reply was sent
    Rep r;
    CHECK(g_replies.size() == 1, "%zu replies", g_replies.size());
    r.raw = g_replies.empty() ? std::string() : g_replies.back();
    g_replies.clear();
    if (parse(r.raw, r.d)) {
        r.ok = r.d["ok"] | false;
        r.err = r.d["err"] | "";
    }
    return r;
}

// Request args: strings are copied, keys must be literals.
struct Req {
    JsonDocument doc;
    Req() { doc["args"].to<JsonObject>(); }
    Req& s(const char* k, const std::string& v) { doc["args"][k] = v; return *this; }
    Req& n(const char* k, long v) { doc["args"][k] = v; return *this; }
};

static void put(const std::string& name, const std::string& data, uint32_t mtime = 1760000000u) {
    g_fs[name] = File{data, mtime};
}
static std::string stored(const std::string& name) {
    auto it = g_fs.find(name);
    return it == g_fs.end() ? std::string("<absent>") : it->second.data;
}

static void reset() {
    g_replies.clear();
    g_events.clear();
    g_order.clear();
    g_fs.clear();
    g_alloc_calls = g_alloc_fail_at = 0;
    g_write_fail = false;
    g_during_write = nullptr;
    g_save_mtime = 1760000000u;
    g_listeners = true;
    g_psa_fail = false;
    g_run_ok = true;
    g_reload_res = LUA_RELOAD_DONE; g_reload_loaded = 3; g_reload_total = 4; g_reload_wait = 0; g_reload_calls = 0;
    remote_xfer_reset();
    g_now_us = 100LL * 1000 * 1000;
}

// ---- the calls ----------------------------------------------------------------------------------------------
static Rep write_src(const std::string& name, const std::string& src, const char* base = nullptr) {
    Req r; r.s("name", name).s("src", src);
    if (base) r.s("base", base);
    h_write(1, 7, r.doc);
    return rep();
}
static Rep write_xfer(const std::string& name, const std::string& xfer, long size, const std::string& fp,
                      const char* base = nullptr) {
    Req r; r.s("name", name).s("xfer", xfer).n("size", size).s("fp", fp);
    if (base) r.s("base", base);
    h_write(1, 7, r.doc);
    return rep();
}
static Rep check_xfer(const std::string& xfer) {
    Req r; r.s("xfer", xfer);
    h_check(1, 8, r.doc);
    return rep();
}
static Rep check_src(const std::string& src, const char* name = nullptr) {
    Req r; r.s("src", src);
    if (name) r.s("name", name);
    h_check(1, 8, r.doc);
    return rep();
}
static Rep part(const std::string& x, long i, long n, const std::string* src) {
    Req r; r.s("xfer", x).n("part", i).n("parts", n);
    if (src) r.s("src", *src);
    h_part(1, 7, r.doc);
    return rep();
}
static Rep read_name(const std::string& name) {
    Req r; r.s("name", name);
    h_read(1, 4, r.doc);
    return rep();
}

// Cut `text` into n parts between characters, as the cloud does (never inside a UTF-8 sequence).
static std::vector<std::string> split_parts(const std::string& text, size_t n) {
    std::vector<std::string> v;
    size_t pos = 0;
    for (size_t i = 0; i < n; i++) {
        size_t end = (i + 1 == n) ? text.size() : text.size() * (i + 1) / n;
        while (end < text.size() && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) end++;
        end = std::max(end, pos);
        v.push_back(text.substr(pos, end - pos));
        pos = end;
    }
    return v;
}
// Stages `text` in n parts under `xfer`; `skip_last`: leave the last part out (an incomplete transfer).
static bool stage(const std::string& xfer, const std::string& text, size_t n, bool skip_last = false) {
    const auto v = split_parts(text, n);
    bool ok = true;
    size_t got = 0;
    for (size_t i = 0; i < n - (skip_last ? 1 : 0); i++) {
        Rep p = part(xfer, static_cast<long>(i), static_cast<long>(n), &v[i]);
        got += v[i].size();
        if (!p.ok) { ok = false; printf("  stage: part %zu answered '%s'\n", i, p.err.c_str()); continue; }
        CHECK(p.d["data"]["received"].as<size_t>() == got, "received %zu, want %zu", p.d["data"]["received"].as<size_t>(), got);
    }
    return ok;
}

// A saved-script event: the Nth pushed event as {event name, parsed payload}.
static bool event(size_t i, const char* name, JsonDocument& d) {
    if (i >= g_events.size()) { CHECK(false, "no event #%zu (%zu pushed)", i, g_events.size()); return false; }
    CHECK(g_events[i].event == name, "event #%zu is '%s', want '%s'", i, g_events[i].event.c_str(), name);
    return parse(g_events[i].payload, d);
}

struct Paged { std::string text, fp; size_t size = 0, pages = 0, max_enc = 0, max_reply = 0; bool ok = false; };
// script.read with offset, page after page, as the cloud does.
static Paged read_paged(const std::string& name, size_t start = 0) {
    Paged p;
    size_t off = start;
    for (int guard = 0; guard < 200; guard++) {
        Req r; r.s("name", name).n("offset", static_cast<long>(off));
        h_read(1, 9, r.doc);
        Rep rp = rep();
        if (!rp.ok) { printf("  page at %zu answered '%s'\n", off, rp.err.c_str()); return p; }
        p.pages++;
        p.max_reply = std::max(p.max_reply, rp.raw.size());
        const size_t a = rp.raw.find("\"src\":\""), b = rp.raw.find("\",\"offset\":");
        CHECK(a != std::string::npos && b != std::string::npos && b >= a + 7, "page layout");
        if (a != std::string::npos && b != std::string::npos && b >= a + 7) p.max_enc = std::max(p.max_enc, b - (a + 7));
        JsonObjectConst d = rp.d["data"];
        p.text += d["src"].as<std::string>();
        const size_t sz = d["size"] | 0u;
        const std::string fp = d["fp"] | "";
        CHECK(d["offset"].as<size_t>() == off, "offset echoed");
        if (p.pages == 1) { p.size = sz; p.fp = fp; }
        else CHECK(sz == p.size && fp == p.fp, "size and fp repeat on every page");
        if (d["next_offset"].isNull()) { p.ok = true; return p; }
        const size_t nx = d["next_offset"];
        CHECK(nx > off, "next_offset %zu does not advance from %zu", nx, off);
        if (nx <= off) return p;
        off = nx;
    }
    return p;
}

static const std::string kHelloLua = "print('hello')\n";

// ---- sha256_hex / send_err ---------------------------------------------------------------------------------
static void test_helpers() {
    printf("== helpers\n");
    reset();
    char fp[65];
    CHECK(h_sha256_hex("", 0, fp) && std::string(fp) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "empty digest");
    CHECK(h_sha256_hex("abc", 3, fp) && std::string(fp) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "abc digest");
    g_psa_fail = true;
    CHECK(!h_sha256_hex("abc", 3, fp) && fp[0] == '\0', "PSA failure gives false and an empty string");
    g_psa_fail = false;
    h_send_err(1, 3, "check:2:unfinished string near '\"ab'");
    Rep r = rep();
    CHECK(!r.ok && r.err == "check:2:unfinished string near '\"ab'", "a quote survives send_err");
}

// ---- script.list ----------------------------------------------------------------------------------------------
static void test_list() {
    printf("== script.list\n");
    reset();
    h_list(1, 5);
    CHECK(g_replies.size() == 1 && g_replies[0] == R"({"id":5,"ok":true,"data":[]})", "empty store");
    g_replies.clear();

    put("alpha", "x", 1760000000u);
    put("beta", "yy", 0);
    put("delta", "q", 1600000000u);   // exactly the threshold: a real date
    put("gamma", "zzz", 1599999999u); // one second under: the clock was not set
    h_list(1, 6);
    Rep r = rep();
    CHECK(r.ok, "list ok");
    JsonArrayConst a = r.d["data"];
    CHECK(a.size() == 4, "4 entries");
    if (a.size() == 4) {
        CHECK(std::string(a[0]["name"] | "") == "alpha" && a[0]["size"] == 1 && a[0]["mtime"] == 1760000000u, "alpha has its mtime");
        CHECK(std::string(a[1]["name"] | "") == "beta" && a[1]["size"] == 2 && a[1]["mtime"].isNull(), "beta: mtime 0 left out");
        CHECK(std::string(a[2]["name"] | "") == "delta" && a[2]["mtime"] == 1600000000u, "delta: 1.6e9 is in");
        CHECK(std::string(a[3]["name"] | "") == "gamma" && a[3]["mtime"].isNull(), "gamma: 1,599,999,999 is out");
    }

    // 16 scripts, 24-character names, full size, with times: the old 1 KB buffer was too small
    reset();
    for (int i = 0; i < 16; i++) {
        char nm[32];
        std::snprintf(nm, sizeof(nm), "s%023d", i);
        put(nm, std::string(16384, 'a'), 1760000000u + i);
    }
    h_list(1, 6);
    r = rep();
    CHECK(r.ok && r.d["data"].as<JsonArrayConst>().size() == 16, "16 scripts listed");
    CHECK(r.raw.size() > 1024 && r.raw.size() < 2048, "reply is %zu bytes: over the old 1 KB, within 2 KB", r.raw.size());
    CHECK(r.d["data"][15]["mtime"] == 1760000015u && r.d["data"][15]["size"] == 16384, "last entry");
}

// ---- script.read, no offset -------------------------------------------------------------------------------------
static void test_read_plain() {
    printf("== script.read (whole file)\n");
    reset();
    { Req r; h_read(1, 3, r.doc); CHECK(rep().err == "missing name", "missing name"); }
    CHECK(read_name("nope").err == "not found", "not found");
    CHECK(read_name("bad name!").err == "not found", "a name the store refuses");

    put("small", "print('h\xC3\xA9llo \"q\" \\ \xD0\x96')\n\ttab\n");
    Rep r = read_name("small");
    CHECK(r.ok && r.d["data"]["src"].as<std::string>() == "print('h\xC3\xA9llo \"q\" \\ \xD0\x96')\n\ttab\n", "small text round trip");
    CHECK(r.d["data"]["offset"].isNull() && r.d["data"]["fp"].isNull(), "without offset: today's {src} only");

    // a full-size script answered "not found" (the buffer held 16,383 bytes)
    const std::string big = slurp("big_cyr.txt");
    CHECK(big.size() == 16384, "test data is 16,384 bytes");
    put("bigcyr", big);
    r = read_name("bigcyr");
    CHECK(r.ok && r.d["data"]["src"].as<std::string>() == big, "a 16,384-byte script reads (it was 'not found')");

    // heavy escaping: 16,384 bytes of quotes and backslashes encode to ~32 KB; a fixed 20 KB buffer cut that into
    // invalid JSON. (ArduinoJson writes other control bytes raw -- today's behaviour of this path; the paged read
    // escapes them, see big_ctl below.)
    std::string esc = "--[==[";
    for (int i = 0; i < 8187; i++) esc += "\"\\";
    esc += "]==]";
    CHECK(esc.size() == 16384, "test data is 16,384 bytes");
    put("bigesc", esc);
    r = read_name("bigesc");
    CHECK(r.ok && r.d["data"]["src"].as<std::string>() == esc, "the reply is sized to fit: all %zu bytes arrive", esc.size());
    CHECK(r.raw.size() > 32000, "reply is %zu bytes (over the old 20 KB buffer)", r.raw.size());

    g_alloc_fail_at = 1;
    CHECK(read_name("small").err == "oom", "oom on the source buffer");
    g_alloc_calls = 0; g_alloc_fail_at = 2;
    CHECK(read_name("small").err == "oom", "oom on the reply buffer");
    g_alloc_fail_at = 0;
}

// ---- script.read, paged -----------------------------------------------------------------------------------------
static void test_read_paged() {
    printf("== script.read (offset)\n");
    reset();
    const std::string big = slurp("big_cyr.txt");
    put("bigcyr", big);
    Paged p = read_paged("bigcyr");
    CHECK(p.ok, "paging reached the end");
    CHECK(p.text == big, "the pages put together are the file, byte for byte");
    CHECK(p.fp == FP_BIG_CYR && p.size == big.size(), "size and fp (sha256sum) on the pages");
    CHECK(p.pages >= 3, "%zu pages", p.pages);
    CHECK(p.max_enc <= kRemotePageBudget, "a page holds at most %zu encoded bytes (max seen %zu)", kRemotePageBudget, p.max_enc);
    CHECK(p.max_reply < 8192, "a reply is at most %zu bytes", p.max_reply);

    // 6 encoded bytes per control character: 1,024 per page, 16 pages
    const std::string ctl = slurp("big_ctl.txt");
    put("bigctl", ctl);
    p = read_paged("bigctl");
    CHECK(p.ok && p.text == ctl && p.fp == FP_BIG_CTL, "control bytes round trip");
    CHECK(p.pages >= 16 && p.max_enc <= kRemotePageBudget && p.max_reply < 8192, "pages %zu, max encoded %zu, max reply %zu", p.pages, p.max_enc, p.max_reply);

    // one page: no next_offset
    put("tiny", "-- hi\n");
    { Req r; r.s("name", "tiny").n("offset", 0); h_read(1, 4, r.doc); Rep t = rep();
      CHECK(t.ok && t.d["data"]["src"] == "-- hi\n" && t.d["data"]["offset"] == 0 && t.d["data"]["size"] == 6 &&
            t.d["data"]["fp"].as<std::string>() == sha("-- hi\n") && t.d["data"]["next_offset"].isNull(), "one page: no next_offset"); }
    // offset at and past the end: an empty last page
    { Req r; r.s("name", "tiny").n("offset", 6); h_read(1, 4, r.doc); Rep t = rep();
      CHECK(t.ok && t.d["data"]["src"] == "" && t.d["data"]["offset"] == 6 && t.d["data"]["next_offset"].isNull(), "offset == size"); }
    { Req r; r.s("name", "tiny").n("offset", 99999); h_read(1, 4, r.doc); Rep t = rep();
      CHECK(t.ok && t.d["data"]["src"] == "" && t.d["data"]["offset"] == 6, "offset past the end is clamped to the size"); }
    { Req r; r.s("name", "tiny").n("offset", -5); h_read(1, 4, r.doc); Rep t = rep();
      CHECK(t.ok && t.d["data"]["offset"].as<size_t>() <= 6, "a negative offset stays inside the file"); }
    { Req r; r.s("name", "tiny").s("offset", "3"); h_read(1, 4, r.doc); Rep t = rep();
      CHECK(t.ok && t.d["data"]["offset"].as<size_t>() <= 6, "a string offset stays inside the file"); }
    // an empty file
    put("empty", "");
    { Req r; r.s("name", "empty").n("offset", 0); h_read(1, 4, r.doc); Rep t = rep();
      CHECK(t.ok && t.d["data"]["src"] == "" && t.d["data"]["size"] == 0 &&
            t.d["data"]["fp"].as<std::string>() == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" &&
            t.d["data"]["next_offset"].isNull(), "empty file"); }
    // an offset inside a letter (a client that guessed): the stray byte reads as U+FFFD, the rest is intact
    put("zhe", "\xD0\x96\xD0\xB6");
    { Req r; r.s("name", "zhe").n("offset", 1); h_read(1, 4, r.doc); Rep t = rep();
      CHECK(t.ok && t.d["data"]["src"].as<std::string>() == "\xEF\xBF\xBD\xD0\xB6" && t.d["data"]["offset"] == 1, "offset inside a letter"); }
    // a stored file with invalid bytes reads back with U+FFFD, and its fp is the hash of the BYTES THE HUB HOLDS
    // (a client hashing the paged text could never match: base and fp come from what the hub reports)
    const std::string bad = "ok \xD0 mid \xFF end \xE2\x82";
    put("badutf", bad);
    p = read_paged("badutf");
    CHECK(p.ok && p.text == "ok \xEF\xBF\xBD mid \xEF\xBF\xBD end \xEF\xBF\xBD\xEF\xBF\xBD", "invalid bytes become U+FFFD");
    CHECK(p.fp == sha(bad) && p.fp != sha(p.text), "fp is of the stored bytes, not of the text shown");

    // errors with an offset
    { Req r; r.n("offset", 0); h_read(1, 4, r.doc); CHECK(rep().err == "missing name", "paged: missing name"); }
    { Req r; r.s("name", "nope").n("offset", 0); h_read(1, 4, r.doc); CHECK(rep().err == "not found", "paged: not found"); }
    for (int k = 1; k <= 3; k++) {   // source, page, reply buffer
        g_alloc_calls = 0; g_alloc_fail_at = k;
        Req r; r.s("name", "tiny").n("offset", 0); h_read(1, 4, r.doc);
        CHECK(rep().err == "oom", "paged: oom at allocation %d", k);
    }
    g_alloc_fail_at = 0;
    g_psa_fail = true;
    { Req r; r.s("name", "tiny").n("offset", 0); h_read(1, 4, r.doc); CHECK(rep().err == "oom", "paged: a failed hash is an error, not a page without fp"); }
    g_psa_fail = false;
}

// ---- script.part ----------------------------------------------------------------------------------------------
static void test_part() {
    printf("== script.part\n");
    reset();
    const std::string a = "aaaa", b = "bbbbbb", c = "cc";
    Rep r = part("x1", 0, 3, &a);
    CHECK(r.ok && r.d["data"]["xfer"].as<std::string>() == "x1" && r.d["data"]["received"] == 4, "part 0");
    r = part("x1", 1, 3, &b);
    CHECK(r.ok && r.d["data"]["received"] == 10, "part 1: bytes so far");
    r = part("x1", 2, 3, &c);
    CHECK(r.ok && r.d["data"]["received"] == 12, "part 2");

    // shapes: every error drops the slot
    CHECK(part("x1", 0, 1, &a).err == "bad_part", "parts 1");
    CHECK(part("x1", 0, 17, &a).err == "bad_part", "parts 17");
    CHECK(part("", 0, 2, &a).err == "bad_part", "empty xfer");
    CHECK(part("abcdefghijklmnopq", 0, 2, &a).err == "bad_part", "17-character xfer");
    CHECK(part("a-b", 0, 2, &a).err == "bad_part", "xfer with a dash");
    CHECK(part("x1", 0, 2, nullptr).err == "bad_part", "no src");
    { Req q; q.s("xfer", "x1").n("parts", 2).s("src", "x"); h_part(1, 7, q.doc); CHECK(rep().err == "bad_part", "no part index"); }
    CHECK(part("x2", 0, 3, &a).ok && part("x2", 2, 3, &a).err == "bad_part", "a part out of order");
    CHECK(part("x3", 0, 3, &a).ok && part("x4", 1, 3, &a).err == "bad_part", "another xfer's part 1");
    CHECK(part("x5", 0, 3, &a).ok && part("x5", 1, 4, &a).err == "bad_part", "a changed parts count");
    // too large: 16,000 + 385 bytes
    const std::string p1(16000, 'a'), p2(385, 'b');
    CHECK(part("x6", 0, 2, &p1).ok && part("x6", 1, 2, &p2).err == "too_large", "16,385 bytes in total");
    CHECK(write_xfer("zz", "x6", 16385, sha(p1 + p2)).err == "no_xfer", "too_large dropped the slot");
    // a new part 0 drops the transfer before it
    CHECK(part("y1", 0, 2, &a).ok && part("y2", 0, 2, &b).ok && part("y1", 1, 2, &a).err == "bad_part", "part 0 of another transfer");
    // 16,384 bytes in total are fine
    const std::string q1(10000, 'a'), q2(6384, 'b');
    CHECK(part("z1", 0, 2, &q1).ok, "10,000 bytes");
    r = part("z1", 1, 2, &q2);
    CHECK(r.ok && r.d["data"]["received"] == 16384, "16,384 bytes in total");
}

// ---- script.write with src ----------------------------------------------------------------------------------------
static void test_write_src() {
    printf("== script.write (src)\n");
    reset();
    Rep r = write_src("heat", kHelloLua);
    CHECK(r.ok && r.d["data"]["name"] == "heat" && r.d["data"]["size"] == kHelloLua.size() &&
          r.d["data"]["fp"].as<std::string>() == sha(kHelloLua), "new script: {name, size, fp}");
    CHECK(stored("heat") == kHelloLua, "stored");
    CHECK(g_events.size() == 1 && g_order == "RE", "one event, after the reply (order '%s')", g_order.c_str());
    JsonDocument d;
    if (event(0, "script.added", d)) {
        CHECK(d["name"] == "heat" && d["size"] == kHelloLua.size() && d["fp"].as<std::string>() == sha(kHelloLua) && d["mtime"] == 1760000000u,
              "script.added {name, size, fp, mtime}");
    }
    // an overwrite is script.updated
    g_events.clear();
    r = write_src("heat", "print('two')\n");
    CHECK(r.ok && stored("heat") == "print('two')\n", "overwrite");
    JsonDocument d2;
    if (event(0, "script.updated", d2)) CHECK(d2["fp"].as<std::string>() == sha("print('two')\n") && d2["size"] == 13, "script.updated carries the new fp");
    // the hub's clock was never set: no mtime
    reset();
    g_save_mtime = 12;
    r = write_src("heat", kHelloLua);
    JsonDocument d3;
    if (event(0, "script.added", d3)) CHECK(d3["mtime"].isNull(), "no mtime when the clock was not set at the save");
    // no listener: the reply, no event
    reset();
    g_listeners = false;
    r = write_src("heat", kHelloLua);
    CHECK(r.ok && g_events.empty(), "no listener, no event");
    g_listeners = true;

    // names
    reset();
    const char* bad_names[] = {"", "a b", "a.b", "../x", "abcdefghijklmnopqrstuvwxy", "\xD0\x96", "a\"b", "a/b"};
    for (const char* nm : bad_names) {
        Rep b = write_src(nm, kHelloLua);
        CHECK(!b.ok && b.err == "bad_name", "name '%s'", nm);
    }
    { Req q; q.s("src", kHelloLua); h_write(1, 7, q.doc); CHECK(rep().err == "bad_name", "no name"); }
    CHECK(g_fs.empty() && g_events.empty(), "nothing stored, nothing pushed");
    CHECK(write_src("abcdefghijklmnopqrstuvwx", kHelloLua).ok, "24 characters are fine");
    CHECK(write_src("a-b_9", kHelloLua).ok, "dash and underscore are fine");
    { Req q; q.s("name", "nosrc"); h_write(1, 7, q.doc); CHECK(rep().err == "missing src", "no src, no xfer"); }
    CHECK(!g_fs.count("nosrc"), "missing src stored nothing");

    // base
    reset();
    CHECK(write_src("b1", kHelloLua, "").ok && stored("b1") == kHelloLua, "base \"\" on a free name saves");
    Rep ex = write_src("b1", "print('x')\n", "");
    CHECK(!ex.ok && ex.err == "exists" && stored("b1") == kHelloLua, "base \"\" on a taken name: exists, nothing written");
    g_events.clear();
    CHECK(write_src("b1", "print('v2')\n", sha(kHelloLua).c_str()).ok && stored("b1") == "print('v2')\n", "base = the stored fp saves");
    JsonDocument d4;
    if (event(0, "script.updated", d4)) CHECK(d4["fp"].as<std::string>() == sha("print('v2')\n"), "and says updated");
    Rep ch = write_src("b1", "print('v3')\n", sha(kHelloLua).c_str());
    CHECK(!ch.ok && ch.err == "changed" && stored("b1") == "print('v2')\n", "a stale base: changed, nothing written");
    CHECK(write_src("b1", "print('v3')\n", "").err == "exists", "base \"\" again");
    std::string upper = sha("print('v2')\n");
    for (char& ch2 : upper) ch2 = static_cast<char>(toupper(static_cast<unsigned char>(ch2)));
    CHECK(write_src("b1", "print('v3')\n", upper.c_str()).err == "changed", "an UPPERCASE base is not the lowercase fp");
    CHECK(write_src("b1", "print('v3')\n", std::string(64, 'z').c_str()).err == "changed", "a base that is not hex");
    CHECK(write_src("b1", "print('v3')\n", "abc").err == "changed", "a short base");
    CHECK(write_src("nofile", kHelloLua, sha(kHelloLua).c_str()).err == "changed" && !g_fs.count("nofile"), "base fp on a missing file: changed");
    { Req q; q.s("name", "b1").s("src", "print('n')\n").n("base", 123); h_write(1, 7, q.doc); Rep nb = rep();
      CHECK(nb.ok && stored("b1") == "print('n')\n", "a non-string base is no base (no conflict check)"); }
    // the base is the fp the hub REPORTS: a stored file with invalid bytes
    const std::string badbytes = "-- \xD0 end\n";
    put("badf", badbytes);
    Paged pg = read_paged("badf");
    CHECK(pg.ok && write_src("badf", "print('fixed')\n", pg.fp.c_str()).ok && stored("badf") == "print('fixed')\n",
          "base = the fp a paged read reported saves, though the text shown never hashes to it");

    // the hub's own syntax check: a failure writes nothing
    reset();
    put("keep", "print('old')\n");
    g_events.clear();
    Rep sx = write_src("keep", "local x = = 1\n");
    CHECK(!sx.ok && sx.err.rfind("check:1:", 0) == 0 && sx.err.find("unexpected symbol") != std::string::npos,
          "syntax error: '%s'", sx.err.c_str());
    CHECK(stored("keep") == "print('old')\n" && g_events.empty(), "a syntax error writes nothing and pushes nothing");
    sx = write_src("keep", "print('a')\nprint('b')\nx = = 3\n");
    CHECK(sx.err.rfind("check:3:", 0) == 0, "the line: '%s'", sx.err.c_str());
    sx = write_src("keep", "x = \"abc\n");
    CHECK(sx.err.rfind("check:1:", 0) == 0 && sx.err.find("\"abc") != std::string::npos, "a quote in Lua's message survives: '%s'", sx.err.c_str());
    sx = write_src("keep", "x = \"" + [] { std::string s; for (int i = 0; i < 70; i++) s += "\xD0\x96"; return s; }());
    CHECK(sx.err.rfind("check:1:unfinished string near", 0) == 0, "long message with Cyrillic: '%s'", sx.err.c_str());
    CHECK(valid_utf8(sx.err), "and the 127-byte cut left valid UTF-8");
    CHECK(sx.err.size() <= 160, "at most 160 bytes of message");
    CHECK(stored("keep") == "print('old')\n", "still the old text");

    // limits
    reset();
    const std::string full = "--" + std::string(16382, 'a');
    CHECK(full.size() == 16384 && write_src("full", full).ok && stored("full") == full, "a 16,384-byte script saves");
    const std::string over = "--" + std::string(16383, 'a');
    Rep tl = write_src("over", over);
    CHECK(!tl.ok && tl.err == "too_large" && !g_fs.count("over"), "16,385 bytes: too_large");
    reset();
    for (int i = 0; i < 16; i++) { char nm[16]; std::snprintf(nm, sizeof(nm), "s%02d", i); put(nm, "x"); }
    Rep tm = write_src("seventeen", kHelloLua);
    CHECK(!tm.ok && tm.err == "too_many" && g_fs.size() == 16, "a 17th script: too_many");
    CHECK(write_src("s03", kHelloLua).ok && stored("s03") == kHelloLua, "an overwrite is fine with 16 stored");
    g_write_fail = true;
    g_events.clear();
    Rep wf = write_src("s04", kHelloLua);
    CHECK(!wf.ok && wf.err == "write_failed" && stored("s04") == "x" && g_events.empty(), "a flash error: write_failed, nothing pushed");
    g_write_fail = false;

    // a long Cyrillic script as one src
    reset();
    const std::string big = slurp("big_cyr.txt");
    Rep bg = write_src("bigcyr", big);
    CHECK(bg.ok && bg.d["data"]["size"] == 16384 && bg.d["data"]["fp"].as<std::string>() == FP_BIG_CYR && stored("bigcyr") == big,
          "16,384 bytes of Cyrillic, emoji and quotes: stored byte for byte, fp is sha256sum's");
}

// ---- script.write with a staged text ------------------------------------------------------------------------------
static void test_write_staged() {
    printf("== script.write (xfer)\n");
    reset();
    // Review Focus 1 (the hub's half): a long Cyrillic script, split between characters, assembled, saved, read back in pages
    const std::string big = slurp("big_cyr.txt");
    CHECK(stage("cyr1", big, 3), "three parts staged");
    CHECK(stored("bigcyr") == "<absent>", "nothing reaches flash before script.write");
    g_order.clear();
    Rep r = write_xfer("bigcyr", "cyr1", 16384, FP_BIG_CYR);
    CHECK(r.ok && r.d["data"]["size"] == 16384 && r.d["data"]["fp"].as<std::string>() == FP_BIG_CYR && r.d["data"]["name"] == "bigcyr", "staged write answers {name, size, fp}");
    CHECK(stored("bigcyr") == big, "the stored bytes are the staged bytes");
    JsonDocument d;
    CHECK(g_events.size() == 1 && g_order == "RE", "one event after the reply");
    if (event(0, "script.added", d)) CHECK(d["fp"].as<std::string>() == FP_BIG_CYR && d["size"] == 16384, "script.added carries size and fp");
    Rep again = write_xfer("bigcyr2", "cyr1", 16384, FP_BIG_CYR);
    CHECK(!again.ok && again.err == "no_xfer" && !g_fs.count("bigcyr2"), "a save used the staged text up");
    Paged p = read_paged("bigcyr");
    CHECK(p.ok && p.text == big && p.fp == FP_BIG_CYR, "a long Cyrillic script round trips byte exact: cloud split -> hub assembly -> paged read, fp equal");

    // the staged text is used up only by a save: a wrong name, base, size or fingerprint, a syntax error or a
    // full store leave it there
    reset();
    const std::string t = "-- \xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD1\x96\xD1\x82\nprint('staged')\nprint('text')\n";
    const std::string fp = sha(t);
    CHECK(stage("keep1", t, 3), "staged");
    CHECK(write_xfer("a b", "keep1", t.size(), fp).err == "bad_name", "bad name");
    put("taken", "print('old')\n");
    CHECK(write_xfer("taken", "keep1", t.size(), fp, "").err == "exists", "base \"\" on a taken name");
    CHECK(write_xfer("taken", "keep1", t.size(), fp, sha("other").c_str()).err == "changed", "a stale base");
    CHECK(write_xfer("fresh", "keep1", t.size() + 1, fp).err == "bad_part", "size off by one");
    CHECK(write_xfer("fresh", "keep1", t.size() - 1, fp).err == "bad_part", "size off by minus one");
    CHECK(write_xfer("fresh", "keep1", t.size(), sha("not it")).err == "bad_part", "a wrong fp");
    std::string upfp = fp;
    for (char& c : upfp) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    CHECK(write_xfer("fresh", "keep1", t.size(), upfp).err == "bad_part", "an UPPERCASE fp");
    { Req q; q.s("name", "fresh").s("xfer", "keep1"); h_write(1, 7, q.doc); CHECK(rep().err == "bad_part", "no size, no fp"); }
    CHECK(g_fs.size() == 1 && stored("taken") == "print('old')\n" && g_events.empty(), "none of those wrote or pushed anything");
    Rep ck = check_xfer("keep1");
    CHECK(ck.ok && ck.d["data"]["ok"] == true, "the slot is still there: script.check reads it");
    Rep ok1 = write_xfer("fresh", "keep1", t.size(), fp);
    CHECK(ok1.ok && stored("fresh") == t, "and the right call saves it");
    CHECK(write_xfer("fresh", "keep1", t.size(), fp).err == "no_xfer", "the save used it up");

    // a syntax error keeps the staged text; script.check still reads it
    reset();
    const std::string broken = "print('a')\nlocal = 1\n";
    CHECK(stage("syn1", broken, 2), "staged");
    Rep se = write_xfer("syn", "syn1", broken.size(), sha(broken));
    CHECK(!se.ok && se.err.rfind("check:2:", 0) == 0 && !g_fs.count("syn"), "check:<line>:<message>, nothing written ('%s')", se.err.c_str());
    Rep sc = check_xfer("syn1");
    CHECK(sc.ok && sc.d["data"]["ok"] == false && sc.d["data"]["line"] == 2, "the staged text can still be checked");
    CHECK(write_xfer("syn", "syn1", broken.size(), sha(broken)).err.rfind("check:2:", 0) == 0, "and still answers the same, not no_xfer");

    // a full store keeps it; making room lets the same staged text save
    reset();
    for (int i = 0; i < 16; i++) { char nm[16]; std::snprintf(nm, sizeof(nm), "s%02d", i); put(nm, "x"); }
    CHECK(stage("full1", kHelloLua, 2), "staged");
    CHECK(write_xfer("brandnew", "full1", kHelloLua.size(), sha(kHelloLua)).err == "too_many", "too_many");
    g_fs.erase("s00");
    CHECK(write_xfer("brandnew", "full1", kHelloLua.size(), sha(kHelloLua)).ok && stored("brandnew") == kHelloLua, "after room is made the same staged text saves");

    // a flash error keeps it
    reset();
    CHECK(stage("wf1", kHelloLua, 2), "staged");
    g_write_fail = true;
    CHECK(write_xfer("wf", "wf1", kHelloLua.size(), sha(kHelloLua)).err == "write_failed", "write_failed");
    g_write_fail = false;
    CHECK(write_xfer("wf", "wf1", kHelloLua.size(), sha(kHelloLua)).ok, "and a retry saves");

    // no_xfer: unknown, incomplete (and the slot waits for the rest), replaced by another transfer's part 0
    reset();
    CHECK(write_xfer("n", "nothing", 1, sha("a")).err == "no_xfer", "unknown xfer");
    CHECK(stage("inc1", kHelloLua, 3, true), "two of three parts");
    CHECK(write_xfer("n", "inc1", kHelloLua.size(), sha(kHelloLua)).err == "no_xfer", "an incomplete transfer is no_xfer");
    CHECK(check_xfer("inc1").err == "no_xfer", "script.check says no_xfer too");
    const auto parts = split_parts(kHelloLua, 3);
    CHECK(part("inc1", 2, 3, &parts[2]).ok, "the last part still fits");
    CHECK(write_xfer("n", "inc1", kHelloLua.size(), sha(kHelloLua)).ok, "then it saves");
    reset();
    CHECK(stage("old1", kHelloLua, 2), "staged");
    const std::string other = "zz";
    CHECK(part("new1", 0, 2, &other).ok, "another transfer starts");
    CHECK(write_xfer("n", "old1", kHelloLua.size(), sha(kHelloLua)).err == "no_xfer", "the first one is gone");

    // the clock: dropped 30 s after the last use; script.check counts as a use
    reset();
    CHECK(stage("clk1", kHelloLua, 2), "staged");
    advance_ms(31000);
    CHECK(write_xfer("c", "clk1", kHelloLua.size(), sha(kHelloLua)).err == "no_xfer", "31 s idle: gone");
    reset();
    CHECK(stage("clk2", kHelloLua, 2), "staged");
    advance_ms(20000);
    CHECK(check_xfer("clk2").ok, "a check at 20 s");
    advance_ms(20000);
    CHECK(write_xfer("c", "clk2", kHelloLua.size(), sha(kHelloLua)).ok, "40 s after the staging, 20 s after the check: still there");

    // out of memory on the private copy: the slot is untouched
    reset();
    CHECK(stage("oom1", kHelloLua, 2), "staged");
    g_alloc_calls = 0; g_alloc_fail_at = 1;
    CHECK(write_xfer("o", "oom1", kHelloLua.size(), sha(kHelloLua)).err == "oom", "oom");
    g_alloc_fail_at = 0;
    CHECK(write_xfer("o", "oom1", kHelloLua.size(), sha(kHelloLua)).ok, "the staged text survived it");
    reset();
    CHECK(stage("oom2", kHelloLua, 2), "staged");
    g_alloc_calls = 0; g_alloc_fail_at = 1;
    CHECK(check_xfer("oom2").err == "oom", "script.check: oom");
    g_alloc_fail_at = 0;

    // nothing slow runs under the slot's lock, and the slot is freed only if it is still this transfer's:
    // another client begins a transfer while the flash write of this one is running
    reset();
    CHECK(stage("mine", kHelloLua, 2), "staged");
    const std::string oa = "aaa", ob = "bbb";
    size_t got = 0;
    const char* during_err = "<not called>";
    g_during_write = [&] { during_err = remote_xfer_part("other", 0, 2, oa.c_str(), oa.size(), h_now_ms(), &got); };
    CHECK(write_xfer("race", "mine", kHelloLua.size(), sha(kHelloLua)).ok && stored("race") == kHelloLua, "the save completes");
    CHECK(during_err == nullptr, "a part 0 from another client was accepted DURING the write (the slot's lock is free): %s", during_err ? during_err : "ok");
    CHECK(remote_xfer_part("other", 1, 2, ob.c_str(), ob.size(), h_now_ms(), &got) == nullptr && got == 6, "and its transfer survived the save of the first");
}

// ---- script.check -------------------------------------------------------------------------------------------------
static void test_check() {
    printf("== script.check\n");
    reset();
    Rep r = check_src(kHelloLua);
    CHECK(r.ok && r.d["data"]["ok"] == true && r.d["data"]["err"] == "" && r.d["data"]["line"] == 0, "a good text");
    r = check_src("local x = = 1\n", "demo");
    CHECK(r.ok && r.d["data"]["ok"] == false && r.d["data"]["line"] == 1 &&
          r.d["data"]["err"].as<std::string>().find("[string \"demo\"]:1:") == 0, "an error: '%s'", r.d["data"]["err"].as<std::string>().c_str());
    r = check_src("x = \"abc\n");
    CHECK(r.ok && r.d["data"]["ok"] == false && r.d["data"]["err"].as<std::string>().find("\"abc") != std::string::npos, "a quote in the message");
    // the 128-byte cut can land inside a letter
    std::string src = "x = \"";
    for (int i = 0; i < 70; i++) src += "\xD0\x96";
    r = check_src(src);
    CHECK(r.ok && valid_utf8(r.d["data"]["err"].as<std::string>()), "a message cut inside a letter is still valid UTF-8");
    CHECK(r.raw.size() < 1024, "worst case fits the reply buffer");
    { Req q; h_check(1, 8, q.doc); CHECK(rep().err == "missing src", "no src, no xfer"); }
    // xfer
    reset();
    CHECK(stage("c1", kHelloLua, 2), "staged");
    r = check_xfer("c1");
    CHECK(r.ok && r.d["data"]["ok"] == true, "a staged text checks");
    r = check_xfer("c1");
    CHECK(r.ok, "and the check did not consume it");
    CHECK(check_xfer("nothing").err == "no_xfer", "no_xfer");
    const std::string badtxt = "x = = 1\n";
    CHECK(stage("c2", badtxt, 2), "staged");
    r = check_xfer("c2");
    CHECK(r.ok && r.d["data"]["ok"] == false && r.d["data"]["line"] == 1, "a staged error");
}

// ---- script.delete / script.run -------------------------------------------------------------------------------------
static void test_delete_run() {
    printf("== script.delete / script.run\n");
    reset();
    put("gone", "x");
    { Req q; q.s("name", "gone"); h_delete(1, 6, q.doc); Rep d = rep(); CHECK(d.ok && !g_fs.count("gone"), "delete"); }
    JsonDocument ev0;
    if (event(0, "script.deleted", ev0)) CHECK(ev0["name"] == "gone", "script.deleted {name}");
    { Req q; h_delete(1, 6, q.doc); CHECK(rep().err == "missing name", "delete: missing name"); }
    { Req q; q.s("name", "a/b"); h_delete(1, 6, q.doc); CHECK(rep().err == "not found", "delete: a bad name"); }
    { Req q; q.s("name", "job"); h_run(1, 6, q.doc); CHECK(rep().ok && g_run_name == "job", "run"); }
    g_run_ok = false;
    { Req q; q.s("name", "job"); h_run(1, 6, q.doc); CHECK(rep().err == "queue saturated or missing", "run: refused"); }
    { Req q; h_run(1, 6, q.doc); CHECK(rep().err == "missing name", "run: missing name"); }
}

// ---- script.reload ------------------------------------------------------------------------------------------------
static void test_reload() {
    printf("== script.reload\n");
    reset();
    h_reload(1, 11);
    Rep r = rep();
    CHECK(r.ok && r.d["data"]["loaded"] == 3 && r.d["data"]["total"] == 4, "done: {loaded, total}");
    CHECK(g_reload_wait == 5000 && g_reload_calls == 1, "waits up to 5 s (asked %u ms)", g_reload_wait);
    g_reload_res = LUA_RELOAD_QUEUED;
    h_reload(1, 12);
    r = rep();
    CHECK(r.ok && r.d["data"]["queued"] == true && r.d["data"]["loaded"].isNull(), "after 5 s: {queued: true}");
    g_reload_res = LUA_RELOAD_BUSY;
    h_reload(1, 13);
    r = rep();
    CHECK(!r.ok && r.err == "lua queue full", "TaskLua's queue is full");
    g_reload_res = LUA_RELOAD_DONE; g_reload_loaded = 16; g_reload_total = 16;
    h_reload(1, 4294967295u);
    r = rep();
    CHECK(r.ok && r.d["data"]["loaded"] == 16 && r.d["data"]["total"] == 16 && r.raw.size() < 112, "16 of 16, longest id: %zu bytes", r.raw.size());
}

// ---- script.error ------------------------------------------------------------------------------------------------
static void test_script_error() {
    printf("== script.error\n");
    reset();
    h_script_error("rrs", 12, "attempt to call a nil value (global 'x')");
    JsonDocument d;
    CHECK(g_events.size() == 1, "one event");
    if (event(0, "script.error", d)) CHECK(d["name"] == "rrs" && d["line"] == 12 && d["message"] == "attempt to call a nil value (global 'x')", "name, line, message");

    // the name is not to be trusted: a script can forge the chunk prefix with error(msg, 0)
    const char* forged[] = {"rrs\"x", "a\nb", "a b", "a.b", "../etc", "abcdefghijklmnopqrstuvwxy", "\xD0\x96", "\xFF\xFE", "a\\b", "a\x01" "b", ""};
    for (const char* nm : forged) {
        g_events.clear();
        h_script_error(nm, 3, "boom");
        JsonDocument f;
        if (g_events.size() == 1 && event(0, "script.error", f)) {
            CHECK(f["name"] == "" && f["line"] == 3 && f["message"] == "boom", "forged name '%s' is reported as \"\"", nm);
        } else CHECK(false, "forged name '%s': %zu events", nm, g_events.size());
    }
    g_events.clear();
    h_script_error(nullptr, 0, "no name");
    JsonDocument f2;
    if (event(0, "script.error", f2)) CHECK(f2["name"] == "" && f2["line"] == 0, "no name");
    g_events.clear();
    h_script_error("abcdefghijklmnopqrstuvwx", 1, "24 characters");
    JsonDocument f3;
    if (event(0, "script.error", f3)) CHECK(f3["name"] == "abcdefghijklmnopqrstuvwx", "a 24-character name is kept");
    g_events.clear();
    h_script_error("a-b_9", 1, "x");
    JsonDocument f4;
    if (event(0, "script.error", f4)) CHECK(f4["name"] == "a-b_9", "dash and underscore are kept");

    // the message may hold any byte
    g_events.clear();
    h_script_error("m", 1, "quote \" backslash \\ newline\n tab\t ctrl \x01 and \xD0 lone \xFF bytes \xE2\x82");
    JsonDocument m;
    if (event(0, "script.error", m)) {
        CHECK(m["message"].as<std::string>() == "quote \" backslash \\ newline\n tab\t ctrl \x01 and \xEF\xBF\xBD lone \xEF\xBF\xBD bytes \xEF\xBF\xBD\xEF\xBF\xBD",
              "escapes, and invalid UTF-8 as U+FFFD");
    }
    // worst case: 120 control bytes (6 bytes each) and a 24-character name, the longest line
    g_events.clear();
    h_script_error("abcdefghijklmnopqrstuvwx", -2147483647, std::string(120, '\x01').c_str());
    CHECK(g_events.size() == 1 && g_events[0].payload.size() < 1024, "worst case fits: %zu bytes", g_events.empty() ? 0 : g_events[0].payload.size());
    JsonDocument w;
    if (event(0, "script.error", w)) CHECK(w["message"].as<std::string>() == std::string(120, '\x01'), "120 control bytes round trip");
    g_events.clear();
    h_script_error("m", 1, "");
    JsonDocument e0;
    if (event(0, "script.error", e0)) CHECK(e0["message"] == "", "empty message");
    // nobody listening: nothing is built or sent
    g_events.clear();
    g_listeners = false;
    h_script_error("rrs", 1, "boom");
    CHECK(g_events.empty(), "no listener, no event");
}

// ---- the saved-script event ------------------------------------------------------------------------------------------
static void test_saved_event() {
    printf("== script.added / updated\n");
    reset();
    put("alpha", kHelloLua, 1760000000u);
    put("beta", kHelloLua, 7);
    ws_push_script_saved("alpha", kHelloLua.c_str(), kHelloLua.size(), false);
    JsonDocument d;
    if (event(0, "script.added", d)) CHECK(d["name"] == "alpha" && d["size"] == kHelloLua.size() && d["fp"].as<std::string>() == sha(kHelloLua) && d["mtime"] == 1760000000u, "added with mtime");
    g_events.clear();
    ws_push_script_saved("beta", kHelloLua.c_str(), kHelloLua.size(), true);
    JsonDocument d2;
    if (event(0, "script.updated", d2)) CHECK(d2["name"] == "beta" && d2["mtime"].isNull(), "updated, mtime 7 is not a date");
    g_events.clear();
    ws_push_script_saved("missing", "x", 1, false);   // not in the list (deleted in between): no mtime
    JsonDocument d3;
    if (event(0, "script.added", d3)) CHECK(d3["mtime"].isNull() && d3["fp"].as<std::string>() == sha("x"), "a name that is gone: no mtime");
    g_events.clear();
    g_psa_fail = true;
    ws_push_script_saved("alpha", "x", 1, false);
    JsonDocument d4;
    if (event(0, "script.added", d4)) CHECK(d4["fp"] == "", "a failed hash: an empty fp, still valid JSON");
    g_psa_fail = false;
    g_events.clear();
    g_listeners = false;
    ws_push_script_saved("alpha", "x", 1, false);
    CHECK(g_events.empty(), "no listener, no event");
}

// ---- REST -------------------------------------------------------------------------------------------------------------
enum { M_DELETE = 0, M_GET = 1, M_POST = 3, M_PUT = 4 };
static void test_rest() {
    printf("== REST (api_scripts.cpp)\n");
    reset();
    const std::string full = "--" + std::string(16382, 'a');
    RestResult r = rest_call(M_PUT, "/api/scripts/foo", full);
    CHECK(r.status == 200 && r.body == "{\"ok\":true}" && stored("foo") == full, "PUT of 16,384 bytes saves (status %d)", r.status);
    JsonDocument d;
    CHECK(g_events.size() == 1, "one event");
    if (event(0, "script.added", d)) CHECK(d["name"] == "foo" && d["size"] == 16384 && d["fp"].as<std::string>() == sha(full), "REST save: script.added now with size and fp");
    g_events.clear();
    r = rest_call(M_POST, "/api/scripts/foo", "print('v2')\n");   // the SPA's POST
    CHECK(r.status == 200 && stored("foo") == "print('v2')\n", "POST saves like PUT");
    JsonDocument d2;
    if (event(0, "script.updated", d2)) CHECK(d2["size"] == 12 && d2["fp"].as<std::string>() == sha("print('v2')\n"), "script.updated with size and fp");

    // over 16,384 bytes: 413, nothing stored, nothing pushed (it used to store the first 16,383 bytes and say ok)
    g_events.clear();
    r = rest_call(M_PUT, "/api/scripts/foo", std::string(16385, 'a'));
    CHECK(r.status == 413, "16,385 bytes: status %d", r.status);
    CHECK(stored("foo") == "print('v2')\n" && g_events.empty(), "the old script is untouched, nothing pushed");
    r = rest_call(M_PUT, "/api/scripts/newone", "x", 20000);   // Content-Length says 20,000
    CHECK(r.status == 413 && !g_fs.count("newone"), "a Content-Length over the cap is refused before any read");
    r = rest_call(M_POST, "/api/scripts/foo/check", "x", 16385);
    CHECK(r.status == 413, "check: 16,385 bytes is 413 too (status %d)", r.status);

    // a NUL in the body: the store keeps the text up to it, so size and fp are of that text
    g_events.clear();
    const std::string nul("abc\0def", 7);
    r = rest_call(M_PUT, "/api/scripts/nulled", nul);
    CHECK(r.status == 200 && stored("nulled") == "abc", "the store keeps the text up to the NUL");
    JsonDocument d3;
    if (event(0, "script.added", d3)) CHECK(d3["size"] == 3 && d3["fp"].as<std::string>() == sha("abc"), "size and fp describe what is stored (size %d)", d3["size"] | -1);

    // a full-size read (it was 404: the buffer held 16,383 bytes)
    put("fullread", full);
    r = rest_call(M_GET, "/api/scripts/fullread", "");
    CHECK(r.status == 200 && r.body == full, "GET of a 16,384-byte script (status %d, %zu bytes)", r.status, r.body.size());
    r = rest_call(M_GET, "/api/scripts/absent", "");
    CHECK(r.status == 404, "GET of a missing script");

    // a full-size check reads every byte: the 16,384th is the one that breaks it (the old cap stopped at 16,383)
    const std::string tail_err = "--" + std::string(16380, 'a') + "\n)";
    CHECK(tail_err.size() == 16384, "test text is 16,384 bytes");
    r = rest_call(M_POST, "/api/scripts/chk/check", tail_err);
    JsonDocument cd;
    CHECK(r.status == 200 && !deserializeJson(cd, r.body) && cd["ok"] == false && cd["line"] == 2, "check sees the last byte: %s", r.body.c_str());
    r = rest_call(M_POST, "/api/scripts/chk/check", full);
    JsonDocument cd2;
    CHECK(r.status == 200 && !deserializeJson(cd2, r.body) && cd2["ok"] == true, "a full-size good text checks ok");

    // the rest of the surface
    r = rest_call(M_POST, "/api/scripts/job/run", "");
    CHECK(r.status == 200 && g_run_name == "job" && r.body == "{\"ok\":true,\"queued\":true}", "run");
    g_events.clear();
    r = rest_call(M_DELETE, "/api/scripts/foo", "");
    CHECK(r.status == 200 && !g_fs.count("foo") && g_events.size() == 1 && g_events[0].event == "script.deleted", "delete");
    r = rest_call(M_GET, "/api/scripts", "");
    JsonDocument ld;
    CHECK(r.status == 200 && !deserializeJson(ld, r.body) && ld.is<JsonArray>(), "list: %s", r.body.c_str());
    r = rest_call(M_PUT, "/api/scripts/bad%20name", "x");
    CHECK(r.status == 500 && !g_fs.count("bad%20name"), "a name the store refuses: write failed (status %d)", r.status);
    r = rest_call(M_PUT, "/api/scripts/", "x");
    CHECK(r.status == 400, "no name: 400");
}

// ---- REST: a body that does not arrive in full -------------------------------------------------------------------
// A receive timeout (the hub's httpd waits 10 s for data) or a client that closes mid-upload used to end the body
// right there: the cut text was saved over the stored script, script.updated was pushed for it, and the answer
// was 200 ok (and /check judged the cut text). Now nothing is written, nothing is pushed, and the answer is 408
// (timeout) or 400.
static void test_rest_body() {
    printf("== REST: a body that does not arrive in full\n");
    reset();
    const std::string old = "print('old')\n";
    const std::string fresh = "print('a new, longer text')\n";
    put("foo", old);
    const std::string checked = fresh + "x = = 1\n";   // longer than every cut point, and a syntax error: a verdict on a cut text would show

    struct Cut { const char* what; int code; int status; const char* msg; };
    const Cut cuts[] = {
        {"timeout", -3, 408, "body timed out"},
        {"client closed", 0, 400, "body incomplete"},
        {"receive failed", -1, 400, "body incomplete"},
        {"invalid receive", -2, 400, "body incomplete"},
    };
    // nothing arrived, a cut text arrived, everything but the last byte arrived
    const long points[] = {0, 5, static_cast<long>(fresh.size()) - 1};
    for (const Cut& c : cuts) {
        for (long at : points) {
            RestWire w;
            w.cut_after = at;
            w.cut_code = c.code;
            for (int method : {M_PUT, M_POST}) {
                g_events.clear();
                RestResult r = rest_call(method, "/api/scripts/foo", fresh, -1, w);
                CHECK(r.status == c.status && r.body == c.msg, "save, %s after %ld bytes: status %d '%s'", c.what, at, r.status, r.body.c_str());
                CHECK(stored("foo") == old && g_events.empty(), "save, %s after %ld bytes: the stored script is untouched and nothing is pushed", c.what, at);
            }
            g_events.clear();
            RestResult nr = rest_call(M_PUT, "/api/scripts/fresh", fresh, -1, w);
            CHECK(nr.status == c.status && !g_fs.count("fresh") && g_events.empty(), "new name, %s after %ld bytes: nothing created, nothing pushed (status %d)", c.what, at, nr.status);
            RestResult cr = rest_call(M_POST, "/api/scripts/foo/check", checked, -1, w);
            CHECK(cr.status == c.status && cr.body == c.msg, "check, %s after %ld bytes: no verdict on a cut text (status %d '%s')", c.what, at, cr.status, cr.body.c_str());
        }
    }

    // a full-size upload cut one byte short, and one cut half way
    const std::string full = "--" + std::string(16382, 'a');
    {
        RestWire w; w.cut_after = 16383; w.cut_code = 0;
        g_events.clear();
        RestResult r = rest_call(M_PUT, "/api/scripts/foo", full, -1, w);
        CHECK(r.status == 400 && stored("foo") == old && g_events.empty(), "16,383 of 16,384 bytes: refused, not saved (status %d)", r.status);
        w.cut_after = 8000; w.cut_code = -3;
        r = rest_call(M_PUT, "/api/scripts/foo", full, -1, w);
        CHECK(r.status == 408 && stored("foo") == old && g_events.empty(), "a timeout half way through 16,384 bytes (status %d)", r.status);
    }

    // the whole body still saves, however the receives cut it up (and no receive is made after the last byte)
    for (size_t chunk : {size_t(1), size_t(7), size_t(1460), static_cast<size_t>(-1)}) {
        put("foo", old);
        g_events.clear();
        RestWire w; w.chunk = chunk;
        RestResult r = rest_call(M_PUT, "/api/scripts/foo", fresh, -1, w);
        CHECK(r.status == 200 && r.body == "{\"ok\":true}" && stored("foo") == fresh, "whole body in receives of %zu bytes saves (status %d)", chunk, r.status);
        JsonDocument d;
        CHECK(g_events.size() == 1, "one event");
        if (event(0, "script.updated", d)) CHECK(d["size"] == fresh.size() && d["fp"].as<std::string>() == sha(fresh), "event with the size and fp of the whole text");
    }
    {
        put("foo", old);
        g_events.clear();
        RestWire w; w.chunk = 1460; w.cut_after = static_cast<long>(full.size()); w.cut_code = -3;   // a timeout would follow the LAST byte
        RestResult r = rest_call(M_PUT, "/api/scripts/foo", full, -1, w);
        CHECK(r.status == 200 && stored("foo") == full, "16,384 bytes in 1,460-byte segments save, and the handler does not read past the end (status %d)", r.status);
        JsonDocument d;
        if (g_events.size() == 1 && event(0, "script.updated", d)) CHECK(d["size"] == 16384 && d["fp"].as<std::string>() == sha(full), "full-size event");
        RestResult c = rest_call(M_POST, "/api/scripts/foo/check", full, -1, w);
        JsonDocument cd;
        CHECK(c.status == 200 && !deserializeJson(cd, c.body) && cd["ok"] == true, "a whole full-size body is checked");
    }

    // A chunked upload (Transfer-Encoding, no Content-Length; `curl -T -`): esp_http_server reports content_len 0
    // and cannot read the chunks, so the body arrived empty, blanked the stored script and answered ok. Now 411
    // and nothing is written or pushed. An explicit empty body (Content-Length: 0) still saves an empty script.
    {
        put("foo", old);
        g_events.clear();
        RestWire w; w.chunked = true;
        for (int method : {M_PUT, M_POST}) {
            RestResult r = rest_call(method, "/api/scripts/foo", fresh, 0, w);
            CHECK(r.status == 411 && stored("foo") == old && g_events.empty(),
                  "a chunked save is refused 411, the old text kept, nothing pushed (status %d '%s')", r.status, r.body.c_str());
        }
        RestResult c = rest_call(M_POST, "/api/scripts/foo/check", fresh, 0, w);
        CHECK(c.status == 411, "a chunked check is refused too (status %d)", c.status);
        RestResult e = rest_call(M_PUT, "/api/scripts/foo", "", 0);
        CHECK(e.status == 200 && stored("foo").empty(), "Content-Length: 0 saves an empty script, as before (status %d)", e.status);
    }
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);   // a mutant that crashes must still show what it broke first
    if (getenv("ONLY_REST")) {   // the RED run against an older api_scripts.cpp
        test_rest();
        test_rest_body();
        reset();
        printf("%d mismatches in %d checks (REST only)\n", g_bad, g_checks);
        return g_bad;
    }
    test_helpers();
    test_list();
    test_read_plain();
    test_read_paged();
    test_part();
    test_write_src();
    test_write_staged();
    test_check();
    test_delete_run();
    test_reload();
    test_script_error();
    test_saved_event();
    test_rest();
    test_rest_body();
    reset();
    if (g_bad) printf("%d MISMATCHES in %d checks\n", g_bad, g_checks);
    else printf("ALL OK (%d checks, 0 mismatches)\n", g_checks);
    return g_bad;
}
