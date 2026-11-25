// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Host check of the alarm panel's glue (Safety profiles, spec 2026-11-24). It includes main/alarm.cpp itself (run.sh
// copies it beside this file), fakes everything around it and runs the real task loop, in turns with the test, on a fake clock.
// Every fake of the shadow, the pool, device_cmd, NVS, the event bus, the cloud, Telegram and the page aborts when
// it is called under the alarm mutex, and the mutex aborts when it is taken twice. Modes (alarm.cpp's statics live
// as long as the process, so one boot is one run): main, a day of use; boot and legacy, a restart during an alarm
// from the new records and from the stage 1 records; fallback, a boot after the stage 1 firmware armed the hub;
// restart, chains of boots on one flash (boot_run: each boot in a child process), for power cuts and restarts.
#include "alarm.cpp"

#include <sys/wait.h>
#include <unistd.h>

#include <condition_variable>
#include <cstdarg>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>


int      g_lock_depth = 0;
int      g_notified = 0;
int      g_sleeps_left = 0;
uint64_t g_now_us = 1000000;
bool     g_log = false;
bool     g_nvs_fail = false;
std::string g_nvs_fail_key;
void (*g_nvs_hook)(const char*) = nullptr;
void (*g_nvs_before)(const char*) = nullptr;
std::map<std::string, int> g_nvs_refused;
std::vector<std::string> g_logs;   // every log line, without its level and tag
void host_log(const char* level, const char* tag, const char* fmt, ...) {
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    g_logs.push_back(line);
    if (g_log) std::fprintf(stderr, "    %s %s: %s\n", level, tag, line);
}
std::vector<std::string> g_trace;
std::map<std::string, std::string> g_nvs;
std::vector<std::string> g_all_cmds;   // never cleared

namespace h {

struct Dev { const char* name; const char* exposes; };
std::map<uint64_t, Dev> devs = {
    {0x1, {"Front door", R"([{"name":"contact","type":"binary","access":1}])"}},
    {0x2, {"Hall motion", R"([{"name":"occupancy","type":"binary","access":1}])"}},
    {0x3, {"Kitchen leak", R"([{"name":"water_leak","type":"binary","access":1}])"}},
    {0x4, {"Smoke", R"([{"name":"smoke","type":"binary","access":1}])"}},
    {0x10, {"Siren", R"([{"name":"warning","type":"composite","access":2}])"}},
    {0x11, {"Valve", R"([{"name":"state","type":"binary","access":7}])"}},
    {0x12, {"Plug", R"([{"name":"state","type":"binary","access":7}])"}},
    {0x20, {"Keypad", R"([{"name":"action","type":"enum","access":1}])"}},
};
std::map<std::pair<uint64_t, std::string>, int> vals;     // the shadow's last sensor values
std::map<std::string, std::string> alarm_attrs;           // the Alarm device in the shadow
device_cmd_local_fn   local_cmd_fn = nullptr;
zhac_local_exposes_fn local_exposes_fn = nullptr;
EventHandler on_event;
EventFilter  filter;
ZapDevice    alarm_dev{};
bool         have_alarm_dev = false;
int          fails = 0, checks = 0;
std::function<void()> on_cmd;   // run once inside the next output command: what happens while it blocks
std::vector<uint64_t> persisted;   // device_shadow_persist_soon calls, in order
int seed_reads = 0, reads_before_sub = -1;   // device_shadow_get_attr calls; how many came before the subscription

void trace(const std::string& s) { g_trace.push_back(s); }
int idx(const char* sub, size_t from = 0) {
    for (size_t i = from; i < g_trace.size(); i++) if (g_trace[i].find(sub) != std::string::npos) return static_cast<int>(i);
    return -1;
}
int pos(const char* exact) {   // the first entry that is exactly this
    for (size_t i = 0; i < g_trace.size(); i++) if (g_trace[i] == exact) return static_cast<int>(i);
    return -1;
}
int cnt(const char* sub) {
    int n = 0;
    for (const auto& s : g_trace) n += s.find(sub) != std::string::npos;
    return n;
}
int logged(const char* sub) {   // the log lines that say this
    int n = 0;
    for (const auto& s : g_logs) n += s.find(sub) != std::string::npos;
    return n;
}
void dump() { for (const auto& s : g_trace) std::printf("      %s\n", s.c_str()); }
#define CHECK(c, what) do { h::checks++; if (!(c)) { std::printf("FAIL: %s (line %d)\n", what, __LINE__); h::fails++; h::dump(); } } while (0)

// The real task loop on its own thread, but never beside the test: one of the two runs, the other waits for its
// turn. The task keeps its locals between the test's steps, as on the hub.
std::mutex&              turn_mu = *new std::mutex;   // never destroyed: the task still waits on them at exit
std::condition_variable& turn_cv = *new std::condition_variable;
bool task_turn = false, task_started = false;
void take_turn(bool task) {   // hand over, then wait to be handed back
    std::unique_lock<std::mutex> l(turn_mu);
    task_turn = !task;
    turn_cv.notify_all();
    turn_cv.wait(l, [task] { return task_turn == task; });
}
// Let the task run until it has nothing to do; it may sleep `sleeps` times first (1 s each while an alarm runs).
void run_task(int sleeps = 0) {
    g_sleeps_left = sleeps;
    if (!task_started) {
        task_started = true;
        std::thread([] {
            {
                std::unique_lock<std::mutex> l(turn_mu);
                turn_cv.wait(l, [] { return task_turn; });
            }
            alarm_task(nullptr);
        }).detach();
    }
    take_turn(false);
}
void emit(uint64_t ieee, const char* key, int v, const char* str = nullptr, bool more = false) {
    if (!str) vals[{ieee, key}] = v;
    Event e{};
    e.type = EventType::ZCL_ATTR;
    auto* z = reinterpret_cast<ZclAttrEvent*>(e.data);
    z->ieee = ieee;
    std::snprintf(z->key, sizeof(z->key), "%s", key);
    z->val_type = str ? VAL_STR : VAL_BOOL;
    if (str) std::snprintf(z->str_val, sizeof(z->str_val), "%s", str);
    else z->int_val = v;
    z->more = more;
    if (filter(e)) on_event(e);
}
std::string status() {
    JsonDocument d;
    alarm_status_json(d.to<JsonObject>());
    std::string s;
    serializeJson(d, s);
    return s;
}
DevCmdResult cmd(const char* key, const char* value) {
    const DevCmdValue v = device_cmd_str(value);
    return local_cmd_fn(kAlarmIeee, key, &v);
}
bool save(const char* json, std::string* why = nullptr) {
    char err[160] = "";
    const bool ok = alarm_config_set(json, std::strlen(json), err, sizeof(err));
    if (why) *why = err;
    return ok;
}
void boot() {
    seed_reads = 0;
    reads_before_sub = -1;
    alarm_early_init();
    alarm_attach_device();
    alarm_start();
}

// One boot of a chain on one flash: `f` runs in a child process, from alarm.cpp's statics as they are at program start
// (this process never boots). When `f` returns the power goes: the flash the child leaves is the next boot's, and its
// checks count here.
void boot_run(const std::function<void()>& f) {
    int fd[2];
    if (pipe(fd) != 0) std::abort();
    std::fflush(stdout);
    const pid_t pid = fork();
    if (pid < 0) std::abort();
    if (pid == 0) {
        close(fd[0]);
        f();
        if (g_lock_depth) { std::printf("FAIL: the mutex is still held\n"); fails++; }
        std::string out = std::to_string(checks) + " " + std::to_string(fails) + " " + std::to_string(g_nvs.size()) + "\n";
        for (const auto& kv : g_nvs) out += std::to_string(kv.first.size()) + " " + std::to_string(kv.second.size()) + "\n" + kv.first + kv.second;
        std::fflush(stdout);
        for (size_t at = 0; at < out.size();) {
            const ssize_t w = write(fd[1], out.data() + at, out.size() - at);
            if (w <= 0) _exit(2);
            at += static_cast<size_t>(w);
        }
        _exit(0);
    }
    close(fd[1]);
    std::string in;
    char buf[4096];
    for (ssize_t r; (r = read(fd[0], buf, sizeof(buf))) > 0;) in.append(buf, static_cast<size_t>(r));
    close(fd[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 || in.empty()) {
        std::printf("FAIL: a boot of the chain died\n");
        fails++;
        return;
    }
    size_t at = 0;
    auto num = [&] {   // digits, then one separator
        int v = 0;
        while (at < in.size() && in[at] >= '0' && in[at] <= '9') v = v * 10 + (in[at++] - '0');
        at++;
        return v;
    };
    checks = num();
    fails = num();
    g_nvs.clear();
    for (int i = 0, n = num(); i < n; i++) {
        const size_t kl = static_cast<size_t>(num()), vl = static_cast<size_t>(num());
        g_nvs[in.substr(at, kl)] = in.substr(at + kl, vl);
        at += kl + vl;
    }
}

}  // namespace h

// ── The fakes ──
void zigbee_pool_lock() { OUTSIDE_LOCK("zigbee_pool_lock"); }
void zigbee_pool_unlock() {}
ZapDevice* pool_find_by_ieee(uint64_t ieee) { return ieee == kAlarmIeee && h::have_alarm_dev ? &h::alarm_dev : nullptr; }
ZapDevice* pool_add() { h::have_alarm_dev = true; return &h::alarm_dev; }
bool zigbee_pool_snapshot(uint64_t ieee, ZapDevice* out) {
    OUTSIDE_LOCK("zigbee_pool_snapshot");
    const auto it = h::devs.find(ieee);
    if (it == h::devs.end()) return false;
    *out = ZapDevice{};
    out->ieee_addr = ieee;
    std::snprintf(out->friendly_name, sizeof(out->friendly_name), "%s", it->second.name);
    return true;
}
bool zap_store_save_device(const ZapDevice*) { OUTSIDE_LOCK("zap_store_save_device"); return true; }
bool device_shadow_get_attr(uint64_t ieee, const char* key, ShadowAttr* out) {
    OUTSIDE_LOCK("device_shadow_get_attr");
    h::seed_reads++;
    const auto it = h::vals.find({ieee, key});
    if (it == h::vals.end()) return false;
    out->val_type = VAL_BOOL;
    out->int_val = it->second;
    return true;
}
uint8_t device_shadow_get_attrs(uint64_t ieee, ShadowAttr* out, uint8_t max) {
    OUTSIDE_LOCK("device_shadow_get_attrs");
    uint8_t n = 0;
    if (ieee != kAlarmIeee) return 0;
    for (const auto& kv : h::alarm_attrs) {
        if (n == max) break;
        out[n] = ShadowAttr{};
        std::snprintf(out[n++].key, ATTR_KEY_MAX, "%s", kv.first.c_str());
    }
    return n;
}
void device_shadow_process_ieee(uint64_t ieee, const ZclAttribute* a, uint8_t n) {
    OUTSIDE_LOCK("device_shadow_process_ieee");
    if (ieee != kAlarmIeee) std::abort();
    for (uint8_t i = 0; i < n; i++) {
        h::alarm_attrs[a[i].key] = a[i].str_val;
        h::trace(std::string("attr:") + a[i].key + "=" + a[i].str_val);
    }
    h::alarm_attrs["_last_seen"] = "1";   // the shadow's own
}
void device_shadow_persist_soon(uint64_t ieee) { OUTSIDE_LOCK("device_shadow_persist_soon"); h::persisted.push_back(ieee); }
bool device_shadow_remove_attr(uint64_t, const char* key) {
    OUTSIDE_LOCK("device_shadow_remove_attr");
    h::trace(std::string("rm:") + key);
    return h::alarm_attrs.erase(key) > 0;
}
DevCmdResult device_cmd_set_attr(uint64_t ieee, uint8_t, const char* key, const DevCmdValue* v) {
    OUTSIDE_LOCK("device_cmd_set_attr");
    char b[96];
    if (v->kind == DEVCMD_STR) std::snprintf(b, sizeof(b), "cmd:%" PRIx64 ":%s=%s", ieee, key, v->s);
    else std::snprintf(b, sizeof(b), "cmd:%" PRIx64 ":%s=%d", ieee, key, v->kind == DEVCMD_BOOL ? v->b : static_cast<int>(v->i));
    h::trace(b);
    g_all_cmds.push_back(b);
    if (h::on_cmd) { auto f = h::on_cmd; h::on_cmd = nullptr; f(); }
    return DEVCMD_OK;
}
const char* device_cmd_result_str(DevCmdResult) { return "x"; }
extern "C" size_t zhac_adapter_build_exposes_json(uint64_t ieee, const char*, const char*, char* buf, size_t cap) {
    OUTSIDE_LOCK("zhac_adapter_build_exposes_json");
    if (ieee == kAlarmIeee) return h::local_exposes_fn(ieee, buf, cap);
    const auto it = h::devs.find(ieee);
    if (it == h::devs.end()) return 0;
    const int n = std::snprintf(buf, cap, "%s", it->second.exposes);
    return static_cast<size_t>(n) < cap ? static_cast<size_t>(n) : 0;
}
void local_dev_register(uint64_t, device_cmd_local_fn c, zhac_local_exposes_fn e) { h::local_cmd_fn = c; h::local_exposes_fn = e; }
void event_bus_publish(const Event& e) {
    OUTSIDE_LOCK("event_bus_publish");
    if (e.type == EventType::RULE_EVENT) h::trace(std::string("event:") + reinterpret_cast<const RuleEventPayload*>(e.data)->name);
    else if (e.type == EventType::DEVICE_JOIN) h::trace("join");
    else std::abort();
}
EventSubHandle event_bus_subscribe(EventType, EventHandler cb, EventFilter f, uint8_t) {
    h::reads_before_sub = h::seed_reads;
    h::on_event = cb;
    h::filter = f;
    return 1;
}
extern "C" void remote_client_publish_event(const char* name, const char* json, size_t len) {
    OUTSIDE_LOCK("remote_client_publish_event");
    h::trace(std::string(name) + ":" + std::string(json, len));
}
extern "C" bool tg_gw_send(const char* text, const char*, const char*) { OUTSIDE_LOCK("tg_gw_send"); h::trace(std::string("tg:") + text); return true; }
void ws_push(const char* event, JsonDocument& d) {
    OUTSIDE_LOCK("ws_push");
    std::string s;
    serializeJson(d, s);
    h::trace(std::string("push:") + event + ":" + s);
}

void host_yield() { h::take_turn(true); }

using h::cnt; using h::idx; using h::pos; using h::run_task; using h::emit;

const char* kCfg = R"({"profiles":[
 {"id":"away","name":"Away","class":"intrusion","role":"away","arm_delay":0,"trigger_delay":5,"siren_time":10,"notify_arm":true,
  "sensors":[{"ieee":"0x1","entry":true},{"ieee":"0x2","entry":false}],
  "outputs":[{"ieee":"0x10","kind":"warning","mode":"burglar","level":"high","strobe":true}]},
 {"id":"night","name":"Night","class":"intrusion","role":"night","arm_delay":30,"trigger_delay":0,"siren_time":10,"notify_arm":false,
  "sensors":[{"ieee":"0x1","entry":false}],"outputs":[]},
 {"id":"flood","name":"Flood","class":"water","arm_delay":0,"trigger_delay":0,"siren_time":20,"notify_arm":true,
  "sensors":[{"ieee":"0x3","entry":false}],
  "outputs":[{"ieee":"0x11","kind":"state","value":"off","keep":true},{"ieee":"0x10","kind":"warning","mode":"emergency","level":"high","strobe":true}]}],
 "keypads":[{"ieee":"0x20","trusted":false}],"codes":["1234"]})";

