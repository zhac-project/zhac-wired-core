// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// What "Allow script changes from the cloud" does when NVS cannot be written (spec 2026-10-05 §3.1), and the
// cloud-control level and "Allow the cloud to disarm the alarm" (review 2026-11-26 HL-03, HL-02).
// Runs the REAL main/sys_state.cpp and the REAL system_apply_settings (main/api_system.cpp) against a fake NVS
// that can fail at open, set and commit. A cold boot is sys_state_init() again over the same fake flash.
// Exit code = number of mismatches. See run.sh.
#include "stubs.h"
#include "sys_state.h"
#include "log_cloud.h"
#include "remote_allow.h"   // the real zhac-components one: the keys the relay keeps local

#include <map>
#include <string>
#include <vector>

bool system_apply_settings(const char* json, size_t len);   // slice_tu.cpp

// ---- log and hub.caps pushes ---------------------------------------------------------------------------
static std::vector<std::string> g_log;
static std::vector<bool> g_pushes;   // sys_remote_scripts() at each ws_push_hub_caps()

void harness_log(char level, const char* tag, const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    g_log.push_back(std::string(1, level) + " (" + tag + ") " + buf);
}

const char* esp_err_to_name(esp_err_t code) {
    switch (code) {
        case ESP_OK: return "ESP_OK";
        case ESP_FAIL: return "ESP_FAIL";
        case ESP_ERR_NVS_NOT_INITIALIZED: return "ESP_ERR_NVS_NOT_INITIALIZED";
        case ESP_ERR_NVS_NOT_FOUND: return "ESP_ERR_NVS_NOT_FOUND";
        case ESP_ERR_NVS_NOT_ENOUGH_SPACE: return "ESP_ERR_NVS_NOT_ENOUGH_SPACE";
        default: return "ESP_ERR_UNKNOWN";
    }
}

static int g_lc_setting = 0;   // log_cloud_on_setting() calls
void log_cloud_on_setting() { g_lc_setting++; }

void ws_push_hub_caps() { g_pushes.push_back(sys_remote_scripts()); }

// ---- fake NVS --------------------------------------------------------------------------------------------
struct Fail { esp_err_t open, set, commit; };   // injected result per stage, ESP_OK = works
static Fail g_fail{ESP_OK, ESP_OK, ESP_OK};
static esp_err_t g_fail_ro = ESP_OK;             // read-only opens (NVS not usable at boot)
static std::map<std::string, uint8_t> g_flash;   // durable "namespace/key" -> u8
static std::map<std::string, std::string> g_flash_str;   // durable strings, set by the cases (nvs_set_str stores none)
static int g_opens_rw = 0;                       // read-write opens (a settings write starts with one)
static int g_flash_writes = 0;                   // commits that changed a durable value
struct Handle { std::string ns; std::map<std::string, uint8_t> pending; };
static std::map<nvs_handle_t, Handle> g_open;
static nvs_handle_t g_next = 1;

extern "C" {
esp_err_t nvs_open(const char* ns, nvs_open_mode_t mode, nvs_handle_t* h) {
    if (mode == NVS_READWRITE) {
        g_opens_rw++;
        if (g_fail.open != ESP_OK) return g_fail.open;
    } else if (g_fail_ro != ESP_OK) {
        return g_fail_ro;
    }
    *h = g_next++;
    g_open[*h] = Handle{ns, {}};
    return ESP_OK;
}
esp_err_t nvs_set_u8(nvs_handle_t h, const char* key, uint8_t v) {
    if (g_fail.set != ESP_OK) return g_fail.set;
    Handle& o = g_open.at(h);
    o.pending[o.ns + "/" + key] = v;
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h) {
    if (g_fail.commit != ESP_OK) return g_fail.commit;   // the pending write is lost with the handle
    Handle& o = g_open.at(h);
    for (auto& kv : o.pending) {
        auto it = g_flash.find(kv.first);
        if (it == g_flash.end() || it->second != kv.second) {   // NVS skips a write of the same value
            g_flash[kv.first] = kv.second;
            g_flash_writes++;
        }
    }
    o.pending.clear();
    return ESP_OK;
}
void nvs_close(nvs_handle_t h) { g_open.erase(h); }
esp_err_t nvs_get_u8(nvs_handle_t h, const char* key, uint8_t* out) {
    auto it = g_flash.find(g_open.at(h).ns + "/" + key);
    if (it == g_flash.end()) return ESP_ERR_NVS_NOT_FOUND;
    *out = it->second;
    return ESP_OK;
}
esp_err_t nvs_get_str(nvs_handle_t h, const char* key, char* out, size_t* len) {
    auto it = g_flash_str.find(g_open.at(h).ns + "/" + key);
    if (it == g_flash_str.end()) return ESP_ERR_NVS_NOT_FOUND;
    const size_t need = it->second.size() + 1;
    if (!out) { *len = need; return ESP_OK; }   // as NVS: a null buffer asks for the length
    if (*len < need) return ESP_FAIL;
    memcpy(out, it->second.c_str(), need);
    *len = need;
    return ESP_OK;
}
esp_err_t nvs_set_str(nvs_handle_t, const char*, const char*) { return ESP_OK; }
}

