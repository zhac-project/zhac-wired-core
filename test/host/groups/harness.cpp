// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Group JSON at the sizes the store allows (review F10): grp_to_json() and
// group_list_json() are cut verbatim out of main/ (run.sh); the store under
// them is a plain array here. Every output must be complete, valid JSON that
// fits its buffer, or a reported failure (0) -- never a broken list or a
// length past the buffer.
#include <ArduinoJson.h>
#include <cstdio>
#include <cstring>
#include <string>
#include "groups_store.h"
#include "device_cmd.h"

size_t group_list_json(char* out, size_t cap);
size_t group_cmd(const char* body, size_t len, char* out, size_t cap);

GrpRecord g_store[GRP_MAX_GROUPS];
uint16_t  g_count = 0;
void grp_store_lock() {}
void grp_store_unlock() {}
uint16_t grp_load_all(GrpRecord* out, uint16_t max) {
    const uint16_t n = g_count < max ? g_count : max;
    std::memcpy(out, g_store, n * sizeof(GrpRecord));
    return n;
}

bool grp_find(uint16_t id, GrpRecord& out) {
    for (uint16_t i = 0; i < g_count; i++) if (g_store[i].id == id) { out = g_store[i]; return true; }
    return false;
}
// The device layer: remember what each member was sent.
DevCmdValue device_cmd_number(double d) { DevCmdValue v{}; v.kind = DEVCMD_FLOAT; v.f = d; return v; }
static std::string g_sent;
DevCmdResult device_cmd_set_attr(uint64_t, uint8_t, const char* key, const DevCmdValue* v) {
    char one[64];
    if (v->kind == DEVCMD_STR) std::snprintf(one, sizeof(one), "%s=%s;", key, v->s);
    else std::snprintf(one, sizeof(one), "%s=%lld;", key, (long long)v->i);
    g_sent += one;
    return DEVCMD_OK;
}
static std::string cmd(const char* body) {
    char out[128];
    g_sent.clear();
    const size_t n = group_cmd(body, std::strlen(body), out, sizeof(out));
    return std::string(out, n);
}

static int g_fail = 0;
static void expect(const char* what, bool ok) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) g_fail++;
}
static GrpRecord group(uint16_t id, const char* name, uint8_t members) {
    GrpRecord r{};
    r.id = id;
    std::strncpy(r.name, name, sizeof(r.name) - 1);
    r.member_count = members;
    for (uint8_t i = 0; i < members; i++) r.members[i] = {0xA4C1380000000000ull + i, 255};
    return r;
}
static bool valid(const char* s, size_t n, size_t want_groups) {
    JsonDocument d;
    return !deserializeJson(d, s, n) && d["groups"].as<JsonArray>().size() == want_groups;
}

int main() {
    static char big[GRP_LIST_JSON_MAX];
    char one[GRP_JSON_MAX];

    // The worst single record: 16 members, a name that is all escapes.
    GrpRecord worst = group(65535, "", GRP_MAX_MEMBERS);
    std::memset(worst.name, 0x01, sizeof(worst.name) - 1);
    const size_t n1 = grp_to_json(worst, one, sizeof(one));
    JsonDocument d1;
    expect("the largest record fits GRP_JSON_MAX", n1 > 0 && !deserializeJson(d1, one, n1) &&
                                                   d1["members"].as<JsonArray>().size() == GRP_MAX_MEMBERS);

    g_count = 0;
    g_store[g_count++] = group(1, "a", 7);       // 290 bytes: past the old 256-byte row buffer
    g_store[g_count++] = group(2, "b", 1);
    size_t n = group_list_json(big, sizeof(big));
    expect("a seven-member group is listed, valid JSON", n > 0 && n < sizeof(big) && valid(big, n, 2));

    g_count = 0;
    for (uint16_t i = 0; i < GRP_MAX_GROUPS; i++) g_store[g_count++] = group(i + 1, "kitchn", 4);
    n = group_list_json(big, 4096);               // the old REST buffer
    expect("a list that does not fit is a failure, not a length past the buffer", n == 0);
    n = group_list_json(big, sizeof(big));
    expect("32 groups fit GRP_LIST_JSON_MAX", n > 0 && valid(big, n, GRP_MAX_GROUPS));

    g_count = 0;
    for (uint16_t i = 0; i < GRP_MAX_GROUPS; i++) { g_store[g_count++] = worst; g_store[i].id = i + 1; }
    n = group_list_json(big, sizeof(big));
    expect("32 worst-case groups fit GRP_LIST_JSON_MAX", n > 0 && valid(big, n, GRP_MAX_GROUPS));

    // Group commands (review F34), two members.
    g_count = 0;
    g_store[g_count++] = group(7, "hall", 2);
    cmd("{\"id\":7,\"ep\":1,\"cluster\":6,\"cmd\":1}");
    expect("the On button turns members on", g_sent == "state=ON;state=ON;");
    cmd("{\"id\":7,\"ep\":1,\"cluster\":6,\"cmd\":0}");
    expect("the Off button turns them off", g_sent == "state=OFF;state=OFF;");
    cmd("{\"id\":7,\"ep\":1,\"cluster\":6,\"cmd\":2}");
    expect("Toggle toggles", g_sent == "state=TOGGLE;state=TOGGLE;");
    cmd("{\"id\":7,\"key\":\"brightness\",\"val\":128}");
    expect("a key/val set reaches every member", g_sent == "brightness=128;brightness=128;");
    for (const char* bad : {"{\"id\":7}", "{\"id\":7,\"val\":null}", "{\"id\":7,\"val\":{}}",
                            "{\"id\":7,\"val\":[1]}", "{\"id\":7,\"cluster\":8,\"cmd\":1}"}) {
        const std::string r = cmd(bad);
        expect(bad, g_sent.empty() && r.find("\"ok\":false") != std::string::npos);
    }

    std::printf(g_fail ? "\n%d FAILURE(S)\n" : "\ngroup JSON and commands OK\n", g_fail);
    return g_fail;
}