int main_flow() {
    // ── Boot with no records: the three default profiles, attributes published, nothing else ──
    h::boot();
    run_task();
    CHECK(idx("attr:alarm_sensor=") == 0 && idx("attr:away=disarmed") == 1 && idx("attr:home=disarmed") == 2 &&
          idx("attr:night=disarmed") == 3 && idx("attr:alarm_state=disarmed") == 4 && g_trace.size() == 7 &&
          g_trace[5] == "nvs:st" && g_trace[6] == "nvs:st2",
          "boot: alarm_sensor, the profiles, alarm_state; a hub with no records writes its states once (F4); no event, no push");
    g_trace.clear();
    run_task(1);
    CHECK(g_trace.empty(), "a pass with nothing new writes nothing when flash holds the states");

    // ── A save: cfg2 only, the new hazard profile armed, the attribute list follows ──
    g_trace.clear();
    std::string why;
    CHECK(h::save(kCfg, &why), "the config saves");
    CHECK(cnt("nvs:cfg2") == 1 && cnt("nvs:cfg") == 1, "the save writes cfg2 and never cfg");
    CHECK(cnt("nvs:st2") == 1 && idx("nvs:st2") > idx("nvs:cfg2") && g_nvs["st2"].find("[\"flood\",2,1]") != std::string::npos,
          "and then the states, the new flood profile armed: a power cut before the task's pass keeps them (M28)");
    CHECK(h::persisted == std::vector<uint64_t>({0x1, 0x2, 0x3}),
          "each sensor the save adds is seeded, and its last values go to flash at the shadow's next sweep (M44)");
    run_task();
    CHECK(h::alarm_attrs.count("home") == 0 && idx("rm:home") >= 0 && idx("rm:_last_seen") < 0, "home's attribute left the device; the shadow's own stayed");
    CHECK(h::alarm_attrs["flood"] == "armed" && h::alarm_attrs["away"] == "disarmed" && h::alarm_attrs["night"] == "disarmed", "every profile's attribute published");
    CHECK(cnt("join") == 1 && idx("rm:home") < idx("attr:flood=armed") && idx("attr:flood=armed") < idx("join"), "removed, published, then announced");
    CHECK(cnt("alert:") == 1 && idx("\"kind\":\"alarm_armed\"") >= 0 && idx("Armed (Flood) at ") >= 0 && idx("\"name\":\"flood\"") >= 0, "the new flood profile is armed and says so, keyed by its id");
    CHECK(idx("event:alarm_armed_flood") >= 0 && cnt("event:") == 1, "one event for it; the summary did not move");
    CHECK(cnt("attr:alarm_state") == 0, "alarm_state is not published again when the summary did not move");
    CHECK(cnt("nvs:st2") == 2 && pos("nvs:st") < 0 && g_nvs["st"] == std::string("\0\0", 2),
          "the task writes st2 once more after the save's own write; st is still the one the boot wrote");
    CHECK(g_nvs["st2"].rfind("{\"v\":1,\"l\":[0,0],\"p\":[", 0) == 0, "st2 carries the st that flash holds (ruling 17)");
    CHECK(idx("nvs:st2") < idx("alert:") && idx("alert:") < idx("attr:flood") && idx("attr:flood") < idx("event:") && idx("event:") < idx("push:alarm.changed"),
          "pass order: states, notifications, attributes, events, (outputs), page");

    // ── A door or a hazard of a configured sensor that changes goes to flash at the shadow's next sweep: a power cut
    //    must not bring back a value from up to 30 minutes ago (F1) ──
    h::persisted.clear();
    emit(0x1, "contact", 0);     // the front door opens,
    emit(0x1, "contact", 0);     // is reported open again,
    emit(0x1, "contact", 1);     // and closes
    emit(0x3, "water_leak", 1);  // a leak (flood's alarm, silenced below),
    emit(0x3, "water_leak", 0);  // and dry again
    emit(0x2, "occupancy", 1);   // motion: an event, nothing kept
    emit(0x1, "battery", 90);    // not a key the panel watches
    emit(0x4, "smoke", 1);       // a device no profile has
    CHECK(h::persisted == std::vector<uint64_t>({0x1, 0x1, 0x3, 0x3}),
          "each change of a door or a hazard asks for a prompt write; a repeat, motion, another key and another device do not");
    CHECK(alarm_silence("flood") == AlarmReply::OK, "flood silenced");
    run_task();

    // ── Arm away from the page ──
    g_trace.clear();
    uint64_t open[kAlarmMaxSensors];
    uint8_t n_open = 9;
    CHECK(alarm_arm("away", false, open, &n_open) == AlarmReply::OK && n_open == 0, "away arms");
    CHECK(alarm_arm("nope", false, open, &n_open) == AlarmReply::NO_PROFILE, "an unknown id is no profile");
    run_task();
    {
        const int a = idx("attr:alarm_sensor="), b = idx("attr:away=armed"), c = idx("attr:alarm_state=armed_away");
        CHECK(a >= 0 && b == a + 1 && c == b + 1, "alarm_sensor first, the profile, alarm_state last");
    }
    CHECK(cnt("event:alarm_armed_away") == 1 && cnt("event:") == 1, "alarm_armed_away fires once");
    CHECK(idx("Armed (Away) at ") >= 0 && idx("tg:Alarm: Armed (Away) at ") >= 0, "armed notice, Telegram with Alarm: in front");
    CHECK(g_nvs["st"] == std::string("\2\2", 2) && pos("nvs:st") >= 0 && pos("nvs:st") < pos("nvs:st2"),
          "the stage 1 record says armed_away; st first, then st2 (ruling 17)");

    // ── The entry door: pending, then triggered when the delay ends ──
    g_trace.clear();
    emit(0x1, "contact", 0);
    run_task();
    CHECK(idx("attr:alarm_sensor=Front door") >= 0 && idx("attr:away=pending") >= 0 && idx("attr:alarm_state=pending") >= 0, "pending with its sensor");
    CHECK(idx("event:alarm_pending_away") >= 0 && idx("event:alarm_pending") >= 0 && cnt("\"kind\":\"alarm\"") == 0 && cnt("cmd:") == 0, "pending: events, no alert, no output");
    CHECK(h::status().find("\"left\":5") != std::string::npos && h::status().find("\"sensor\":\"Front door\"") != std::string::npos, "the status counts the delay down and names the door");
    g_trace.clear();
    run_task(5);
    CHECK(idx("\"kind\":\"alarm\"") >= 0 && idx("\"message\":\"Front door (Away) at ") >= 0 && idx("\"ieee\":\"0x0000000000000001\"") >= 0 && g_trace[idx("\"kind\":\"alarm\"")].find("\"name\"") == std::string::npos, "the alarm alert, keyed by its sensor");
    CHECK(idx("tg:Alarm: Front door (Away) at ") >= 0, "the alarm on Telegram");
    CHECK(cnt("cmd:") == 1 && idx("cmd:10:warning=burglar,high,1,10") >= 0, "the siren starts for the whole alarm time");
    CHECK(idx("event:alarm_triggered_away") >= 0 && idx("event:alarm_triggered") >= 0, "both trigger events");
    CHECK(idx("nvs:st2") < idx("alert:") && idx("alert:") < idx("attr:away=triggered") && idx("attr:alarm_state=triggered") < idx("event:alarm_triggered_away") &&
          idx("event:alarm_triggered") < idx("cmd:10") && idx("cmd:10") < idx("push:alarm.changed"), "pass order with an output");

    // ── A leak while the intrusion alarm sounds: the valve closes, the siren changes to the higher class ──
    g_trace.clear();
    emit(0x3, "water_leak", 1);
    run_task();
    CHECK(idx("cmd:11:state=0") >= 0 && idx("cmd:10:warning=emergency,high,1,20") > idx("cmd:11:state=0"), "valve first, then the siren takes the flood's tone");
    CHECK(idx("Kitchen leak (Flood) at ") >= 0 && cnt("attr:alarm_state") == 0 && cnt("attr:alarm_sensor") == 0, "the flood alert; alarm_state and alarm_sensor untouched");
    g_trace.clear();
    run_task(11);   // away's 10 s end, the flood's 20 s do not
    CHECK(h::alarm_attrs["away"] == "armed" && h::alarm_attrs["flood"] == "triggered" && cnt("cmd:") == 0, "the intrusion alarm ends and silences nothing");
    CHECK(idx("attr:alarm_sensor=") >= 0 && idx("attr:alarm_state=armed_away") >= 0 && idx("event:alarm_armed_away") >= 0, "the summary is back to armed_away");
    g_trace.clear();
    run_task(12);
    CHECK(h::alarm_attrs["flood"] == "armed" && cnt("cmd:") == 1 && idx("cmd:10:warning=0") >= 0, "the flood alarm ends: siren off, the kept valve left closed");
    CHECK(cnt("Disarmed") == 0 && cnt("Armed (") == 0, "an alarm that ends tells nobody armed or disarmed");

    // ── Keypad: a wrong code, then the right one; the flood profile stays armed ──
    g_trace.clear();
    emit(0x20, "action", 0, "disarm", true);
    emit(0x20, "action_code", 0, "0000");
    run_task();
    CHECK(h::alarm_attrs["away"] == "armed" && g_trace.empty(), "a wrong code disarms nothing");
    emit(0x20, "action", 0, "disarm", true);
    emit(0x20, "action_code", 0, "1234");
    run_task();
    CHECK(h::alarm_attrs["away"] == "disarmed" && h::alarm_attrs["flood"] == "armed", "the code disarms intrusion only");
    CHECK(idx("Disarmed (Away) at ") >= 0 && idx("\"kind\":\"alarm_disarmed\"") >= 0 && idx("\"name\":\"away\"") >= 0, "disarmed notice with the profile id");
    CHECK(idx("event:alarm_disarmed_away") >= 0 && idx("event:alarm_disarmed") >= 0 && g_nvs["st"] == std::string("\0\0", 2), "disarm events; st follows");

    // ── The Alarm device: a refused arm is told; the page's is not; force bypasses ──
    g_trace.clear();
    CHECK(h::cmd("alarm_state", "armed_away") == DEVCMD_BAD_VALUE, "an arm with the door open is refused");
    run_task();
    CHECK(idx("\"kind\":\"alarm_arm_failed\"") >= 0 && idx("Not armed (Away): Front door is open at ") >= 0 && idx("\"name\":\"away\"") >= 0 &&
          idx("\"ieee\":\"0x0000000000000001\"") >= 0, "the refusal is notified");
    CHECK(g_trace.size() == 4 && g_trace[2] == "event:alarm_arm_failed_away" && g_trace[3] == "event:alarm_arm_failed", "both refusal events");
    g_trace.clear();
    CHECK(alarm_arm("away", false, open, &n_open) == AlarmReply::OPEN && n_open == 1 && open[0] == 0x1, "the page gets the open sensors");
    run_task();
    CHECK(g_trace.empty(), "the page's refusal is not notified");
    CHECK(alarm_arm("away", true, open, &n_open) == AlarmReply::OK, "forced");
    run_task();
    CHECK(h::status().find("\"bypassed\":[\"0x0000000000000001\"]") != std::string::npos, "the forced door is bypassed");
    CHECK(h::cmd("away", "disarmed") == DEVCMD_OK && h::cmd("flood", "armed") == DEVCMD_OK, "a profile's own attribute takes disarmed and armed");
    CHECK(h::cmd("alarm_state", "armed_flood") == DEVCMD_BAD_VALUE && h::cmd("alarm_state", "pending") == DEVCMD_BAD_VALUE &&
          h::cmd("away", "triggered") == DEVCMD_BAD_VALUE && h::cmd("alarm_sensor", "x") == DEVCMD_NO_CONVERTER && h::cmd("nokey", "armed") == DEVCMD_NO_CONVERTER,
          "everything else is a bad value or no converter");
    CHECK(alarm_silence("nope") == AlarmReply::NO_PROFILE && alarm_disarm("nope") == AlarmReply::NO_PROFILE && alarm_silence("flood") == AlarmReply::OK, "silence and disarm by id");
    run_task();
    {
        char buf[2048];
        const size_t n = h::local_exposes_fn(kAlarmIeee, buf, sizeof(buf));
        const std::string e(buf, n);
        CHECK(e.rfind(R"([{"name":"away","type":"enum","access":7,"values":["disarmed","arming","armed","pending","triggered"]},)", 0) == 0 &&
              e.find(R"({"name":"alarm_state","type":"enum","access":7,"values":["disarmed","arming","armed_away","armed_night","pending","triggered"]},{"name":"alarm_sensor","type":"text","access":1}])") != std::string::npos,
              "the exposes follow the profiles");
        char small[64];
        CHECK(h::local_exposes_fn(kAlarmIeee, small, sizeof(small)) == 0, "exposes that do not fit give nothing");
    }

    // ── No save while an alarm runs; panic; keypad arm by role ──
    emit(0x1, "contact", 1);
    emit(0x20, "action", 0, "arm_night_zones");
    run_task();
    CHECK(h::alarm_attrs["night"] == "arming", "the keypad's night button arms the night profile");
    CHECK(!h::save(kCfg, &why) && why == "an alarm is running: disarm or wait, then save", "a save during the arm delay is refused");
    emit(0x20, "action", 0, "arm_day_zones");   // no profile has the role home: ignored
    g_trace.clear();
    emit(0x20, "action", 0, "panic");
    run_task();
    CHECK(idx("Panic button on Keypad at ") >= 0 && h::alarm_attrs["night"] == "triggered" && idx("attr:alarm_sensor=Keypad") >= 0, "panic triggers the profile that is not disarmed");
    CHECK(alarm_arm("away", false, open, &n_open) == AlarmReply::BUSY, "busy while an intrusion alarm is on");
    CHECK(alarm_disarm("") == AlarmReply::OK, "disarm all");
    run_task();

    // ── Time moves the panel before every input: night's arm delay runs out while an output blocks, then the door
    //    opens (W15) ──
    CHECK(alarm_arm("night", false, open, &n_open) == AlarmReply::OK, "night arms: a 30 s delay");
    run_task();
    g_trace.clear();
    h::on_cmd = [] {                     // while the valve's command waits on the radio,
        g_now_us += 31 * 1000000ull;     // night's arm delay runs out,
        emit(0x1, "contact", 0);         // and the front door opens
    };
    emit(0x3, "water_leak", 0);
    emit(0x3, "water_leak", 1);          // a leak: the valve closes
    run_task();
    CHECK(h::alarm_attrs["night"] == "triggered" && idx("Front door (Night) at ") >= 0,
          "the door counts: night was armed by then, not still arming");
    alarm_disarm("");
    emit(0x1, "contact", 1);
    run_task();


    // ── A burst while an output blocks: steps collapse, but every alarm is told and the final state is saved ──
    CHECK(alarm_arm("away", false, open, &n_open) == AlarmReply::OK, "armed for the burst");
    run_task();
    g_trace.clear();
    h::on_cmd = [] {                      // while the siren's start waits on the radio:
        alarm_silence("away");            // the alarm is silenced (back to armed),
        emit(0x2, "occupancy", 1);        // motion sets it off again,
        emit(0x3, "water_leak", 0);
        emit(0x3, "water_leak", 1);       // a leak triggers flood,
        alarm_disarm("");                 // and "disarm all" disarms away and silences flood
    };
    emit(0x2, "occupancy", 1);
    run_task();
    CHECK(cnt("\"message\":\"Hall motion (Away) at ") == 2 && cnt("\"message\":\"Kitchen leak (Flood) at ") == 1 && cnt("\"message\":\"Disarmed (Away) at ") == 1,
          "both intrusion alarms, the flood alarm and the disarm are notified");
    CHECK(g_nvs["st2"].find("[\"away\",0,0]") != std::string::npos && g_nvs["st2"].find("[\"flood\",2,1]") != std::string::npos && g_nvs["st"] == std::string("\0\0", 2),
          "the final states are in flash");
    CHECK(h::alarm_attrs["away"] == "disarmed" && h::alarm_attrs["flood"] == "armed" && h::alarm_attrs["alarm_state"] == "disarmed" && h::alarm_attrs["alarm_sensor"].empty(),
          "the final states are shown");
    CHECK(idx("cmd:10:warning=burglar") >= 0 && idx("cmd:10:warning=0") > idx("cmd:10:warning=burglar"), "the siren that was started is stopped");
    CHECK(cnt("cmd:11:state=0") == 1 && cnt("cmd:") == 3,
          "the leak alarm was silenced before the task looked: its kept valve is closed all the same, and nothing else is driven (ruling 19)");
    CHECK(cnt("event:alarm_triggered_away") == 2 && cnt("event:alarm_triggered_flood") == 1 && cnt("event:alarm_disarmed_away") == 1, "the events of every alarm and of the end");
    CHECK(h::status().find("\"since\":0") == std::string::npos, "each profile has the time of its last change");

    // ── Silenced and set off again inside one busy pass: the siren starts again, for the whole alarm time (F2) ──
    CHECK(alarm_arm("away", false, open, &n_open) == AlarmReply::OK, "armed again");
    run_task();
    g_trace.clear();
    h::on_cmd = [] {                      // while the siren's start waits 4 s on the radio,
        g_now_us += 4000000;
        alarm_silence("away");            // the alarm is silenced,
        emit(0x2, "occupancy", 1);        // and motion sets it off again: a new alarm time
    };
    emit(0x2, "occupancy", 1);
    run_task();
    CHECK(cnt("cmd:10:warning=burglar,high,1,10") == 2 && cnt("cmd:") == 2,
          "the second alarm starts the siren again, for its whole time: the first start told it 10 s from the first alarm");
    alarm_disarm("");
    run_task();

    // ── A state that cannot be written still takes effect. A record that failed is tried again at the next change of
    //    state, else no sooner than 5 min after the failure, as the shadow waits: on a nearly full NVS each try can
    //    cost a page copy and a sector erase (round 2). Each record logs a run of failures once, and once its end; flash
    //    that comes to agree with the hub without a write ends a run too, so the next failure is logged again (M47) ──
    g_logs.clear();
    CHECK(alarm_arm("away", false, open, &n_open) == AlarmReply::OK, "armed");
    run_task();
    g_nvs_fail = true;
    alarm_disarm("");
    run_task();
    CHECK(h::alarm_attrs["away"] == "disarmed" && g_nvs["st2"].find("[\"away\",2,1]") != std::string::npos, "disarmed and shown, though not written");
    CHECK(g_nvs_refused["st"] == 1 && g_nvs_refused["st2"] == 1, "each record tried once");
    CHECK(h::logged("stage 1 state record not saved") == 1 && h::logged("states not saved") == 1, "each failure logged");
    run_task(9);   // nine passes with nothing new, 30 s apart
    CHECK(g_nvs_refused["st"] == 1 && g_nvs_refused["st2"] == 1, "no record is tried again within 5 min of its failure");
    CHECK(alarm_arm("away", false, open, &n_open) == AlarmReply::OK, "armed again, as flash still says: that run of failures is over");
    run_task();
    alarm_disarm("");   // the records that failed 4.5 min ago, but a change of state
    run_task();
    CHECK(g_nvs_refused["st"] == 2 && g_nvs_refused["st2"] == 2, "a change of state is tried at once");
    run_task(9);
    CHECK(g_nvs_refused["st"] == 2 && g_nvs_refused["st2"] == 2, "and then waits 5 min again");
    run_task(1);
    CHECK(g_nvs_refused["st"] == 3 && g_nvs_refused["st2"] == 3, "5 min after the failure, one more try of each, though nothing changed");
    CHECK(h::logged("stage 1 state record not saved") == 2 && h::logged("states not saved") == 2,
          "each record logs both runs of failures, each once: fail, agree, fail again (M47)");
    g_nvs_fail = false;
    g_trace.clear();
    run_task(9);
    CHECK(cnt("nvs:") == 0, "flash accepts again: still no try within 5 min of the failure");
    run_task(1);
    CHECK(g_nvs["st"] == std::string("\0\0", 2) && g_nvs["st2"].find("[\"away\",0,0]") != std::string::npos && cnt("nvs:") == 2,
          "5 min after the failure, both records are written, whether or not the panel moved (F4)");
    CHECK(h::logged("stage 1 state record saved again") == 1 && h::logged("states saved again") == 1, "each record says once that it is saved again");
    run_task(20);
    CHECK(cnt("nvs:") == 2 && h::logged("saved again") == 2 && h::logged("not saved") == 4, "nothing more to write, nothing more said");

    // ── The wait after a failure is for the same bytes only: a change to others, which flash does not hold either, is
    //    tried at once (M46) ──
    g_logs.clear();
    g_nvs_fail = true;
    CHECK(alarm_arm("away", false, open, &n_open) == AlarmReply::OK, "armed while flash refuses");
    run_task();
    {
        const int st_tries = g_nvs_refused["st"], st2_tries = g_nvs_refused["st2"];
        CHECK(alarm_arm("night", false, open, &n_open) == AlarmReply::OK, "night arming: other bytes than the failed ones and than flash's");
        run_task();
        CHECK(g_nvs_refused["st"] == st_tries + 1 && g_nvs_refused["st2"] == st2_tries + 1, "each record is tried at once, within 5 min of its failure");
    }
    CHECK(alarm_disarm("") == AlarmReply::OK, "disarmed: flash agrees again, nothing to write");
    run_task();
    g_nvs_fail = false;

    // ── Only the stage 1 record cannot be written: st2 is, and carries the st that flash still holds (ruling 17) ──
    g_nvs_fail_key = "st";
    CHECK(alarm_arm("away", false, open, &n_open) == AlarmReply::OK, "armed while st cannot be written");
    run_task();
    CHECK(g_nvs["st"] == std::string("\0\0", 2) && g_nvs["st2"].find("\"l\":[0,0]") != std::string::npos && g_nvs["st2"].find("[\"away\",2,1]") != std::string::npos,
          "a failed st write leaves the two agreeing: st2 holds the new state and the st bytes of before");
    g_nvs_fail_key.clear();
    alarm_disarm("");
    run_task();

    // ── A disarm lands while st is written and a re-arm before the pass ends (W1): st2 holds the look's states, as st
    //    does, so flash says armed, as the hub is ──
    CHECK(alarm_arm("away", false, open, &n_open) == AlarmReply::OK, "armed");
    g_nvs_hook = [](const char* key) {
        uint64_t o[kAlarmMaxSensors];
        uint8_t no = 0;
        if (std::strcmp(key, "st") == 0) alarm_disarm("away");   // while st goes to flash, away is disarmed,
        else if (std::strcmp(key, "st2") == 0) { g_nvs_hook = nullptr; alarm_arm("away", false, o, &no); }   // and armed again
    };
    run_task();
    CHECK(h::alarm_attrs["away"] == "armed" && g_nvs["st"] == std::string("\2\2", 2) && g_nvs["st2"].find("[\"away\",2,1]") != std::string::npos,
          "after the next pass, both records say armed, as the hub is");
    alarm_disarm("");
    run_task();

    // ── More refusals than the queue holds: the rest is logged, nothing breaks ──
    emit(0x1, "contact", 0);
    g_trace.clear();
    for (int i = 0; i < 17; i++) CHECK(h::cmd("away", "armed") == DEVCMD_BAD_VALUE, "refused");
    run_task();
    CHECK(cnt("\"kind\":\"alarm_arm_failed\"") == 16, "sixteen refusals told");
    emit(0x1, "contact", 1);
    run_task();

    // ── Checks of a save against the devices ──
    CHECK(!h::save(R"({"profiles":[{"id":"a","name":"A","class":"other","arm_delay":0,"trigger_delay":0,"siren_time":10,"notify_arm":false,"sensors":[{"ieee":"0x99","entry":false}],"outputs":[]}]})", &why) &&
          why == "profiles[0].sensors[0]: 0x0000000000000099 is not a device on this hub", why.c_str());
    CHECK(!h::save(R"({"profiles":[{"id":"a","name":"A","class":"other","arm_delay":0,"trigger_delay":0,"siren_time":10,"notify_arm":false,"sensors":[{"ieee":"0x1","entry":false},{"ieee":"0x10","entry":false}],"outputs":[]}]})", &why) &&
          why == "profiles[0].sensors[1]: Siren has no contact, occupancy, presence, vibration, water_leak, smoke, gas or carbon_monoxide", why.c_str());
    // M43: the row is the place the page listed the sensor at in its first profile, not its place among that profile's slots:
    // the Siren is b's first sensor (row 0), though the 0x1 that profile a has comes first by slot
    CHECK(!h::save(R"({"profiles":[{"id":"a","name":"A","class":"other","arm_delay":0,"trigger_delay":0,"siren_time":10,"notify_arm":false,"sensors":[{"ieee":"0x1","entry":false}],"outputs":[]},
          {"id":"b","name":"B","class":"other","arm_delay":0,"trigger_delay":0,"siren_time":10,"notify_arm":false,"sensors":[{"ieee":"0x10","entry":false},{"ieee":"0x1","entry":false}],"outputs":[]}]})", &why) &&
          why == "profiles[1].sensors[0]: Siren has no contact, occupancy, presence, vibration, water_leak, smoke, gas or carbon_monoxide", why.c_str());
    CHECK(!h::save(R"({"profiles":[{"id":"a","name":"A","class":"other","arm_delay":0,"trigger_delay":0,"siren_time":10,"notify_arm":false,"sensors":[],"outputs":[]},
          {"id":"b","name":"B","class":"other","arm_delay":0,"trigger_delay":0,"siren_time":10,"notify_arm":false,"sensors":[],"outputs":[{"ieee":"0x12","kind":"state","value":"on","keep":false},{"ieee":"0x11","kind":"warning","mode":"fire","level":"high","strobe":false}]}]})", &why) &&
          why.rfind("profiles[1].outputs[1]: Valve has no warning", 0) == 0, why.c_str());
    CHECK(!h::save(R"({"profiles":[],"keypads":[{"ieee":"0x77","trusted":true}]})", &why) && why == "keypads[0]: 0x0000000000000077 is not a device on this hub", why.c_str());
    {   // ruling 15: a profile that keeps its id keeps its class
        std::string changed = kCfg;
        const std::string from = "\"class\":\"water\"";
        changed.replace(changed.find(from), from.size(), "\"class\":\"fire\"");
        CHECK(!h::save(changed.c_str(), &why) && why == "profiles[2]: the class of a saved profile cannot change", why.c_str());
        CHECK(h::save(kCfg, &why), "the same config with the class it had still saves");
        run_task();
    }
    g_nvs_fail = true;
    CHECK(!h::save(kCfg, &why) && why == "could not save the config" && h::alarm_attrs.count("night") == 1, "a failed write changes nothing");
    g_nvs_fail = false;

    // ── A save that drops night and adds a fire profile whose smoke sensor is already active ──
    h::vals[{0x4, "smoke"}] = 1;
    g_trace.clear();
    h::persisted.clear();
    const std::string fire_cfg = R"({"profiles":[
      {"id":"fire","name":"Fire","class":"fire","arm_delay":0,"trigger_delay":0,"siren_time":15,"notify_arm":false,
       "sensors":[{"ieee":"0x4","entry":false}],"outputs":[{"ieee":"0x10","kind":"warning","mode":"fire","level":"very_high","strobe":true},{"ieee":"0x12","kind":"state","value":"on","keep":false}]},
      {"id":"away","name":"Away","class":"intrusion","role":"away","arm_delay":0,"trigger_delay":5,"siren_time":10,"notify_arm":true,
       "sensors":[{"ieee":"0x1","entry":true}],"outputs":[{"ieee":"0x10","kind":"warning","mode":"burglar","level":"high","strobe":true}]},
      {"id":"flood","name":"Flood","class":"water","arm_delay":0,"trigger_delay":0,"siren_time":20,"notify_arm":true,
       "sensors":[{"ieee":"0x3","entry":false}],"outputs":[{"ieee":"0x11","kind":"state","value":"off","keep":true}]}]})";
    CHECK(h::save(fire_cfg.c_str(), &why), why.c_str());
    CHECK(h::persisted == std::vector<uint64_t>({0x4}), "only the sensor the save adds is persisted soon: the kept ones were already");
    run_task();
    CHECK(h::alarm_attrs.count("night") == 0 && h::alarm_attrs["fire"] == "triggered" && h::alarm_attrs["flood"] == "armed" && h::alarm_attrs["away"] == "disarmed",
          "night gone; flood kept its state under its new index; the new fire profile armed and went off for the smoke that was there");
    CHECK(idx("Smoke (Fire) at ") >= 0 && idx("cmd:10:warning=fire,very_high,1,15") >= 0 && idx("cmd:12:state=1") >= 0 && cnt("Armed (Flood)") == 0, "its alert and outputs; no second notice for flood");
    CHECK(alarm_silence("fire") == AlarmReply::OK, "silence");
    g_trace.clear();
    run_task();
    CHECK(h::alarm_attrs["fire"] == "armed" && idx("cmd:10:warning=0") >= 0 && idx("cmd:12:state=0") >= 0, "silenced: back to armed, outputs stopped");

    // ── A leak, its silence and a save that removes its valve, all while an output command blocks: the alarm's outputs
    //    are those of the config it went off under, so the valve closes (M31) ──
    {
        std::string no_valve = fire_cfg;
        const std::string valve = R"({"ieee":"0x11","kind":"state","value":"off","keep":true})";
        no_valve.erase(no_valve.find(valve), valve.size());
        auto round = [](const std::string& cfg) {
            g_trace.clear();
            h::on_cmd = [cfg] {              // while fire's siren start waits on the radio:
                alarm_silence("fire");       // fire is silenced,
                emit(0x3, "water_leak", 0);
                emit(0x3, "water_leak", 1);  // a leak sets off flood,
                alarm_silence("flood");      // which is silenced too,
                std::string w;
                CHECK(h::save(cfg.c_str(), &w), w.c_str());   // and `cfg` is saved
            };
            emit(0x4, "smoke", 0);
            emit(0x4, "smoke", 1);           // smoke again: fire's alarm
            run_task();
        };
        round(no_valve);                     // the save takes flood's valve away
        CHECK(idx("Kitchen leak (Flood) at ") >= 0 && cnt("cmd:11:state=0") == 1,
              "the leak is told, and the valve it went off with is closed, though the save removed it");
        CHECK(idx("cmd:10:warning=0") >= 0 && idx("cmd:12:state=0") >= 0, "fire's outputs stop after its silence");
        round(fire_cfg);                     // the same, and the save brings the valve back
        CHECK(idx("Kitchen leak (Flood) at ") >= 0 && cnt("cmd:11:") == 0, "that leak went off without a valve: the one the save adds is left alone");
    }

    // ── A disarm the task has not acted on, then a save that drops the output: it is stopped by its old entry ──
    emit(0x4, "smoke", 0);
    emit(0x4, "smoke", 1);
    run_task();
    CHECK(h::alarm_attrs["fire"] == "triggered", "smoke again: the alarm");
    CHECK(alarm_disarm("fire") == AlarmReply::OK, "disarmed, the task not run yet");
    g_trace.clear();
    CHECK(h::save(R"({"profiles":[{"id":"fire","name":"Fire","class":"fire","arm_delay":0,"trigger_delay":0,"siren_time":15,"notify_arm":false,
       "sensors":[{"ieee":"0x4","entry":false}],"outputs":[]}]})", &why), why.c_str());
    run_task();
    CHECK(idx("cmd:10:warning=0") >= 0 && idx("cmd:12:state=0") >= 0 && cnt("cmd:") == 2, "the outputs the save removed are stopped");
    CHECK(h::alarm_attrs["fire"] == "disarmed" && h::alarm_attrs.count("away") == 0, "and the state is the one after the disarm");
    int reopened = 0;
    for (const auto& c : g_all_cmds) reopened += c.find("cmd:11:state=1") != std::string::npos;
    CHECK(reopened == 0 && !g_all_cmds.empty(), "the kept valve never got the opposite value, in the whole run");

    // ── Two leaks the task has not looked at, and between them a save that gives the kept valve the other value: the
    //    valve ends as the newer alarm's config sets it ──
    {
        const std::string closes = R"({"profiles":[{"id":"flood","name":"Flood","class":"water","arm_delay":0,"trigger_delay":0,"siren_time":20,
          "notify_arm":false,"sensors":[{"ieee":"0x3","entry":false}],"outputs":[{"ieee":"0x11","kind":"state","value":"off","keep":true}]}]})";
        std::string opens = closes;
        opens.replace(opens.find("\"off\""), 5, "\"on\"");
        emit(0x3, "water_leak", 0);          // dry: the new profile does not go off at the save
        CHECK(h::save(closes.c_str(), &why), why.c_str());
        run_task();
        g_trace.clear();
        h::on_cmd = [opens] {                // while the valve closes for a first leak:
            alarm_silence("flood");
            emit(0x3, "water_leak", 0);
            emit(0x3, "water_leak", 1);      // a second leak, under the config that closes the valve,
            alarm_silence("flood");
            std::string w;
            CHECK(h::save(opens.c_str(), &w), w.c_str());   // a save that opens it instead,
            emit(0x3, "water_leak", 0);
            emit(0x3, "water_leak", 1);      // and a third leak, under the new config
            alarm_silence("flood");
        };
        emit(0x3, "water_leak", 1);
        run_task();
        std::string last;
        for (const auto& s : g_trace) if (s.rfind("cmd:11:", 0) == 0) last = s;
        CHECK(cnt("cmd:11:state=0") == 2 && last == "cmd:11:state=1",
              "the valve is closed for the second leak, then opened for the third: it ends as the newer alarm's config sets it");
    }

    // ── A panic in the arm delay, a smoke sensor of the profile still on: the panic's end in armed sets the smoke off in
    //    the same tick (ruling 7), and the siren starts again (W4) ──
    CHECK(h::save(R"({"profiles":[{"id":"away","name":"Away","class":"intrusion","role":"away","arm_delay":5,"trigger_delay":0,"siren_time":10,
       "notify_arm":false,"sensors":[{"ieee":"0x4","entry":false}],"outputs":[{"ieee":"0x10","kind":"warning","mode":"burglar","level":"high","strobe":true}]}],
       "keypads":[{"ieee":"0x20","trusted":false}]})", &why), why.c_str());
    run_task();
    CHECK(alarm_arm("away", false, open, &n_open) == AlarmReply::OK, "away arms, its smoke sensor still on");
    run_task();
    g_trace.clear();
    emit(0x20, "action", 0, "panic");
    run_task();
    CHECK(h::alarm_attrs["away"] == "triggered" && cnt("cmd:10:warning=burglar,high,1,10") == 1, "the panic in the arm delay: the siren for 10 s");
    run_task(10);
    CHECK(h::alarm_attrs["away"] == "triggered" && idx("Smoke (Away) at ") >= 0 && cnt("cmd:10:warning=burglar,high,1,10") == 2,
          "the panic ends in armed and the smoke goes off at once: the siren starts again, for the whole time");
    alarm_disarm("");
    run_task();
    // ── The largest device and status: eight profiles with the longest ids and names ──
    {
        std::string big = "{\"profiles\":[";
        for (int i = 0; i < 8; i++) {
            char one[256];
            std::snprintf(one, sizeof(one), "%s{\"id\":\"p%dxxxxxxxxxxx\",\"name\":\"Profile number %d xxxxxxx\",\"class\":\"intrusion\",\"arm_delay\":0,\"trigger_delay\":0,"
                          "\"siren_time\":10,\"notify_arm\":false,\"sensors\":[],\"outputs\":[]}", i ? "," : "", i, i);
            big += one;
        }
        big += "]}";
        CHECK(h::save(big.c_str(), &why), why.c_str());
        run_task();
        static char buf[4096];
        const size_t n = h::local_exposes_fn(kAlarmIeee, buf, sizeof(buf));
        std::printf("note: exposes of 8 profiles: %zu B; status of 8 idle profiles: %zu B\n", n, h::status().size());
        CHECK(n > 0 && n < 2048, "the exposes of eight profiles fit the callers' 2048-byte buffers");
        CHECK(h::alarm_attrs.size() == 8 + 3, "eight profile attributes, alarm_state, alarm_sensor (and the shadow's own)");
    }
    return 0;
}