// ---- neighbours with nothing to do here ------------------------------------------------------------------
bool auth_enabled() { return false; }
void auth_set_enabled(bool) {}
size_t auth_token_copy(char*, size_t) { return 0; }
bool auth_rotate_token(char*, size_t) { return false; }
void mqtt_glue_apply_settings(JsonDocument&) {}
bool ntp_cfg_set_server(const char*) { return true; }
extern "C" void metrics_mqtt_publisher_start() {}
void log_sinks_set_mqtt(bool, char) {}
void log_sinks_set_ws(bool, char) {}
bool log_sinks_get_mqtt_enabled() { return false; }
bool log_sinks_get_ws_enabled() { return false; }
char log_sinks_get_mqtt_level() { return 'I'; }
char log_sinks_get_ws_level() { return 'I'; }

// ---- the cases -------------------------------------------------------------------------------------------
static const char* kKey = "sys_cfg/remote_scripts";
static int g_mismatches = 0;

static int nvs_now() {
    auto it = g_flash.find(kKey);
    return it == g_flash.end() ? -1 : it->second;
}
static std::string nv_s(int v) { return v < 0 ? "missing" : std::to_string(v); }
static std::string pushes_s(const std::vector<bool>& p) {
    std::string s = "[";
    for (size_t i = 0; i < p.size(); i++) s += (i ? "," : "") + std::string(p[i] ? "on" : "off");
    return s + "]";
}

// A hub that has just booted over this NVS content (-1 = key missing). The counters count what comes after the
// boot (which writes the cloud-control level once on a flash without it).
static void world(int nvs) {
    g_flash.clear();
    g_flash_str.clear();
    if (nvs >= 0) g_flash[kKey] = static_cast<uint8_t>(nvs);
    g_fail = Fail{ESP_OK, ESP_OK, ESP_OK};
    g_fail_ro = ESP_OK;
    sys_state_init();
    g_opens_rw = g_flash_writes = 0;
    g_pushes.clear();
    g_log.clear();
}
static bool apply(const char* json) { return system_apply_settings(json, strlen(json)); }
static bool logged(const char* needle) {
    for (auto& l : g_log) if (l.find(needle) != std::string::npos) return true;
    return false;
}

static void header() {
    printf("%-44s | %-5s | %-3s | %-7s | %-10s | %-6s |\n", "case", "ret", "RAM", "NVS", "hub.caps", "reboot");
    printf("%.44s-+-%.5s-+-%.3s-+-%.7s-+-%.10s-+-%.6s-+-\n", "--------------------------------------------",
           "-----", "---", "-------", "----------", "------");
}
static void row(const char* name, bool ret, bool ram, int nv, const std::vector<bool>& pushes, bool after, bool ok) {
    printf("%-44s | %-5s | %-3s | %-7s | %-10s | %-6s | %s\n", name, ret ? "true" : "false", ram ? "on" : "off",
           nv_s(nv).c_str(), pushes_s(pushes).c_str(), after ? "on" : "off", ok ? "ok" : "MISMATCH");
    if (!ok) g_mismatches++;
}
static void show_errors(const std::vector<std::string>& log) {
    for (auto& l : log) if (l[0] == 'E') printf("      %s\n", l.c_str());
}

struct Expect {
    bool ret, ram;
    int nvs;
    std::vector<bool> pushes;
    bool after_reboot;
    const char* log_has;   // must appear in the log, or nullptr
    const char* log_has2;
};

