// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// What "Allow script changes from the cloud" does when NVS cannot be written (spec 2026-10-05 §3.1).
// Runs the REAL main/sys_state.cpp and the REAL system_apply_settings (main/api_system.cpp) against a fake NVS
// that can fail at open, set and commit. A cold boot is sys_state_init() again over the same fake flash.
// Exit code = number of mismatches. See run.sh.
#include "stubs.h"
#include "sys_state.h"
#include "log_cloud.h"

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
static std::map<std::string, uint8_t> g_flash;   // durable "namespace/key" -> u8
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
esp_err_t nvs_get_str(nvs_handle_t, const char*, char*, size_t*) { return ESP_ERR_NVS_NOT_FOUND; }
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

// A hub that has just booted over this NVS content (-1 = key missing).
static void world(int nvs) {
    g_flash.clear();
    if (nvs >= 0) g_flash[kKey] = static_cast<uint8_t>(nvs);
    g_fail = Fail{ESP_OK, ESP_OK, ESP_OK};
    g_opens_rw = g_flash_writes = 0;
    sys_state_init();
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

    printf("\n%s (%d mismatch%s)\n", g_mismatches ? "FAILED" : "ALL OK", g_mismatches, g_mismatches == 1 ? "" : "es");
    return g_mismatches;
}