// A restart during an alarm: `legacy` = only the stage 1 records are there.
int boot_flow(bool legacy) {
    if (legacy) {
        g_nvs["cfg"] = R"({"exit_delay":45,"entry_delay":20,"siren_time":120,"notify_arm":true,
          "sensors":[{"ieee":"0x1","modes":["away","home","night"],"entry":true}],
          "sirens":[{"ieee":"0x10","kind":"warning","mode":"burglar","level":"very_high","strobe":false},{"ieee":"0x12","kind":"state"}],
          "keypads":[{"ieee":"0x20","trusted":false}],"codes":["4321"]})";
        g_nvs["st"] = std::string("\6\2", 2);   // triggered, rests armed_away
    } else {
        static AlarmConfig c;
        char err[96];
        static char buf[kAlarmStoredMax];
        if (!alarm_cfg_from_json(kCfg, std::strlen(kCfg), nullptr, &c, err, sizeof(err))) return 1;
        g_nvs["cfg2"] = std::string(buf, alarm_cfg_pack(c, buf, sizeof(buf)));
        g_nvs["st2"] = R"({"v":1,"l":[6,2],"p":[["away",4,1],["night",0,0],["flood",4,1]]})";
        g_nvs["st"] = std::string("\6\2", 2);
    }
    const std::string cfg_before = g_nvs["cfg"];
    h::vals[{0x1, "contact"}] = 0;   // the door that set it off is still open
    h::vals[{0x2, "occupancy"}] = 1;   // and flash says the hall is occupied
    h::boot();
    CHECK(h::reads_before_sub == 0 && h::seed_reads > 0,
          "subscribed before the seeds are read: a report in between waits for the event pump, after them, and is not lost (W16)");
    run_task(58);
    {
        auto asked = [](size_t n, unsigned caps) {
            for (const auto& a : heap_asked()) if (a.first == n && a.second == caps) return true;
            return false;
        };
        CHECK(asked(sizeof(Pass), MALLOC_CAP_SPIRAM) && asked(2 * sizeof(ZapDevice), MALLOC_CAP_SPIRAM) &&
              asked(kAlarmMaxSensors * sizeof(Seed), MALLOC_CAP_SPIRAM) && asked(kBootStopMax, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
              "the task's pass, the Alarm device's copies and the seeds are in PSRAM; the boot stop record is read into internal RAM (M49)");
        CHECK(asked(kAlarmStoredMax, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT), "the config is read into internal RAM too: an NVS read target");
    }
    CHECK(h::alarm_attrs["away"] == "armed" && h::alarm_attrs["alarm_state"] == "armed_away", "back in the state it rested in");
    CHECK(cnt("nvs:") == 1 && pos("nvs:bt") == 0 && cnt("alert:") == 0 && cnt("cmd:") == 0 && cnt("event:") == 0,
          "in the first minute only the boot stop's own record is written, at boot (M30); nothing told or stopped");
    CHECK(g_nvs["bt"] == (legacy ? std::string("\1\2\x10\0\0\0\0\0\0\0\0\0\0\x12\0\0\0\0\0\0\0\2\1\0", 24)
                                 : std::string("\1\2\x10\0\0\0\0\0\0\0\0\0\0\x11\0\0\0\0\0\0\0\2\0\1", 24)),
          "it lists what sounded, each device once: the siren, and the switch siren or the kept valve");
    if (!legacy) CHECK(h::alarm_attrs["flood"] == "armed" && h::alarm_attrs["night"] == "disarmed", "every profile by its id");
    else CHECK(h::alarm_attrs["home"] == "disarmed" && h::status().find("\"name\":\"Night\"") != std::string::npos, "the stage 1 config as away, home, night");
    run_task(5);
    const int told = idx("The hub restarted during an alarm at "), stop = idx("cmd:10:warning=0"), st = pos("nvs:st"), st2 = pos("nvs:st2");
    CHECK(told >= 0 && cnt("\"kind\":\"alarm\"") == 1 && idx("tg:Alarm: The hub restarted during an alarm at ") >= 0, "the restart notice, once");
    CHECK(stop > told && st > stop && st2 > st, "told, stopped, and only then the states written, st first");
    CHECK(cnt("nvs:st2") == 1 && cnt("nvs:") == 3 && g_nvs["st"] == std::string("\2\2", 2), "the records written once: the next restart is not during an alarm");
    CHECK(idx("nvs-erase:bt") > st2 && g_nvs.count("bt") == 0, "and then the boot stop's record is erased: its job is done");
    CHECK(cnt("nvs:cfg") == 0 && g_nvs["cfg"] == cfg_before, "the config records are not touched at boot");
    if (legacy) CHECK(idx("cmd:12:state=0") > told && cnt("cmd:") == 2, "the switch siren goes off too");
    else CHECK(cnt("cmd:") == 2 && cnt("cmd:11:state=0") == 1 && idx("cmd:11:state=0") > told && st > idx("cmd:11:state=0") && cnt("cmd:11:state=1") == 0,
               "the kept valve is closed again by the boot stop, before the states are written: the restart may have come before its command (W14)");
    CHECK(cnt("event:") == 0 && cnt("attr:alarm_state") == 1, "no event and no second alarm_state for a restart");
    if (!legacy) {   // motion keeps no last value: an "occupied" from flash swallows nothing (ruling on F1)
        emit(0x2, "occupancy", 1);
        run_task();
        CHECK(h::alarm_attrs["away"] == "triggered", "a boot with flash saying occupied counts the next occupied report");
    }
    return 0;
}