// One request over a hub that booted with `nvs0`, NVS failing as `f`; then a cold boot with NVS healthy again.
static void run(const char* name, int nvs0, const char* json, Fail f, const Expect& x) {
    world(nvs0);
    g_fail = f;
    const bool ret = apply(json);
    const bool ram = sys_remote_scripts();
    const int nv = nvs_now();
    const std::vector<bool> pushes = g_pushes;
    const std::vector<std::string> log = g_log;
    const bool has = (!x.log_has || logged(x.log_has)) && (!x.log_has2 || logged(x.log_has2));
    g_fail = Fail{ESP_OK, ESP_OK, ESP_OK};
    sys_state_init();   // reboot
    const bool after = sys_remote_scripts();
    row(name, ret, ram, nv, pushes, after,
        ret == x.ret && ram == x.ram && nv == x.nvs && pushes == x.pushes && after == x.after_reboot && has);
    show_errors(log);
}

int main() {
    const Fail none{ESP_OK, ESP_OK, ESP_OK};
    const Fail f_open{ESP_ERR_NVS_NOT_INITIALIZED, ESP_OK, ESP_OK};
    const Fail f_set{ESP_OK, ESP_ERR_NVS_NOT_ENOUGH_SPACE, ESP_OK};
    const Fail f_commit{ESP_OK, ESP_OK, ESP_FAIL};
    const char* ON = "{\"remote_scripts\":true}";
    const char* OFF = "{\"remote_scripts\":false}";

    printf("ret = system_apply_settings() result (false = the page gets \"bad settings\", REST 400)\n");
    printf("RAM/NVS/hub.caps = right after the request; reboot = RAM after a cold boot over the NVS content\n\n");
    header();
    // The four branches of the ruling.
    run("save ok, ON  (nvs missing)", -1, ON, none, {true, true, 1, {true}, true, nullptr, nullptr});
    run("save ok, OFF (nvs 1)", 1, OFF, none, {true, false, 0, {false}, false, nullptr, nullptr});
    run("save FAILS at set,    ON  (nvs 0)", 0, ON, f_set,
        {false, false, 0, {}, false, "ESP_ERR_NVS_NOT_ENOUGH_SPACE", "it stays off"});
    run("save FAILS at open,   ON  (nvs 0)", 0, ON, f_open,
        {false, false, 0, {}, false, "ESP_ERR_NVS_NOT_INITIALIZED", "it stays off"});
    run("save FAILS at commit, ON  (nvs 0)", 0, ON, f_commit,
        {false, false, 0, {}, false, "ESP_FAIL", "it stays off"});
    run("save FAILS at set,    OFF (nvs 1)", 1, OFF, f_set,
        {false, false, 1, {false}, true, "ESP_ERR_NVS_NOT_ENOUGH_SPACE", "may be on again after a reboot"});
    run("save FAILS at open,   OFF (nvs 1)", 1, OFF, f_open,
        {false, false, 1, {false}, true, "ESP_ERR_NVS_NOT_INITIALIZED", "may be on again after a reboot"});
    run("save FAILS at commit, OFF (nvs 1)", 1, OFF, f_commit,
        {false, false, 1, {false}, true, "ESP_FAIL", "may be on again after a reboot"});

    // After a failed OFF (RAM off, NVS 1): what the owner can do next.
    {
        world(1);
        g_fail = f_set;
        const bool first = apply(OFF);
        g_fail = none;
        g_log.clear();
        const std::vector<bool> before = g_pushes;
        const bool again = apply(OFF);   // sends OFF again, NVS healthy now
        const bool ram = sys_remote_scripts();
        const int nv = nvs_now();
        const std::vector<bool> pushes = g_pushes;
        sys_state_init();
        row("...then OFF again, NVS healthy", again, ram, nv, pushes, sys_remote_scripts(),
            !first && again && !ram && nv == 0 && pushes == before && !sys_remote_scripts() && g_flash_writes == 1);
    }
    {
        world(1);
        g_fail = f_set;
        const bool first = apply(OFF);
        g_fail = none;
        const bool on_again = apply(ON);   // the page's toggle: back to on, NVS healthy
        const bool ram = sys_remote_scripts();
        const int nv = nvs_now();
        const std::vector<bool> pushes = g_pushes;
        sys_state_init();
        row("...then ON, NVS healthy (no flash write)", on_again, ram, nv, pushes, sys_remote_scripts(),
            !first && on_again && ram && nv == 1 && pushes == std::vector<bool>({false, true}) &&
            sys_remote_scripts() && g_flash_writes == 0);
    }

    // Requests that change nothing, and the odd values.
    {
        world(1);
        const bool r = apply(ON);
        row("ON while on: no NVS call, no push", r, sys_remote_scripts(), nvs_now(), g_pushes, sys_remote_scripts(),
            r && sys_remote_scripts() && g_opens_rw == 0 && g_pushes.empty());
    }
    {
        world(0);
        const bool r = apply(OFF);
        row("OFF while off and saved: no flash write", r, sys_remote_scripts(), nvs_now(), g_pushes,
            sys_remote_scripts(), r && !sys_remote_scripts() && g_flash_writes == 0 && g_pushes.empty() && g_log.empty());
    }
    {
        world(1);
        const bool r = apply("{\"remote_scripts\":\"true\"}");   // a string is not JSON true
        row("\"true\" (string) while on: off", r, sys_remote_scripts(), nvs_now(), g_pushes, sys_remote_scripts(),
            r && !sys_remote_scripts() && nvs_now() == 0 && g_pushes == std::vector<bool>({false}));
    }
    {
        world(1);
        const bool r = apply("{\"remote_scripts\":null}");
        row("null while on: untouched", r, sys_remote_scripts(), nvs_now(), g_pushes, sys_remote_scripts(),
            r && sys_remote_scripts() && g_opens_rw == 0 && g_pushes.empty());
    }
    {
        world(1);
        const bool r = apply("{}");
        row("key absent while on: untouched", r, sys_remote_scripts(), nvs_now(), g_pushes, sys_remote_scripts(),
            r && sys_remote_scripts() && g_opens_rw == 0 && g_pushes.empty());
    }

    // The other flags still ignore a failed save, exactly as before the fix (no log line, RAM applied).
    {
        world(-1);
        g_fail = f_set;
        sys_set_metrics_enabled(true);
        sys_set_ap_disabled(true);
        sys_set_metrics_mqtt_enabled(true);
        sys_set_metrics_mqtt_interval_s(30);
        const bool r = apply("{\"metrics_enabled\":true}");
        const bool ok = r && sys_metrics_enabled() && sys_ap_disabled() && sys_metrics_mqtt_enabled() &&
                        sys_metrics_mqtt_interval_s() == 30 && g_log.empty() && !sys_remote_scripts();
        printf("%-44s | ret=%s, RAM flags applied, NVS untouched, no log line | %s\n",
               "other flags, NVS failing (unchanged)", r ? "true" : "false", ok ? "ok" : "MISMATCH");
        if (!ok) g_mismatches++;
    }

    // "Log to cloud" (spec 2026-10-06 §4.1): same fake NVS, key sys_cfg/log_cloud.
    {
        const char* kLc = "sys_cfg/log_cloud";
        auto lc_row = [&](const char* name, bool ok) {
            printf("%-44s | %s\n", name, ok ? "ok" : "MISMATCH");
            if (!ok) g_mismatches++;
        };
        auto lc_nv = [&]() { auto it = g_flash.find(kLc); return it == g_flash.end() ? -1 : (int)it->second; };
        printf("\nlog_to_cloud\n");
        world(-1);
        lc_row("missing key = off", sys_log_cloud() == 0);
        for (const char* bad : {"{\"log_to_cloud\":\"loud\"}", "{\"log_to_cloud\":2}", "{\"log_to_cloud\":true}",
                                "{\"log_to_cloud\":\"\"}", "{\"log_to_cloud\":\"Errors\"}"}) {
            world(-1);
            g_flash[kLc] = 1;
            sys_state_init();
            g_lc_setting = 0;
            const bool r = apply(bad);
            lc_row(bad, !r && sys_log_cloud() == 1 && lc_nv() == 1 && g_lc_setting == 0 && g_pushes.empty());
        }
        world(-1);
        bool r = apply("{\"log_to_cloud\":null}");
        lc_row("null counts as absent", r && sys_log_cloud() == 0 && g_opens_rw == 0 && g_lc_setting == 0);
        world(-1);
        r = apply("{\"log_to_cloud\":\"warnings\"}");
        lc_row("good value: saved, applied, backlog hook, caps push",
               r && sys_log_cloud() == 2 && lc_nv() == 2 && g_lc_setting == 1 && g_pushes.size() == 1);
        r = apply("{\"log_to_cloud\":\"off\"}");
        lc_row("then off: saved, applied, hook called again",
               r && sys_log_cloud() == 0 && lc_nv() == 0 && g_lc_setting == 2 && g_pushes.size() == 2);
        world(-1);
        g_flash[kLc] = 3;
        sys_state_init();
        g_lc_setting = 0;
        g_pushes.clear();
        r = apply("{\"log_to_cloud\":\"everything\"}");
        lc_row("same level again: saved (repair), no hook, no push",
               r && sys_log_cloud() == 3 && g_lc_setting == 0 && g_pushes.empty() && g_opens_rw == 1);
        for (const Fail& f : {f_open, f_set, f_commit}) {
            world(-1);
            g_flash[kLc] = 3;
            sys_state_init();
            g_lc_setting = 0;
            g_fail = f;
            r = apply("{\"log_to_cloud\":\"off\"}");
            lc_row("save FAILS, off: refused, level unchanged", !r && sys_log_cloud() == 3 && lc_nv() == 3 &&
                   g_lc_setting == 0 && g_pushes.empty() && logged("not saved"));
            g_fail = none;
        }
        world(-1);
        g_fail = f_set;
        r = apply("{\"log_to_cloud\":\"errors\"}");
        lc_row("save FAILS, on: refused, stays off", !r && sys_log_cloud() == 0 && lc_nv() == -1 && g_lc_setting == 0);
        g_fail = none;
    }

    // "Cloud control" (review 2026-11-26 HL-03, ruling R18) and "Allow the cloud to disarm the alarm" (HL-02, R19):
    // same fake NVS, keys sys_cfg/remote_level and sys_cfg/remote_alarm; remote_client keeps its token in
    // zhac_remote/token. The relay's own hooks (remote_level_now, remote_alarm_allowed) are the real lines of
    // ws_bridge.cpp and the real remote_allow.cpp.
    {
        const char* kLv = "sys_cfg/remote_level";
        const char* kAl = "sys_cfg/remote_alarm";
        const char* kTok = "zhac_remote/token";
        const char* tok = "0123456789abcdef0123456789abcdef";
        auto ck = [&](const char* name, bool ok) {
            printf("%-44s | %s\n", name, ok ? "ok" : "MISMATCH");
            if (!ok) {
                g_mismatches++;
                show_errors(g_log);
            }
        };
        auto nv = [&](const char* k) { auto it = g_flash.find(k); return it == g_flash.end() ? -1 : (int)it->second; };
        auto fresh = [&](const char* token) {   // a flash with nothing in it but the cloud token, when given
            g_flash.clear();
            g_flash_str.clear();
            if (token) g_flash_str[kTok] = token;
        };
        auto boot = [&](Fail f, esp_err_t ro) {   // a cold boot over the flash as it is; the counters count the boot
            g_fail = f;
            g_fail_ro = ro;
            g_opens_rw = g_flash_writes = 0;
            g_log.clear();
            sys_state_init();
            g_fail = none;
            g_fail_ro = ESP_OK;
        };
        auto at = [&](int level, int alarm) {   // a hub booted at this level and alarm switch (-1 = key missing)
            fresh(nullptr);
            if (level >= 0) g_flash[kLv] = static_cast<uint8_t>(level);
            if (alarm >= 0) g_flash[kAl] = static_cast<uint8_t>(alarm);
            boot(none, ESP_OK);
            g_opens_rw = g_flash_writes = 0;
            g_pushes.clear();
            g_log.clear();
        };
        printf("\nremote_level, remote_alarm\n");

        // The pure parts.
        ck("default: full with a cloud token, else view", sys_remote_level_default(true) == 2 && sys_remote_level_default(false) == 0);
        ck("names; above 2 counts as view",
           !strcmp(sys_remote_level_name(0), "view") && !strcmp(sys_remote_level_name(1), "devices") &&
           !strcmp(sys_remote_level_name(2), "full") && !strcmp(sys_remote_level_name(3), "view") &&
           !strcmp(sys_remote_level_name(255), "view"));
        bool parse_ok = sys_remote_level_parse("view") == 0 && sys_remote_level_parse("devices") == 1 &&
                        sys_remote_level_parse("full") == 2 && sys_remote_level_parse(nullptr) == -1;
        for (const char* bad : {"Full", "FULL", "", " full", "full ", "ful", "fully", "2", "admin"})
            parse_ok = parse_ok && sys_remote_level_parse(bad) == -1;
        ck("parse: the three names exactly, else -1", parse_ok);
        ck("save rule: a raise once saved, a lowering at once",
           sys_remote_level_after_save(0, 2, true) == 2 && sys_remote_level_after_save(0, 2, false) == 0 &&
           sys_remote_level_after_save(0, 1, false) == 0 && sys_remote_level_after_save(1, 2, false) == 1 &&
           sys_remote_level_after_save(2, 0, false) == 0 && sys_remote_level_after_save(2, 1, false) == 1 &&
           sys_remote_level_after_save(1, 0, true) == 0 && sys_remote_level_after_save(2, 2, false) == 2);
        ck("the relay keeps both keys local", remote_is_local_only_key("remote_level") && remote_is_local_only_key("remote_alarm"));

        // The first boot of this firmware: full once for a hub that is linked, view for every other one.
        fresh(tok);
        boot(none, ESP_OK);
        ck("upgrade with a cloud token: full, written once", sys_remote_level() == 2 && nv(kLv) == 2 && g_flash_writes == 1);
        boot(none, ESP_OK);
        ck("...next boot: full, nothing written", sys_remote_level() == 2 && g_opens_rw == 0);
        bool r = apply("{\"remote_level\":\"view\"}");
        boot(none, ESP_OK);
        ck("...owner sets view, reboot: view (no 2nd run)", r && sys_remote_level() == 0 && nv(kLv) == 0 && g_opens_rw == 0);
        fresh(nullptr);
        boot(none, ESP_OK);
        ck("fresh hub: view, written once", sys_remote_level() == 0 && nv(kLv) == 0 && g_flash_writes == 1);
        g_flash_str[kTok] = tok;
        boot(none, ESP_OK);
        ck("...linked later, reboot: view", sys_remote_level() == 0 && g_opens_rw == 0);
        fresh("");
        boot(none, ESP_OK);
        ck("an empty token is none: view", sys_remote_level() == 0 && nv(kLv) == 0);
        fresh(tok);
        boot(none, ESP_OK);
        fresh(nullptr);   // system.storage_reset erases the partition, the token with it
        boot(none, ESP_OK);
        ck("storage reset: view", sys_remote_level() == 0 && nv(kLv) == 0);
        for (const Fail& f : {f_open, f_set, f_commit}) {
            fresh(tok);
            boot(f, ESP_OK);
            const bool first = sys_remote_level() == 0 && nv(kLv) == -1 && logged("remote_level=2 not saved");
            boot(none, ESP_OK);
            ck("upgrade, write FAILS: view; next boot full", first && sys_remote_level() == 2 && nv(kLv) == 2);
        }
        fresh(tok);
        boot(f_open, ESP_ERR_NVS_NOT_INITIALIZED);
        ck("NVS not usable at boot: view, no write", sys_remote_level() == 0 && g_opens_rw == 0 && !sys_remote_alarm());
        fresh(tok);
        g_flash[kLv] = 7;
        boot(none, ESP_OK);
        ck("a stored 7 is view, left for the next save", sys_remote_level() == 0 && nv(kLv) == 7 && g_opens_rw == 0);

        // The page sets the level.
        at(0, -1);
        r = apply("{\"remote_level\":\"full\"}");
        ck("view -> full, saved: full, hub.caps", r && sys_remote_level() == 2 && nv(kLv) == 2 && g_pushes.size() == 1);
        r = apply("{\"remote_level\":\"devices\"}");
        ck("full -> devices, saved: devices, hub.caps", r && sys_remote_level() == 1 && nv(kLv) == 1 && g_pushes.size() == 2);
        for (const Fail& f : {f_open, f_set, f_commit}) {
            at(0, -1);
            g_fail = f;
            r = apply("{\"remote_level\":\"full\"}");
            g_fail = none;
            ck("view -> full, save FAILS: refused, stays view",
               !r && sys_remote_level() == 0 && nv(kLv) == 0 && g_pushes.empty() && logged("it stays view"));
            at(2, -1);
            g_fail = f;
            r = apply("{\"remote_level\":\"view\"}");
            g_fail = none;
            const bool now = !r && sys_remote_level() == 0 && nv(kLv) == 2 && g_pushes.size() == 1 &&
                             logged("may be full again after a reboot");
            boot(none, ESP_OK);
            ck("full -> view, save FAILS: refused, view now", now && sys_remote_level() == 2);   // why the page is told
        }
        at(2, -1);
        g_fail = f_set;
        apply("{\"remote_level\":\"view\"}");
        g_fail = none;
        g_pushes.clear();
        r = apply("{\"remote_level\":\"view\"}");
        const bool repaired = r && sys_remote_level() == 0 && nv(kLv) == 0 && g_pushes.empty();
        boot(none, ESP_OK);
        ck("...then view again, NVS healthy: repaired", repaired && sys_remote_level() == 0);
        at(1, -1);
        r = apply("{\"remote_level\":\"devices\"}");
        ck("same level again: saved (repair), no hub.caps",
           r && sys_remote_level() == 1 && g_opens_rw == 1 && g_flash_writes == 0 && g_pushes.empty());
        for (const char* bad : {"{\"remote_level\":\"Full\"}", "{\"remote_level\":\"\"}", "{\"remote_level\":2}",
                                "{\"remote_level\":true}", "{\"remote_level\":\"admin\"}", "{\"remote_level\":{}}"}) {
            at(1, -1);
            r = apply(bad);
            ck(bad, !r && sys_remote_level() == 1 && nv(kLv) == 1 && g_opens_rw == 0 && g_pushes.empty());
        }
        at(1, -1);
        r = apply("{\"remote_level\":null}");
        ck("level null counts as absent", r && sys_remote_level() == 1 && g_opens_rw == 0 && g_pushes.empty());

        // The relay reads the level for every command: a lowering refuses the very next one.
        at(1, -1);
        const bool before = remote_cmd_allowed("device.attr.set") && !remote_cmd_allowed("rule.create");
        apply("{\"remote_level\":\"view\"}");
        ck("devices -> view: the next device write refused",
           before && !remote_cmd_allowed("device.attr.set") && remote_cmd_allowed("status.get"));
        g_fail = f_set;
        apply("{\"remote_level\":\"full\"}");
        g_fail = none;
        ck("a raise not saved: the relay still refuses", !remote_cmd_allowed("device.attr.set") && !remote_cmd_allowed("rule.create"));
        apply("{\"remote_level\":\"full\"}");
        ck("the raise saved: the relay passes rules", remote_cmd_allowed("rule.create") && remote_cmd_allowed("device.attr.set"));

        // The alarm switch: as remote_scripts.
        at(-1, -1);
        ck("alarm switch: missing = off", !sys_remote_alarm() && !remote_alarm_allowed());
        r = apply("{\"remote_alarm\":true}");
        ck("alarm on, saved: on, hub.caps, the relay sees it",
           r && sys_remote_alarm() && nv(kAl) == 1 && g_pushes.size() == 1 && remote_alarm_allowed());
        for (const Fail& f : {f_open, f_set, f_commit}) {
            at(-1, 0);
            g_fail = f;
            r = apply("{\"remote_alarm\":true}");
            g_fail = none;
            ck("alarm on, save FAILS: refused, stays off",
               !r && !sys_remote_alarm() && nv(kAl) == 0 && g_pushes.empty() && logged("it stays off"));
            at(-1, 1);
            g_fail = f;
            r = apply("{\"remote_alarm\":false}");
            g_fail = none;
            const bool now = !r && !sys_remote_alarm() && !remote_alarm_allowed() && nv(kAl) == 1 && g_pushes.size() == 1 &&
                             logged("may be on again after a reboot");
            boot(none, ESP_OK);
            ck("alarm off, save FAILS: refused, off now", now && sys_remote_alarm());
        }
        at(-1, 1);
        r = apply("{\"remote_alarm\":\"true\"}");
        ck("alarm \"true\" (string) while on: off", r && !sys_remote_alarm() && nv(kAl) == 0);
        at(-1, 1);
        r = apply("{\"remote_alarm\":null}");
        ck("alarm null while on: untouched", r && sys_remote_alarm() && g_opens_rw == 0 && g_pushes.empty());
        at(-1, 1);
        r = apply("{\"remote_alarm\":true}");
        ck("alarm on while on: no NVS call, no push", r && sys_remote_alarm() && g_opens_rw == 0 && g_pushes.empty());
    }

    printf("\n%s (%d mismatch%s)\n", g_mismatches ? "FAILED" : "ALL OK", g_mismatches, g_mismatches == 1 ? "" : "es");
    return g_mismatches;
}