// The hub fell back to the stage 1 firmware, was armed there, and is updated again: "st" is newer than "st2" (ruling 17).
int fallback_flow() {
    static AlarmConfig c;
    char err[96];
    static char buf[kAlarmStoredMax];
    if (!alarm_cfg_from_json(kCfg, std::strlen(kCfg), nullptr, &c, err, sizeof(err))) return 1;
    g_nvs["cfg2"] = std::string(buf, alarm_cfg_pack(c, buf, sizeof(buf)));
    g_nvs["st2"] = R"({"v":1,"l":[0,0],"p":[["away",0,0],["night",0,0],["flood",2,1]]})";   // written beside st = disarmed
    g_nvs["st"] = std::string("\2\2", 2);                                                   // stage 1 wrote armed_away since
    h::boot();
    run_task();
    CHECK(h::alarm_attrs["away"] == "armed" && h::alarm_attrs["alarm_state"] == "armed_away" && h::alarm_attrs["flood"] == "armed" && h::alarm_attrs["night"] == "disarmed",
          "armed on stage 1 since: the hub comes up armed; flood from st2");
    CHECK(cnt("alert:") == 0 && cnt("event:") == 0 && cnt("cmd:") == 0, "a boot tells and drives nothing");
    CHECK(cnt("nvs:") == 1 && g_nvs["st2"] == R"({"v":1,"l":[2,2],"p":[["away",2,1],["night",0,0],["flood",2,1]]})" && g_nvs["st"] == std::string("\2\2", 2),
          "st2 is written once, beside the st it was read with (F4); st is left as stage 1 wrote it");
    g_trace.clear();
    uint64_t open[kAlarmMaxSensors];
    uint8_t n_open = 0;
    CHECK(alarm_arm("night", false, open, &n_open) == AlarmReply::OK, "night armed on this firmware");
    run_task();
    CHECK(pos("nvs:st") >= 0 && pos("nvs:st") < pos("nvs:st2") && g_nvs["st"] == std::string("\1\4", 2) &&
          g_nvs["st2"] == R"({"v":1,"l":[1,4],"p":[["away",0,0],["night",1,1],["flood",2,1]]})", "the next change writes st, then st2 beside it");
    return 0;
}

std::string packed(const char* json) {   // a config as "cfg2" holds it
    static AlarmConfig c;
    static char buf[kAlarmStoredMax];
    char err[96];
    if (!alarm_cfg_from_json(json, std::strlen(json), nullptr, &c, err, sizeof(err))) { std::printf("FAIL: %s\n", err); std::abort(); }
    return std::string(buf, alarm_cfg_pack(c, buf, sizeof(buf)));
}

// Power cuts and restarts: chains of boots on one flash (h::boot_run). This process never boots itself.
int restart_flow() {
    using h::boot_run;
    static uint64_t open[kAlarmMaxSensors];
    static uint8_t n_open = 0;
    // A flood profile whose alarm switches a relay on; restarted while it sounds.
    const char* relay = R"({"profiles":[{"id":"flood","name":"Flood","class":"water","arm_delay":0,"trigger_delay":0,"siren_time":20,
      "notify_arm":false,"sensors":[{"ieee":"0x3","entry":false}],"outputs":[{"ieee":"0x12","kind":"state","value":"on","keep":false}]}],
      "keypads":[{"ieee":"0x20","trusted":true}]})";
    const std::map<std::string, std::string> sounding = {
        {"cfg2", packed(relay)}, {"st2", R"({"v":1,"l":[0,0],"p":[["flood",4,1]]})"}, {"st", std::string("\0\0", 2)}};

    // ── A save inside the minute after that restart, which swaps the alarm's output for a siren: the boot stop stops what
    //    sounded at the restart, not what the save lists (W9, M29) ──
    std::string siren = relay;
    const std::string out12 = R"({"ieee":"0x12","kind":"state","value":"on","keep":false})";
    siren.replace(siren.find(out12), out12.size(), R"({"ieee":"0x10","kind":"warning","mode":"emergency","level":"high","strobe":true})");
    g_nvs = sounding;
    boot_run([&] {
        h::boot();
        run_task(20);
        std::string why;
        CHECK(h::save(siren.c_str(), &why), why.c_str());
        run_task(38);
        CHECK(cnt("cmd:") == 0, "nothing is stopped before the minute is over");
        run_task(5);
        CHECK(idx("The hub restarted during an alarm at ") >= 0 && cnt("cmd:12:state=0") == 1 && cnt("cmd:10:") == 0,
              "a minute after the restart, the relay that was on is switched off, and the siren the save lists is left alone");
    });

    // ── A disarm inside that minute stops what sounded at the restart at once; the notice still waits for the minute
    //    (F5/W10, M30) ──
    g_nvs = sounding;
    boot_run([] {
        h::boot();
        run_task(10);
        g_trace.clear();
        CHECK(alarm_disarm("") == AlarmReply::OK, "disarm, 10 s after the restart");
        run_task();
        CHECK(cnt("cmd:12:state=0") == 1 && cnt("alert:") == 0, "the relay is switched off at once; the notice waits");
        g_trace.clear();
        run_task(55);
        CHECK(idx("The hub restarted during an alarm at ") >= 0 && cnt("cmd:") == 0, "the notice a minute after the restart; nothing is stopped twice");
    });
    g_nvs = sounding;
    boot_run([] {
        h::boot();
        run_task(10);
        g_trace.clear();
        emit(0x20, "action", 0, "disarm");   // a trusted keypad: no code
        run_task();
        CHECK(cnt("cmd:12:state=0") == 1, "a keypad's disarm stops it at once too");
    });

    // ── A second restart inside that minute, after a save that rewrote the states and swapped the output: the next boot
    //    still tells and stops what sounded at the first restart (F5/W10, M30) ──
    g_nvs = sounding;
    boot_run([&] {
        h::boot();
        run_task(20);
        std::string why;
        CHECK(h::save(siren.c_str(), &why), why.c_str());
        run_task(10);
        CHECK(g_nvs["st2"] == R"({"v":1,"l":[0,0],"p":[["flood",2,1]]})" && cnt("cmd:") == 0, "the states say armed now; nothing stopped yet");
    });   // the power goes 30 s after the restart
    boot_run([] {
        h::boot();
        run_task(58);
        CHECK(cnt("alert:") == 0 && cnt("cmd:") == 0, "nothing in the first minute");
        run_task(5);
        CHECK(idx("The hub restarted during an alarm at ") >= 0 && cnt("cmd:12:state=0") == 1 && cnt("cmd:10:") == 0,
              "a minute after the second restart: the notice, and the relay of the first one switched off");
        CHECK(g_nvs.count("bt") == 0, "and the job is done");
    });
    boot_run([] {
        h::boot();
        run_task(70);
        CHECK(cnt("alert:") == 0 && cnt("cmd:") == 0, "the next boot has nothing left to tell or stop");
    });

    // ── The same, but both writes of the states fail, and the save makes the relay a kept output: the next boot still finds
    //    flood sounding, so its config lists the relay as well, and "bt", what was on at the first restart, wins ──
    std::string kept_relay = relay;
    kept_relay.replace(kept_relay.find(out12), out12.size(), R"({"ieee":"0x12","kind":"state","value":"on","keep":true})");
    g_nvs = sounding;
    boot_run([&] {
        h::boot();
        run_task(20);
        g_nvs_fail_key = "st2";
        std::string why;
        CHECK(h::save(kept_relay.c_str(), &why), why.c_str());
        run_task(10);
        CHECK(g_nvs["st2"] == sounding.at("st2") && cnt("cmd:") == 0, "the states still say flood sounds; nothing stopped yet");
    });   // the power goes 30 s after the restart
    boot_run([] {
        h::boot();
        run_task(63);
        CHECK(idx("The hub restarted during an alarm at ") >= 0 && cnt("cmd:12:state=0") == 1 && cnt("cmd:12:state=1") == 0,
              "a minute after the second restart, the relay is switched off as it sounded at the first, not set as the save keeps it");
    });

    // ── A foreign "bt" that keeps a siren: only a switch is left as set, so the siren is stopped, not started ──
    g_nvs = {{"cfg2", packed(relay)}, {"st2", R"({"v":1,"l":[0,0],"p":[["flood",2,1]]})"}, {"st", std::string("\0\0", 2)},
             {"bt", std::string("\1\1" "\x10\0\0\0\0\0\0\0" "\0\0\1", 13)}};   // the siren 0x10, warning, keep
    boot_run([] {
        h::boot();
        run_task(63);
        CHECK(idx("The hub restarted during an alarm at ") >= 0 && cnt("cmd:10:warning=0") == 1 && cnt("cmd:10:") == 1,
              "a siren that a foreign record keeps is stopped a minute after the restart, not started");
    });

    // ── A power cut right after a save: every profile comes back in its state at the save, a new one too (W6, M28) ──
    std::string with_fire = kCfg;
    with_fire.insert(with_fire.find("],\n \"keypads\""), R"(,
 {"id":"fire","name":"Fire","class":"fire","arm_delay":0,"trigger_delay":0,"siren_time":15,"notify_arm":false,
  "sensors":[{"ieee":"0x4","entry":false}],"outputs":[]})");
    g_nvs.clear();
    boot_run([&] {
        h::boot();
        run_task();
        std::string why;
        CHECK(h::save(kCfg, &why), why.c_str());
        run_task();
        CHECK(alarm_arm("away", false, open, &n_open) == AlarmReply::OK, "away armed");
        run_task();
        CHECK(h::save(with_fire.c_str(), &why), why.c_str());   // a fire profile more: it guards at once
    });   // the power goes before the task's next pass
    boot_run([] {
        h::boot();
        run_task();
        CHECK(h::alarm_attrs["fire"] == "armed" && h::alarm_attrs["away"] == "armed" && h::alarm_attrs["flood"] == "armed" &&
              h::alarm_attrs["night"] == "disarmed", "after the cut, every profile is in its state at the save, the new one too");
    });

    // ── While the siren's stop waits on the radio: a disarm, a save of the same config and an arm again (or the mirror:
    //    arm, save, disarm). The save writes the states itself, behind the task's copy of flash: the task's next pass
    //    still writes the hub's states, though they are the ones it wrote last (final review, Important 1) ──
    static const char* const fire_away = R"({"profiles":[
      {"id":"fire","name":"Fire","class":"fire","arm_delay":0,"trigger_delay":0,"siren_time":15,"notify_arm":false,
       "sensors":[{"ieee":"0x4","entry":false}],"outputs":[{"ieee":"0x10","kind":"warning","mode":"fire","level":"high","strobe":true}]},
      {"id":"away","name":"Away","class":"intrusion","role":"away","arm_delay":0,"trigger_delay":5,"siren_time":10,"notify_arm":false,
       "sensors":[{"ieee":"0x1","entry":true}],"outputs":[]}]})";
    for (const bool armed : {true, false}) {
        g_nvs.clear();
        boot_run([armed] {
            h::boot();
            run_task();
            std::string why;
            CHECK(h::save(fire_away, &why), why.c_str());
            run_task();
            if (armed) CHECK(alarm_arm("away", false, open, &n_open) == AlarmReply::OK, "away armed");
            run_task();
            emit(0x4, "smoke", 1);
            run_task();
            CHECK(h::alarm_attrs["fire"] == "triggered", "fire sounds");
            h::on_cmd = [armed] {   // while the siren's stop waits on the radio:
                std::string w;
                if (armed) alarm_disarm("away"); else alarm_arm("away", false, open, &n_open);
                CHECK(h::save(fire_away, &w), w.c_str());   // the save writes away's state of the moment,
                if (armed) alarm_arm("away", false, open, &n_open); else alarm_disarm("away");   // and away goes back
            };
            alarm_silence("fire");
            run_task();
            run_task(4);   // four passes with nothing new, 30 s apart
            CHECK(h::alarm_attrs["away"] == (armed ? "armed" : "disarmed"), "the hub is back in away's state of before");
            CHECK((g_nvs["st2"].find("[\"away\",2,1]") != std::string::npos) == armed,
                  armed ? "disarm, save, arm: flash says armed, as the hub does" : "arm, save, disarm: flash says disarmed, as the hub does");
        });   // the power goes
        boot_run([armed] {
            h::boot();
            run_task();
            CHECK(h::alarm_attrs["away"] == (armed ? "armed" : "disarmed"),
                  armed ? "after a power cut, away is armed, as it was" : "after a power cut, away is disarmed, as it was");
        });
    }

    // ── The second form: an arm that the task writes between the save's pack of the states and its write of them. The
    //    save's older bytes land over it, and the task's next pass writes the hub's states again: also inside the minute
    //    after a restart during an alarm, where a pass with nothing new writes no state record. Flood, as "st" (which
    //    follows the intrusion profiles) cannot bring it back ──
    for (const bool minute : {false, true}) {
        g_nvs = minute ? sounding : std::map<std::string, std::string>{};
        boot_run([minute] {
            h::boot();
            run_task(minute ? 20 : 0);
            std::string why;
            CHECK(h::save(kCfg, &why), why.c_str());
            CHECK(alarm_disarm("flood") == AlarmReply::OK, "flood disarmed");
            run_task();
            g_nvs_before = [](const char* key) {   // the save has packed the states and is about to write them:
                if (std::strcmp(key, "st2") != 0) return;
                g_nvs_before = nullptr;
                alarm_arm("flood", false, open, &n_open);   // flood is armed,
                run_task();                                 // and the task writes that first
            };
            CHECK(h::save(kCfg, &why), why.c_str());
            CHECK(h::alarm_attrs["flood"] == "armed" && g_nvs["st2"].find("[\"flood\",0,0]") != std::string::npos,
                  "the save's states, packed before the arm, land over the task's");
            run_task();   // one pass, with nothing new
            CHECK(g_nvs["st2"].find("[\"flood\",2,1]") != std::string::npos,
                  minute ? "inside the minute, the next pass writes the hub's states: flood armed" : "the next pass writes the hub's states: flood armed");
        });   // the power goes
        boot_run([] {
            h::boot();
            run_task();
            CHECK(h::alarm_attrs["flood"] == "armed", "after a power cut, flood comes back armed");
        });
    }
    return 0;
}

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "main";
    const int rc = mode == "main" ? main_flow() : mode == "fallback" ? fallback_flow() : mode == "restart" ? restart_flow()
                 : boot_flow(mode == "legacy");
    if (rc) { std::printf("FAIL: setup\n"); return 1; }
    if (g_lock_depth) { std::printf("FAIL: the mutex is still held\n"); return 1; }
    std::printf("%s: %s (%d checks, %d failed)\n", mode.c_str(), h::fails ? "FAILED" : "ALL PASSED", h::checks, h::fails);
    return h::fails != 0;
}
