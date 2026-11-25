// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Alarm panel ("Safety") glue. Every change to the panel happens under s_mu in act(), which then
// wakes the "alarm" task. The task keeps no queue of changes: each pass it asks the panel what is
// different from what it acted on last, so a burst may collapse steps but never loses a profile's
// final state or an alarm. The task does everything that can wait on the flash, the radio or the
// network: the state records, notifications, the Alarm device, rule events, outputs, the page.
#include "alarm.h"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <new>
#include <utility>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#include "device_cmd.h"
#include "device_shadow.h"
#include "event_bus.h"
#include "local_dev.h"
#include "psram_json.h"
#include "remote_client.h"
#include "tg_gw.h"
#include "ws_bridge.h"
#include "zap_common.h"
#include "zap_store.h"
#include "zcl_attribute.h"
#include "zhc_adapter.h"
#include "zigbee_pool.h"

namespace {

const char* const TAG = "alarm";
constexpr char     kNs[] = "alarm";
constexpr size_t   kExposesMax = 2048;
constexpr size_t   kStatesMax = 256;        // "st2": eight profiles pack to 200 bytes
constexpr size_t   kSummaryMax = 24;        // "armed_" + an id
constexpr uint8_t  kEventsMax = 20;         // one pass gives 18 names at most
constexpr uint32_t kBootAlertDelayS = 60;   // the network and the cloud link come up first
constexpr size_t   kBootItem = 11;          // "bt", per output: ieee (8 bytes, as the chip holds it), kind, value, keep
constexpr size_t   kBootStopMax = 2 + kAlarmMaxOutputs * kBootItem;   // ... after the version (1) and the count
constexpr uint32_t kSaveRetryS = 300;       // a state record that failed to write waits this long, unless it changes

SemaphoreHandle_t s_mu = nullptr;   // guards everything below up to s_ids_changed
struct Lock {
    Lock() { xSemaphoreTake(s_mu, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(s_mu); }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
};

AlarmConfig* s_cfg = nullptr;     // heap: ~4.4 KB; a save swaps both
AlarmPanel*  s_panel = nullptr;
struct PadInput {
    char action[24];
    char code[16];   // a secret: cleared once used
    bool have;
};
PadInput     s_pad[kAlarmMaxKeypads];
AlarmSeen    s_seen[kAlarmMaxProfiles];     // what the task acted on last
uint32_t     s_since[kAlarmMaxProfiles];    // wall clock of each profile's last change; 0 while the clock is unset
int8_t       s_follows[kAlarmMaxOutputs];   // by device of the drive plan: the output it was last told to follow, -1
// What ending an output's alarm takes: a stop, or for one left as set (`keep`) its value again. A save lists here the
// outputs that still followed an alarm under the config it replaced, and the ones left as set of an alarm the task has
// not acted on; the task drives them before anything by the new config.
struct Orphan { uint64_t ieee; AlarmOutKind kind; bool value, keep; };
Orphan       s_orphans[kAlarmMaxOutputs];
uint8_t      s_n_orphans = 0;
uint32_t     s_kept_alarms[kAlarmMaxProfiles];   // by profile: the alarms whose outputs left as set are set or queued
bool         s_boot_stop_now = false;   // a disarm: the boot stop's outputs are not left on until its minute is over
bool         s_resync = false;        // a save swapped the config: the task takes a pass over it
bool         s_ids_changed = false;   // ... and the Alarm device has another list of attributes

// An arm refused for open doors or windows, not from the hub page (which lists them itself).
struct Refusal {
    char     id[kAlarmIdMax];
    char     name[kAlarmNameMax];
    uint64_t first;   // the first open sensor
    uint8_t  n;       // how many are open
};
QueueHandle_t s_refused = nullptr;
TaskHandle_t  s_task = nullptr;

// The task's own; set at boot before it starts.
bool     s_boot_alert = false;              // restarted during pending or triggered
// The boot stop: the outputs of what sounded at the restart, by the config of then (a save since changes nothing),
// each device once. PSRAM, and only while one is due.
Orphan*  s_boot_stop = nullptr;
uint8_t  s_n_boot_stop = 0;
char     s_st2_saved[kStatesMax];           // the "st2" record flash holds
size_t   s_st2_len = 0;
uint8_t  s_st_saved[2] = {0, 0};            // the stage 1 "st" record flash holds, when s_st_have
bool     s_st_have = false;
// A state record's write that failed: the same bytes are not tried again for kSaveRetryS. On a nearly full NVS each
// try can cost a page copy and a sector erase; the shadow waits as long after one.
template <size_t N>
struct SaveRetry {
    bool     failing;   // no write of the record landed since one failed: the failure is logged once
    uint32_t at;        // the last failed try
    size_t   len;       // ... and what it tried; 0 once flash held what the hub had (what comes next is a change)
    uint8_t  tried[N];
};
SaveRetry<2>          s_st_retry{};
SaveRetry<kStatesMax> s_st2_retry{};
char     s_summary[kSummaryMax] = "disarmed";   // the alarm_state acted on last
uint64_t s_summary_cause = 0;               // ... and the device behind its alarm_sensor

uint32_t mono_s() { return static_cast<uint32_t>(esp_timer_get_time() / 1000000); }

uint32_t wall_s() {
    const time_t t = time(nullptr);
    return t > 1577836800 ? static_cast<uint32_t>(t) : 0;
}

void ieee_str(uint64_t ieee, char (&out)[19]) { std::snprintf(out, sizeof(out), "0x%016" PRIX64, ieee); }

// A device's friendly name, else its ieee. Not for app_main: a ZapDevice is 522 B.
void name_of(uint64_t ieee, char (&out)[32]) {
    ZapDevice d{};
    if (zigbee_pool_snapshot(ieee, &d) && d.friendly_name[0]) std::snprintf(out, sizeof(out), "%s", d.friendly_name);
    else std::snprintf(out, sizeof(out), "0x%016" PRIX64, ieee);
}

bool nvs_put(const char* key, const void* data, size_t len) {
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t e = nvs_set_blob(h, key, data, len);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK;
}

bool nvs_drop(const char* key) {   // gone, also when it was never there
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t e = nvs_erase_key(h, key);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK || e == ESP_ERR_NVS_NOT_FOUND;
}

size_t nvs_get(const char* key, void* data, size_t cap) {
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READONLY, &h) != ESP_OK) return 0;
    size_t len = cap;
    const esp_err_t e = nvs_get_blob(h, key, data, &len);
    nvs_close(h);
    return e == ESP_OK ? len : 0;
}

// ── A sensor's last values, from the device shadow ──

bool key_active(AlarmKey k, int32_t v) { return k == AlarmKey::CONTACT ? v == 0 : v != 0; }   // a contact reports "closed"

// The keys whose last value the panel keeps; motion, presence and vibration are events only.
const char* const kLevelKeys[] = {"contact", "water_leak", "smoke", "gas", "carbon_monoxide"};
constexpr size_t kLevelKeyCount = sizeof(kLevelKeys) / sizeof(kLevelKeys[0]);
struct Seed {
    uint64_t ieee;
    int8_t   v[kLevelKeyCount];   // -1 unknown, else 0 or 1
};

// Never under s_mu: the shadow's lock is not taken under it.
void read_seeds(Seed* s, uint8_t n) {
    for (uint8_t i = 0; i < n; i++) {
        for (size_t k = 0; k < kLevelKeyCount; k++) {
            ShadowAttr a{};
            const bool have = device_shadow_get_attr(s[i].ieee, kLevelKeys[k], &a) && (a.val_type == VAL_BOOL || a.val_type == VAL_INT);
            s[i].v[k] = have ? (a.int_val ? 1 : 0) : -1;
        }
    }
}

// What was already open or wet is not a new event. The caller holds s_mu, or nothing else sees `p` yet.
void apply_seeds(AlarmPanel* p, const AlarmConfig& c, const Seed* s, uint8_t n) {
    for (uint8_t i = 0; i < n; i++) {
        const int slot = alarm_cfg_sensor(c, s[i].ieee);
        for (size_t k = 0; slot >= 0 && k < kLevelKeyCount; k++) {
            AlarmKey key;
            if (s[i].v[k] >= 0 && alarm_key_parse(kLevelKeys[k], &key))
                p->seed(static_cast<uint8_t>(slot), key, key_active(key, s[i].v[k]));
        }
    }
}

// ── In: every change to the panel ──

void wake() {
    if (s_task) xTaskNotifyGive(s_task);
}

// Run `f` on the panel under the lock, then let the task look at what it did. Time moves the panel first: an input
// meets the panel of now, not of the task's last look (an arm delay that ran out while an output blocked is armed).
template <typename F>
void act(F&& f) {
    const uint32_t now = mono_s();
    {
        Lock l;
        s_panel->tick(now);
        f();
    }
    wake();
}

// Arm one profile: by id, or with no id the intrusion profile of `role`. OPEN: the open contact sensors go
// to `open` (room for kAlarmMaxSensors) when given. `tell`: such a refusal is notified, for a keypad, a button,
// a rule, Lua, MQTT or Home Assistant: nobody there sees it, and leaving the house unguarded is what an alarm is for.
AlarmReply arm(const char* id, AlarmRole role, bool force, bool tell, uint64_t* open, uint8_t* n_open) {
    AlarmReply r = AlarmReply::NO_PROFILE;
    Refusal ref{};
    const uint32_t now = mono_s();
    act([&] {
        const int p = id ? alarm_cfg_profile(*s_cfg, id) : alarm_cfg_role(*s_cfg, role);
        if (p < 0) return;
        uint64_t slots = 0;
        const AlarmArmResult a = s_panel->arm(static_cast<uint8_t>(p), force, now, &slots);
        r = a == AlarmArmResult::OK ? AlarmReply::OK : a == AlarmArmResult::OPEN ? AlarmReply::OPEN : AlarmReply::BUSY;
        if (r != AlarmReply::OPEN) return;
        for (uint8_t slot = 0; slot < s_cfg->n_sensors; slot++) {
            if (!(slots >> slot & 1)) continue;
            if (!ref.n) ref.first = s_cfg->sensors[slot].ieee;
            if (open) open[ref.n] = s_cfg->sensors[slot].ieee;
            ref.n++;
        }
        std::memcpy(ref.id, s_cfg->profiles[p].id, sizeof(ref.id));
        std::memcpy(ref.name, s_cfg->profiles[p].name, sizeof(ref.name));
    });
    if (n_open) *n_open = ref.n;
    if (r == AlarmReply::OPEN && tell) {
        if (xQueueSend(s_refused, &ref, 0) == pdTRUE) wake();
        else ESP_LOGE(TAG, "arm %s refused, not reported: queue full", ref.id);
    }
    return r;
}

// ── Out: named events, notifications, outputs, the Alarm device ──

void rule_event(const char* name) {   // Event#<name>; the core's names start with alarm_
    Event ev{};
    ev.type = EventType::RULE_EVENT;
    auto* p = reinterpret_cast<RuleEventPayload*>(ev.data);
    std::snprintf(p->name, sizeof(p->name), "%s", name);
    event_bus_publish(ev);
}

// The cloud's `alert` (push + inbox) and Telegram. Both queue; neither waits on the network, and
// both hold a message back until it is up again, so each text says when it happened.
// `profile`: the profile's id, which keeps the alerts of two profiles apart when there is no ieee.
void notify(const char* kind, uint64_t ieee, const char* profile, const char* message) {
    char at[16] = "";
    if (const time_t t = time(nullptr); t > 1577836800) {
        struct tm lt;
        localtime_r(&t, &lt);
        std::strftime(at, sizeof(at), " at %H:%M", &lt);
    }
    char text[160], tg[176];   // a message is 127 bytes at most: nothing is cut
    std::snprintf(text, sizeof(text), "%s%s", message, at);
    std::snprintf(tg, sizeof(tg), "Alarm: %s", text);
    JsonDocument d;
    d["kind"]    = kind;
    d["state"]   = true;
    d["message"] = static_cast<const char*>(text);
    char id[19];
    if (ieee) {
        ieee_str(ieee, id);
        d["ieee"] = static_cast<const char*>(id);
    }
    if (profile) d["name"] = profile;
    char buf[448];
    const size_t n = serializeJson(d, buf, sizeof(buf));
    if (n && n < sizeof(buf)) remote_client_publish_event("alert", buf, n);
    tg_gw_send(tg, nullptr, nullptr);
}

void set(uint64_t ieee, const char* key, const DevCmdValue& v) {
    const DevCmdResult r = device_cmd_set_attr(ieee, 0, key, &v);
    if (r != DEVCMD_OK) ESP_LOGW(TAG, "output 0x%016" PRIX64 " %s: %s", ieee, key, device_cmd_result_str(r));
}

bool exposes_of(uint64_t ieee, JsonDocument& doc) {
    ZapDevice d{};
    if (!zigbee_pool_snapshot(ieee, &d)) return false;
    char* buf = static_cast<char*>(heap_caps_malloc(kExposesMax, MALLOC_CAP_SPIRAM));
    if (!buf) return false;
    const size_t n = zhac_adapter_build_exposes_json(ieee, d.model_id, d.manufacturer_name, buf, kExposesMax);
    const bool ok = n && deserializeJson(doc, buf, n) == DeserializationError::Ok;
    heap_caps_free(buf);
    return ok;
}

JsonObjectConst expose(const JsonDocument& doc, const char* name) {
    for (JsonObjectConst e : doc.as<JsonArrayConst>())
        if (std::strcmp(e["name"] | "", name) == 0) return e;
    return JsonObjectConst();
}

// A tone or volume label, as a number when the device takes one.
void set_label(uint64_t ieee, const char* key, JsonObjectConst e, const char* label) {
    if (std::strcmp(e["type"] | "", "numeric") == 0) set(ieee, key, device_cmd_int(std::strtol(label, nullptr, 10)));
    else set(ieee, key, device_cmd_str(label));
}

// A Tuya siren: its tone, volume and own duration, as it exposes them, before alarm=true.
void tuya_siren_setup(const AlarmOutput& s, uint16_t secs) {
    JsonDocument ex;
    if (!exposes_of(s.ieee, ex)) return;
    if (s.melody[0]) {
        for (const char* k : {"melody", "ringtone"}) {
            const JsonObjectConst e = expose(ex, k);
            if (e.isNull()) continue;
            set_label(s.ieee, k, e, s.melody);
            break;
        }
    }
    if (const JsonObjectConst e = expose(ex, "volume"); s.volume[0] && !e.isNull()) set_label(s.ieee, "volume", e, s.volume);
    if (const JsonObjectConst e = expose(ex, "duration"); !e.isNull()) {
        long v = std::strcmp(e["unit"] | "", "min") == 0 ? (secs + 59) / 60 : secs;
        if (e["value_max"].is<long>() && v > e["value_max"].as<long>()) v = e["value_max"].as<long>();
        if (e["value_min"].is<long>() && v < e["value_min"].as<long>()) v = e["value_min"].as<long>();
        set(s.ieee, "duration", device_cmd_int(v));
    }
}

// Start an output for `secs`, or stop it. Can wait seconds on a sleepy or far device.
void drive(const AlarmOutput& o, bool on, uint16_t secs) {
    if (!on && o.keep) return;   // left as it was set: the hub never sends such a device the opposite value
    switch (o.kind) {
    case AlarmOutKind::WARNING: {
        // embedded-zhc's warning encoder: "mode,level,strobe,duration" (z2m's warning object).
        char v[48];
        std::snprintf(v, sizeof(v), "%s,%s,%d,%u", o.wd_mode, o.wd_level, o.strobe ? 1 : 0, secs);
        set(o.ieee, "warning", on ? device_cmd_str(v) : device_cmd_bool(false));
        break;
    }
    case AlarmOutKind::ALARM:
        if (on) tuya_siren_setup(o, secs);
        set(o.ieee, "alarm", device_cmd_bool(on));
        break;
    case AlarmOutKind::STATE:
        set(o.ieee, "state", device_cmd_bool(on ? o.value : !o.value));
        break;
    }
}

void push_changed() {
    JsonDocument d(psram_json());   // kilobytes with eight profiles: not from internal RAM
    alarm_status_json(d.to<JsonObject>());
    ws_push("alarm.changed", d);
}

void touch_last_seen() {   // the device is always there
    const uint32_t t = wall_s();
    if (!t) return;
    zigbee_pool_lock();
    if (ZapDevice* d = pool_find_by_ieee(kAlarmIeee)) d->last_seen = t;
    zigbee_pool_unlock();
}

// ── The task: one look under the lock, then the work outside it ──

struct Act {
    bool        on;      // start, else stop
    uint8_t     index;   // of `out` in the config's outputs
    uint16_t    secs;    // start: the alarm time left
    AlarmOutput out;
};

// What one look found, copied under the lock. The task's only, in PSRAM (too big for its stack): no NVS or flash read
// writes into it, the records are only written from it.
struct Pass {
    uint8_t      n_ch, n_prof, n_ev, n_orphans, n_act;
    bool         running, resync, ids_changed;
    AlarmChange  ch[kAlarmMaxProfiles];
    bool         panic[kAlarmMaxProfiles];   // by change: the alarm came from a keypad, not from a sensor
    AlarmProfile prof[kAlarmMaxProfiles];    // the profiles and their states as of this look
    AlarmState   state[kAlarmMaxProfiles];
    bool         rests[kAlarmMaxProfiles];   // ... and where a restart comes back to (rests_armed)
    char         summary[kSummaryMax];
    uint64_t     summary_cause;              // the device behind the summary's pending or triggered, else 0
    char         events[kEventsMax][kAlarmEventMax];
    uint8_t      st[2];                      // the stage 1 state record as of this look
    Orphan       orphans[2 * kAlarmMaxOutputs];   // a save's, then the boot stop's
    Act          acts[kAlarmMaxOutputs];
};

// One more output command of the pass (the caller holds s_mu). They stay in the order of the config's outputs, so
// that a profile's valve comes before its sirens also when an earlier profile lists that siren too (the plan has
// each device where it is listed first).
void add_act(Pass& p, bool on, uint8_t k, uint16_t secs) {
    if (p.n_act >= kAlarmMaxOutputs) return;   // cannot be: each command is for another output
    uint8_t at = p.n_act++;
    for (; at > 0 && p.acts[at - 1].index > k; at--) p.acts[at] = p.acts[at - 1];
    p.acts[at] = {on, k, secs, s_cfg->outputs[k]};
}

// The boot stop into the pass: each output of it that the plan of now does not sound (the plan drives the others),
// once. The caller holds s_mu.
void boot_stop_to(Pass& p, const AlarmDrive* plan, uint8_t n) {
    for (uint8_t b = 0; b < s_n_boot_stop; b++) {
        bool sounds = false;
        for (uint8_t e = 0; e < n; e++) sounds = sounds || (plan[e].ieee == s_boot_stop[b].ieee && plan[e].output >= 0);
        if (!sounds) p.orphans[p.n_orphans++] = s_boot_stop[b];
    }
    heap_caps_free(s_boot_stop);
    s_boot_stop = nullptr;
    s_n_boot_stop = 0;
}

// `boot_stop`: a minute after a restart during an alarm, what sounded before it and does not sound now is stopped.
void look(uint32_t now, bool boot_stop, Pass& p) {
    static AlarmDrive plan[kAlarmMaxOutputs];
    const uint32_t wall = wall_s();
    Lock l;
    s_panel->tick(now);   // before the plan: it counts the time left from the deadlines
    p.n_ch = alarm_changes(*s_panel, s_seen, p.ch);
    alarm_seen(*s_panel, s_seen);
    p.running     = s_panel->running();
    p.resync      = s_resync;
    p.ids_changed = s_ids_changed;
    s_resync = s_ids_changed = false;
    p.n_prof = s_cfg->n_profiles;
    for (uint8_t i = 0; i < p.n_prof; i++) {
        p.prof[i]  = s_cfg->profiles[i];
        p.state[i] = s_panel->at(i).state;
        p.rests[i] = s_panel->rests_armed(i);
    }
    for (uint8_t i = 0; i < p.n_ch; i++) {
        const AlarmChange& c = p.ch[i];
        p.panic[i] = c.alarm && alarm_cfg_keypad(*s_cfg, c.cause) && alarm_cfg_sensor(*s_cfg, c.cause) < 0;
        if (c.from != c.to) s_since[c.profile] = wall;
    }
    alarm_summary_name(*s_cfg, *s_panel, p.summary, sizeof(p.summary));
    const int sp = s_panel->summary_profile();
    const AlarmState ss = sp >= 0 ? s_panel->at(static_cast<uint8_t>(sp)).state : AlarmState::DISARMED;
    p.summary_cause = ss == AlarmState::PENDING || ss == AlarmState::TRIGGERED ? s_panel->at(static_cast<uint8_t>(sp)).cause : 0;
    p.n_ev = alarm_events(*s_cfg, p.ch, p.n_ch, s_summary, p.summary, p.events, kEventsMax);
    alarm_legacy_record(*s_cfg, *s_panel, p.st);

    p.n_orphans = s_n_orphans;
    std::memcpy(p.orphans, s_orphans, s_n_orphans * sizeof(Orphan));
    s_n_orphans = 0;
    p.n_act = 0;
    // The profiles with a new alarm since the last look. Their outputs start again also when they follow that profile
    // already: a siren was told the time of the alarm before (silenced and set off again inside one busy pass, or a
    // panic's end in the arm delay that sets off a hazard still on, in one tick).
    uint8_t alarmed = 0;
    for (uint8_t i = 0; i < p.n_ch; i++) if (p.ch[i].alarm) alarmed |= static_cast<uint8_t>(1u << p.ch[i].profile);
    const uint8_t n = alarm_drive_plan(*s_cfg, *s_panel, now, plan);
    for (uint8_t e = 0; e < n; e++) {
        const int8_t was = s_follows[e];
        AlarmOutAct a = alarm_output_step(*s_cfg, was, plan[e].output);
        if (plan[e].output >= 0 && (alarmed >> s_cfg->outputs[plan[e].output].profile & 1)) a = AlarmOutAct::START;
        s_follows[e] = plan[e].output;
        if (a != AlarmOutAct::NONE) add_act(p, a == AlarmOutAct::START, static_cast<uint8_t>(a == AlarmOutAct::START ? plan[e].output : was), plan[e].secs);
    }
    // The boot stop, at its minute or at a disarm before it, also sets again what the restart's alarm left as set: the
    // states are saved before the outputs are set, so the restart may have come in between.
    if (boot_stop || s_boot_stop_now) boot_stop_to(p, plan, n);
    s_boot_stop_now = false;
    // The outputs left as set are set for every alarm, also one that is over before the task looks (silenced inside one
    // busy pass: a valve closes for every leak alarm, however short), as the config it went off under has them: a save
    // queues them for an alarm before it (alarm_config_set). It is the same command each time. The other outputs follow
    // the state of now.
    uint8_t kept = 0;   // the profiles whose kept outputs are set
    for (uint8_t i = 0; i < p.n_prof; i++) {
        if (s_panel->at(i).alarms != s_kept_alarms[i]) kept |= static_cast<uint8_t>(1u << i);
        s_kept_alarms[i] = s_panel->at(i).alarms;
    }
    for (uint8_t k = 0; kept && k < s_cfg->n_outputs; k++) {
        const AlarmOutput& o = s_cfg->outputs[k];
        if (!(kept >> o.profile & 1) || o.kind != AlarmOutKind::STATE || !o.keep) continue;
        bool set = false;   // in this pass, by the plan or by a save (to this value: an older config's other one goes first)
        for (uint8_t a = 0; a < p.n_act; a++) set = set || (p.acts[a].on && p.acts[a].out.ieee == o.ieee);
        for (uint8_t a = 0; a < p.n_orphans; a++)
            set = set || (p.orphans[a].keep && p.orphans[a].ieee == o.ieee && p.orphans[a].value == o.value);
        if (!set) add_act(p, true, k, 0);
    }
}

// One state record that differs from flash, unless the same bytes failed less than kSaveRetryS ago. The first failure
// of a run is logged, and the write that lands after it.
template <size_t N>
bool put_state(const char* key, const void* d, size_t n, SaveRetry<N>& r, const char* what, const char* lost) {
    const uint32_t now = mono_s();
    if (r.len == n && std::memcmp(r.tried, d, n) == 0 && now - r.at < kSaveRetryS) return false;
    if (nvs_put(key, d, n)) {
        if (r.failing) ESP_LOGI(TAG, "%s saved again", what);
        r.failing = false;
        r.len = 0;
        return true;
    }
    if (!r.failing)
        ESP_LOGW(TAG, "%s not saved: %s; tried again at the next change, or in %u min", what, lost, static_cast<unsigned>(kSaveRetryS / 60));
    r.failing = true;
    r.at = now;
    r.len = n;
    std::memcpy(r.tried, d, n);
    return false;
}

// The stage 1 record ("st", which follows the intrusion profiles) and the states of all profiles ("st2") as of the look,
// each when it differs from what flash holds. "st" goes first, and "st2" carries the "st" that flash holds then: a boot
// that finds another "st" beside it knows the stage 1 firmware wrote that one since (alarm_states_restore). Both
// describe the one instant of the look: what happens while they are written is the next look's. A write that fails is
// logged and the change still takes effect; a failed "st" leaves the two agreeing. A record that failed is tried again
// at its next change, or kSaveRetryS later (put_state). Flash that comes to agree without a write ends a run of failures.
void save_states(const Pass& p) {
    if (s_st_have && std::memcmp(p.st, s_st_saved, sizeof(p.st)) == 0) {
        s_st_retry.len = 0;
        s_st_retry.failing = false;
    } else if (put_state("st", p.st, sizeof(p.st), s_st_retry, "stage 1 state record", "a fall back to that firmware finds the one saved last")) {
        std::memcpy(s_st_saved, p.st, sizeof(p.st));
        s_st_have = true;
    }
    static char st2[kStatesMax];
    const size_t len = alarm_states_pack(p.prof, p.state, p.rests, p.n_prof, st2, sizeof(st2), s_st_have ? s_st_saved : nullptr);
    if (len == s_st2_len && std::memcmp(st2, s_st2_saved, len) == 0) {
        s_st2_retry.len = 0;
        s_st2_retry.failing = false;
        return;
    }
    if (len && put_state("st2", st2, len, s_st2_retry, "states", "a restart comes back to the ones saved last")) {
        std::memcpy(s_st2_saved, st2, len);
        s_st2_len = len;
    }
}

void tell_changes(const Pass& p) {
    char dev[32], msg[128];
    for (uint8_t i = 0; i < p.n_ch; i++) {
        const AlarmChange& c = p.ch[i];
        if (c.profile >= p.n_prof) continue;
        const AlarmProfile& pr = p.prof[c.profile];
        ESP_LOGI(TAG, "%s: %s -> %s%s", pr.id, alarm_state_name(c.from), alarm_state_name(c.to), c.alarm ? " (alarm)" : "");
        if (c.alarm) {   // also when it is over again by now
            name_of(c.cause, dev);
            alarm_text(p.panic[i] ? AlarmText::PANIC : AlarmText::TRIGGERED, pr.name, dev, 0, msg, sizeof(msg));
            notify("alarm", c.cause, nullptr, msg);
        }
        if (!pr.notify_arm) continue;
        if (c.to == AlarmState::ARMED && (c.from == AlarmState::DISARMED || c.from == AlarmState::ARMING)) {
            alarm_text(AlarmText::ARMED, pr.name, nullptr, 0, msg, sizeof(msg));
            notify("alarm_armed", 0, pr.id, msg);
        } else if (c.to == AlarmState::DISARMED && c.from != c.to && c.by_command) {   // not for a profile another one replaced
            alarm_text(AlarmText::DISARMED, pr.name, nullptr, 0, msg, sizeof(msg));
            notify("alarm_disarmed", 0, pr.id, msg);
        }
    }
}

void tell_refused(const Refusal& r) {
    char dev[32], msg[128], ev[kAlarmEventMax];
    name_of(r.first, dev);
    alarm_text(AlarmText::REFUSED, r.name, dev, r.n ? r.n - 1u : 0, msg, sizeof(msg));
    notify("alarm_arm_failed", r.first, r.id, msg);
    std::snprintf(ev, sizeof(ev), "alarm_arm_failed_%s", r.id);
    rule_event(ev);
    rule_event("alarm_arm_failed");
}

// The Alarm device's attributes, in the order handlers see them. `all`: every profile's, else those of the
// profiles whose state moved. `summary`: alarm_sensor and alarm_state too.
void publish_attrs(const Pass& p, bool all, bool summary) {
    static ZclAttribute a[kAlarmMaxProfiles + 2];
    std::memset(a, 0, sizeof(a));
    uint8_t n = 0;
    if (summary) {   // first: a handler of alarm_state can read it
        char name[32] = "";
        if (p.summary_cause) name_of(p.summary_cause, name);
        zcl_attr_set_str(&a[n++], "alarm_sensor", name);
    }
    for (uint8_t i = 0; all && i < p.n_prof; i++) zcl_attr_set_str(&a[n++], p.prof[i].id, alarm_state_name(p.state[i]));
    for (uint8_t i = 0; !all && i < p.n_ch; i++) {
        const AlarmChange& c = p.ch[i];
        if (c.from != c.to && c.profile < p.n_prof) zcl_attr_set_str(&a[n++], p.prof[c.profile].id, alarm_state_name(c.to));
    }
    if (summary) zcl_attr_set_str(&a[n++], "alarm_state", p.summary);
    if (n) device_shadow_process_ieee(kAlarmIeee, a, n);
}

// A save removed profiles: their attributes leave the device. Whatever it holds that is no profile of now
// goes (as Hub I/O drops the pins it lost), so one a hurried second save removed is not left behind either.
void drop_gone_attrs(const Pass& p) {
    constexpr uint8_t kMax = 32;
    auto* attrs = static_cast<ShadowAttr*>(heap_caps_malloc(kMax * sizeof(ShadowAttr), MALLOC_CAP_SPIRAM));
    if (!attrs) return;
    const uint8_t n = device_shadow_get_attrs(kAlarmIeee, attrs, kMax);
    for (uint8_t k = 0; k < n; k++) {
        bool live = attrs[k].key[0] == '_' ||   // the shadow's own (_last_seen)
                    std::strcmp(attrs[k].key, "alarm_state") == 0 || std::strcmp(attrs[k].key, "alarm_sensor") == 0;
        for (uint8_t i = 0; !live && i < p.n_prof; i++) live = std::strcmp(attrs[k].key, p.prof[i].id) == 0;
        if (!live) device_shadow_remove_attr(kAlarmIeee, attrs[k].key);
    }
    heap_caps_free(attrs);
}

// The work of one pass, in this order: what must not be lost first, what can wait on a device after it.
void run(const Pass& p, bool boot_due) {
    const bool moved = std::strcmp(p.summary, s_summary) != 0 || p.summary_cause != s_summary_cause;
    if (!boot_due) save_states(p);   // 1
    if (boot_due) {                  // 2: these only queue
        char msg[128];
        alarm_text(AlarmText::RESTARTED, nullptr, nullptr, 0, msg, sizeof(msg));
        notify("alarm", 0, nullptr, msg);
    }
    tell_changes(p);
    if (p.ids_changed) drop_gone_attrs(p);   // 3
    publish_attrs(p, p.ids_changed, moved);
    if (p.ids_changed) {   // hub page, cloud and HA re-read the device: its exposes changed (as Hub I/O does)
        Event ev{};
        ev.type = EventType::DEVICE_JOIN;
        std::memcpy(ev.data, &kAlarmIeee, sizeof(kAlarmIeee));
        event_bus_publish(ev);
    }
    for (uint8_t i = 0; i < p.n_ev; i++) rule_event(p.events[i]);   // 4
    for (uint8_t i = 0; i < p.n_orphans; i++) {                     // 5
        AlarmOutput o{};
        o.ieee  = p.orphans[i].ieee;
        o.kind  = p.orphans[i].kind;
        o.value = p.orphans[i].value;
        drive(o, p.orphans[i].keep, 0);
    }
    for (uint8_t i = 0; i < p.n_act; i++) drive(p.acts[i].out, p.acts[i].on, p.acts[i].secs);
    // Told and stopped: only now is the next restart not "during an alarm" again.
    if (boot_due) {
        save_states(p);
        if (!nvs_drop("bt")) ESP_LOGW(TAG, "boot stop record not erased: the next boot tells and stops it again");
    }
    push_changed();   // 6
    std::memcpy(s_summary, p.summary, sizeof(s_summary));
    s_summary_cause = p.summary_cause;
}

void alarm_task(void*) {
    void* mem = heap_caps_malloc(sizeof(Pass), MALLOC_CAP_SPIRAM);
    configASSERT(mem);
    Pass& p = *new (mem) Pass();   // for good: the task never ends
    uint32_t boot_alert_at = s_boot_alert ? mono_s() + kBootAlertDelayS : 0;
    uint32_t next_seen = 0;
    bool first = true, stack_logged = false;
    for (;;) {
        const uint32_t now = mono_s();
        const bool boot_due = boot_alert_at && now >= boot_alert_at;
        if (boot_due) boot_alert_at = 0;
        look(now, boot_due, p);
        if (first) {   // the device as it stands at boot: every attribute, no event
            first = false;
            publish_attrs(p, true, true);
        }
        const bool work = p.n_ch || p.resync || p.n_orphans || p.n_act || boot_due;
        if (work) {
            run(p, boot_due);
            if (!stack_logged) {   // for the hardware check: outputs and NVS are this task's deepest paths
                stack_logged = true;
                ESP_LOGI(TAG, "task: %u B of stack never used", static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
            }
        } else if (!boot_alert_at) {
            // Nothing new, and flash may still hold other states: a write that failed (tried again kSaveRetryS after
            // it), steps the look did not see as a change (a disarm and an arm), a boot that read other records. Not in
            // the minute after a restart during an alarm: its record waits for the boot stop.
            save_states(p);
        }
        Refusal r;
        while (xQueueReceive(s_refused, &r, 0) == pdTRUE) tell_refused(r);
        if (now >= next_seen) {
            next_seen = now + 60;
            touch_last_seen();
        }
        if (work) continue;   // again, until a pass finds nothing: the panel may have moved while this one worked
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(p.running || boot_alert_at ? 1000 : 30000));
    }
}

// ── In: sensors and keypads ──

// In the publisher's task: only the configured sensors' and keypads' values go on to on_attr.
bool attr_filter(const Event& e) {
    const auto& z = *reinterpret_cast<const ZclAttrEvent*>(e.data);
    if (z.ieee == kAlarmIeee) return false;
    AlarmKey key;
    const bool skey = alarm_key_parse(z.key, &key);
    Lock l;
    return (skey && alarm_cfg_sensor(*s_cfg, z.ieee) >= 0) || alarm_cfg_keypad(*s_cfg, z.ieee);
}

void copy_str(char* dst, size_t cap, const ZclAttrEvent& z) {   // str_val need not end in NUL
    const size_t n = strnlen(z.str_val, ATTR_STR_MAX);
    const size_t m = n < cap - 1 ? n : cap - 1;
    std::memcpy(dst, z.str_val, m);
    dst[m] = '\0';
}

// A keypad press arrives as action, action_code, ... in one report: act on its last value.
void on_keypad(const ZclAttrEvent& z) {
    PadInput in{};
    {
        Lock l;
        const AlarmKeypad* k = alarm_cfg_keypad(*s_cfg, z.ieee);
        if (!k) return;
        PadInput& p = s_pad[k - s_cfg->keypads];
        if (z.val_type == VAL_STR && std::strcmp(z.key, "action") == 0) {
            copy_str(p.action, sizeof(p.action), z);
            p.code[0] = '\0';
            p.have = true;
        } else if (z.val_type == VAL_STR && std::strcmp(z.key, "action_code") == 0) {
            copy_str(p.code, sizeof(p.code), z);
        }
        if (z.more || !p.have) return;
        in = p;
        p = PadInput{};
    }
    // IAS ACE arm modes as embedded-zhc names them (z2m's armMode): each arms the intrusion profile with that role.
    static constexpr struct { const char* action; AlarmRole role; } kArm[] = {
        {"arm_all_zones", AlarmRole::AWAY}, {"arm_day_zones", AlarmRole::HOME}, {"arm_night_zones", AlarmRole::NIGHT}};
    if (std::strcmp(in.action, "disarm") == 0) {
        bool ok = false;
        act([&] {
            ok = alarm_keypad_may_disarm(*s_cfg, z.ieee, in.code);
            if (ok) s_panel->disarm_all();
            s_boot_stop_now = s_boot_stop_now || ok;
        });
        if (!ok) ESP_LOGW(TAG, "keypad 0x%016" PRIX64 ": disarm refused, wrong or no code", z.ieee);
    } else if (std::strcmp(in.action, "emergency") == 0 || std::strcmp(in.action, "panic") == 0 ||
               std::strcmp(in.action, "fire") == 0) {
        bool ok = false;
        const uint32_t now = mono_s();
        act([&] { ok = s_panel->panic(z.ieee, now); });
        if (!ok) ESP_LOGW(TAG, "keypad 0x%016" PRIX64 ": %s ignored, there is no intrusion profile", z.ieee, in.action);
    }
    for (const auto& a : kArm) {
        if (std::strcmp(in.action, a.action) != 0) continue;
        const AlarmReply r = arm(nullptr, a.role, false, true, nullptr, nullptr);
        if (r == AlarmReply::NO_PROFILE)
            ESP_LOGW(TAG, "keypad 0x%016" PRIX64 ": %s ignored, no profile has that role", z.ieee, in.action);
        else if (r != AlarmReply::OK)
            ESP_LOGW(TAG, "keypad 0x%016" PRIX64 ": %s refused (%s)", z.ieee, in.action,
                     r == AlarmReply::OPEN ? "a door or window is open" : "an alarm is on");
    }
    std::memset(&in, 0, sizeof(in));
}

void on_attr(const Event& e) {
    const auto& z = *reinterpret_cast<const ZclAttrEvent*>(e.data);
    AlarmKey key;
    if (alarm_key_parse(z.key, &key) && (z.val_type == VAL_BOOL || z.val_type == VAL_INT)) {
        const bool active = key_active(key, z.int_val);
        const uint32_t now = mono_s();
        bool kept = false;
        act([&] {
            // The panel knows what counts: a contact that opens, a hazard that turns true, every motion report.
            const int slot = alarm_cfg_sensor(*s_cfg, z.ieee);
            if (slot >= 0) kept = s_panel->sensor(static_cast<uint8_t>(slot), key, active, now);
        });
        // A door's or a hazard's new value is what a restart seeds the panel with: to flash at the shadow's next
        // sweep, not up to 30 minutes later, or a power cut brings back an "open" or a "wet" that is over and the
        // next real one does not count. Motion keeps no value (a restart takes no "occupied" from flash).
        if (kept) device_shadow_persist_soon(z.ieee);
    }
    on_keypad(z);
}

// ── The Alarm device ──

// alarm_state takes disarmed (all) and armed_<id> of an intrusion profile; a profile's own attribute takes
// armed and disarmed. arming, pending and triggered are the panel's own.
DevCmdResult local_cmd(uint64_t ieee, const char* key, const DevCmdValue* v) {
    if (ieee != kAlarmIeee) return DEVCMD_NOT_FOUND;
    const bool summary = std::strcmp(key, "alarm_state") == 0;
    if (!summary) {
        Lock l;
        if (alarm_cfg_profile(*s_cfg, key) < 0) return DEVCMD_NO_CONVERTER;
    }
    if (v->kind != DEVCMD_STR || !v->s) return DEVCMD_BAD_VALUE;
    if (std::strcmp(v->s, "disarmed") == 0)
        return alarm_disarm(summary ? nullptr : key) == AlarmReply::OK ? DEVCMD_OK : DEVCMD_BAD_VALUE;
    const char* id = key;
    if (summary) {
        if (std::strncmp(v->s, "armed_", 6) != 0) return DEVCMD_BAD_VALUE;
        id = v->s + 6;
        Lock l;
        const int p = alarm_cfg_profile(*s_cfg, id);
        if (p < 0 || s_cfg->profiles[p].cls != AlarmClass::INTRUSION) return DEVCMD_BAD_VALUE;
    } else if (std::strcmp(v->s, "armed") != 0) {
        return DEVCMD_BAD_VALUE;
    }
    const AlarmReply r = arm(id, AlarmRole::NONE, false, true, nullptr, nullptr);
    if (r == AlarmReply::OK) return DEVCMD_OK;
    ESP_LOGW(TAG, "arm %s refused: %s", id,
             r == AlarmReply::OPEN ? "a door or window is open (the hub page can force it)"
             : r == AlarmReply::BUSY ? "an alarm is on, disarm first" : "no such profile");
    return DEVCMD_BAD_VALUE;
}

// One enum per profile, named by its id; then the stage 1 pair, alarm_state and alarm_sensor.
size_t local_exposes(uint64_t ieee, char* buf, size_t cap) {
    if (ieee != kAlarmIeee) return 0;
    JsonDocument d;
    JsonArray list = d.to<JsonArray>();
    auto add = [&](const char* name, const char* type, int access) {
        JsonObject e = list.add<JsonObject>();
        e["name"]   = name;   // a pointer: copied
        e["type"]   = type;
        e["access"] = access;
        return e;
    };
    {
        Lock l;
        for (uint8_t i = 0; i < s_cfg->n_profiles; i++) {
            JsonArray v = add(s_cfg->profiles[i].id, "enum", 7)["values"].to<JsonArray>();
            for (uint8_t s = 0; s <= static_cast<uint8_t>(AlarmState::TRIGGERED); s++) v.add(alarm_state_name(static_cast<AlarmState>(s)));
        }
        JsonArray v = add("alarm_state", "enum", 7)["values"].to<JsonArray>();
        v.add("disarmed");
        v.add("arming");
        for (uint8_t i = 0; i < s_cfg->n_profiles; i++) {
            if (s_cfg->profiles[i].cls != AlarmClass::INTRUSION) continue;
            char word[kSummaryMax];
            std::snprintf(word, sizeof(word), "armed_%s", s_cfg->profiles[i].id);
            v.add(static_cast<const char*>(word));   // a copy: word is reused
        }
        v.add("pending");
        v.add("triggered");
    }
    add("alarm_sensor", "text", 1);
    const size_t n = measureJson(d);
    return !d.overflowed() && n < cap ? serializeJson(d, buf, cap) : 0;
}

// Does the device report something a profile can watch? `exposes`: the JSON array the adapter builds.
bool has_sensor_key(const char* exposes, size_t len) {
    JsonDocument filter, doc;   // a device's exposes run to kilobytes: keep only the names
    filter[0]["name"] = true;
    if (deserializeJson(doc, exposes, len, DeserializationOption::Filter(filter)) != DeserializationError::Ok) return false;
    AlarmKey key;
    for (JsonObjectConst e : doc.as<JsonArrayConst>())
        if (alarm_key_parse(e["name"] | "", &key)) return true;
    return false;
}

}  // namespace

// ── Public ──

void alarm_early_init() {
    s_mu      = xSemaphoreCreateMutex();
    s_refused = xQueueCreate(16, sizeof(Refusal));
    s_cfg     = new (std::nothrow) AlarmConfig();
    s_panel   = s_cfg ? new (std::nothrow) AlarmPanel(s_cfg) : nullptr;
    configASSERT(s_mu && s_refused && s_panel);
    std::memset(s_follows, -1, sizeof(s_follows));

    // The config: "cfg2", else the stage 1 "cfg" (read only, for good), else the defaults. Nothing else reads
    // s_cfg yet, so each loader builds it in place (too big for app_main's stack) and from scratch. The record is read
    // into internal RAM, an NVS read target, until it is parsed (into PSRAM).
    char* buf = static_cast<char*>(heap_caps_malloc(kAlarmStoredMax, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!buf) ESP_LOGE(TAG, "out of memory: the config is not read, empty profiles this boot");
    bool have = false;
    if (const size_t len = buf ? nvs_get("cfg2", buf, kAlarmStoredMax) : 0) {
        have = alarm_cfg_unpack(buf, len, s_cfg, psram_json());
        if (!have) ESP_LOGE(TAG, "stored config unreadable: trying the stage 1 one");
    }
    if (!have) {
        if (const size_t len = buf ? nvs_get("cfg", buf, kAlarmStoredMax) : 0) {
            char err[96] = "";
            have = alarm_cfg_from_stage1(buf, len, s_cfg, err, sizeof(err), psram_json());
            if (!have) ESP_LOGE(TAG, "stage 1 config unreadable (%s): empty profiles this boot", err);
        }
    }
    if (!have) alarm_cfg_defaults(s_cfg);
    heap_caps_free(buf);

    // The states: "st2"; the intrusion profiles from the stage 1 "st" when that firmware wrote it since; "st" alone
    // when there is no "st2" (alarm_states_restore). Both are kept as read: the task writes one only when it differs.
    uint8_t in_alarm = 0, sounding = 0;
    s_st2_len = nvs_get("st2", s_st2_saved, sizeof(s_st2_saved));
    s_st_have = nvs_get("st", s_st_saved, sizeof(s_st_saved)) == sizeof(s_st_saved);
    switch (alarm_states_restore(s_st2_saved, s_st2_len, s_st_have ? s_st_saved : nullptr, *s_cfg, s_panel, &in_alarm, &sounding)) {
    case AlarmRestored::NEW:
        break;
    case AlarmRestored::MIXED:
        ESP_LOGW(TAG, "the stage 1 firmware armed or disarmed since: the intrusion profiles come from its record");
        break;
    case AlarmRestored::OLD:
        if (s_st2_len) ESP_LOGE(TAG, "stored states unreadable: the stage 1 record is used");
        break;
    case AlarmRestored::NOTHING:
        if (s_st2_len || s_st_have) ESP_LOGE(TAG, "stored states unreadable: every profile starts disarmed");
        break;
    }
    // The boot stop: what a boot less than a minute before left to stop in "bt", as it was on then, then the outputs of
    // the profiles that sounded, by this config. That record holds the job until it is done: a change in the minute
    // rewrites the state records, and a save in it the outputs. Internal RAM for it: an NVS read target.
    auto* bt = static_cast<uint8_t*>(heap_caps_malloc(kBootStopMax, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    size_t bt_len = bt ? nvs_get("bt", bt, kBootStopMax) : 0;
    if (bt_len && (bt[0] != 1 || bt_len != 2 + bt[1] * kBootItem)) {
        ESP_LOGE(TAG, "boot stop record unreadable: ignored");
        bt_len = 0;
    }
    s_boot_alert = in_alarm != 0 || bt_len != 0;
    if (s_boot_alert) s_boot_stop = static_cast<Orphan*>(heap_caps_malloc(kAlarmMaxOutputs * sizeof(Orphan), MALLOC_CAP_SPIRAM));
    if (s_boot_alert && !s_boot_stop) ESP_LOGE(TAG, "out of memory: what sounded is not stopped a minute after the restart");
    auto listed = [](uint64_t ieee) {   // each device once (and none when the list is full)
        for (uint8_t i = 0; i < s_n_boot_stop; i++) if (s_boot_stop[i].ieee == ieee) return true;
        return s_n_boot_stop == kAlarmMaxOutputs;
    };
    for (size_t at = 2; s_boot_stop && at < bt_len; at += kBootItem) {
        Orphan o{};
        std::memcpy(&o.ieee, bt + at, sizeof(o.ieee));
        o.kind  = static_cast<AlarmOutKind>(bt[at + 8]);
        o.value = bt[at + 9];
        o.keep  = bt[at + 10];
        if (!listed(o.ieee)) s_boot_stop[s_n_boot_stop++] = o;
    }
    for (uint8_t k = 0; s_boot_stop && k < s_cfg->n_outputs; k++) {
        const AlarmOutput& o = s_cfg->outputs[k];
        if ((sounding >> o.profile & 1) && !listed(o.ieee)) s_boot_stop[s_n_boot_stop++] = {o.ieee, o.kind, o.value, o.keep};
    }
    if (bt && s_boot_stop && (!bt_len || s_n_boot_stop != bt[1])) {   // to flash now, unless it holds this already
        bt[0] = 1;
        bt[1] = s_n_boot_stop;
        for (uint8_t i = 0; i < s_n_boot_stop; i++) {
            uint8_t* b = bt + 2 + i * kBootItem;
            std::memcpy(b, &s_boot_stop[i].ieee, sizeof(s_boot_stop[i].ieee));
            b[8]  = static_cast<uint8_t>(s_boot_stop[i].kind);
            b[9]  = s_boot_stop[i].value;
            b[10] = s_boot_stop[i].keep;
        }
        if (!nvs_put("bt", bt, 2 + s_n_boot_stop * kBootItem))
            ESP_LOGW(TAG, "boot stop record not saved: a restart in the next minute stops nothing");
    }
    heap_caps_free(bt);
    alarm_seen(*s_panel, s_seen);   // the restored states are where the task starts from, not changes
    alarm_summary_name(*s_cfg, *s_panel, s_summary, sizeof(s_summary));

    local_dev_register(kAlarmIeee, local_cmd, local_exposes);
    ESP_LOGI(TAG, "%s%s; %u profile(s), %u sensor(s), %u output(s), %u keypad(s)", s_summary,
             s_boot_alert ? " (restarted during an alarm)" : "", s_cfg->n_profiles, s_cfg->n_sensors, s_cfg->n_outputs,
             s_cfg->n_keypads);
}

void alarm_attach_device() {
    // PSRAM, not app_main's tight stack: copies of the pool's entry, and zap_store writes from its own copy.
    auto* m = static_cast<ZapDevice*>(heap_caps_malloc(2 * sizeof(ZapDevice), MALLOC_CAP_SPIRAM));
    if (!m) {
        ESP_LOGE(TAG, "out of memory: the Alarm device is not shown");
        return;
    }
    ZapDevice& snap = m[0];
    ZapDevice& before = m[1];
    std::memset(&snap, 0, sizeof(snap));
    bool save = false;
    zigbee_pool_lock();
    ZapDevice* d = pool_find_by_ieee(kAlarmIeee);
    bool added = false;
    if (!d && (d = pool_add()) != nullptr) {
        d->ieee_addr = kAlarmIeee;
        std::snprintf(d->friendly_name, sizeof(d->friendly_name), "Alarm");
        added = true;
    }
    if (d) {
        before = *d;
        d->protocol        = PROTO_LOCAL;
        d->nwk_addr        = 0xFFFE;   // "no network address"
        d->device_type     = 1;        // mains powered, always there
        d->power_source    = 1;
        d->flags          &= static_cast<uint8_t>(~(ZAP_DEV_REMOVED | ZAP_DEV_FIX_BINDS | ZAP_DEV_FIX_CIE));
        d->interview_state = static_cast<uint8_t>(InterviewState::IDENTITY_READY);
        d->support_state   = static_cast<uint8_t>(SupportState::MATCHED);
        d->configure_state = static_cast<uint8_t>(ConfigureState::DONE);
        std::snprintf(d->model_id, sizeof(d->model_id), "ZHAC-ALARM");
        std::snprintf(d->manufacturer_name, sizeof(d->manufacturer_name), "ZHAC");
        if (const uint32_t t = wall_s()) d->last_seen = t;
        save = added || std::memcmp(before.model_id, d->model_id, sizeof(d->model_id)) != 0 ||
               before.protocol != d->protocol || before.flags != d->flags ||
               before.support_state != d->support_state || before.interview_state != d->interview_state ||
               before.configure_state != d->configure_state;
        snap = *d;
    }
    zigbee_pool_unlock();
    if (!snap.ieee_addr) ESP_LOGE(TAG, "device pool full: the Alarm device is not shown");
    if (save && !zap_store_save_device(&snap)) ESP_LOGW(TAG, "Alarm device not saved");
    heap_caps_free(m);
}

void alarm_start() {
    // Subscribed first: a report that comes while the seeds are read is queued for the event pump, which app_main
    // starts after this returns, so it lands after the seeds instead of between the read and the subscription, lost.
    if (event_bus_subscribe(EventType::ZCL_ATTR, on_attr, attr_filter) == EVENT_SUB_INVALID)
        ESP_LOGE(TAG, "not subscribed: sensors and keypads are not watched");
    // PSRAM, not app_main's tight stack: the seeds are copied from the shadow's table in RAM, never read from flash.
    auto* seeds = static_cast<Seed*>(heap_caps_malloc(kAlarmMaxSensors * sizeof(Seed), MALLOC_CAP_SPIRAM));
    if (seeds) {
        uint8_t n;
        {
            Lock l;
            n = s_cfg->n_sensors;
            for (uint8_t i = 0; i < n; i++) seeds[i].ieee = s_cfg->sensors[i].ieee;
        }
        read_seeds(seeds, n);
        {
            Lock l;
            apply_seeds(s_panel, *s_cfg, seeds, n);
        }
        heap_caps_free(seeds);
    } else {
        ESP_LOGE(TAG, "out of memory: the sensors start with no last values");
    }
    // Internal RAM (xTaskCreate): it writes NVS, so its stack cannot sit in PSRAM. Its first pass
    // publishes the Alarm device's attributes.
    if (xTaskCreate(alarm_task, "alarm", 8192, nullptr, 3, &s_task) != pdPASS)
        ESP_LOGE(TAG, "task not started: no outputs, notifications or delays");
}

AlarmReply alarm_arm(const char* id, bool force, uint64_t* open, uint8_t* n_open) {
    return arm(id ? id : "", AlarmRole::NONE, force, false, open, n_open);
}

AlarmReply alarm_disarm(const char* id) {
    AlarmReply r = AlarmReply::OK;
    act([&] {
        const int p = id && id[0] ? alarm_cfg_profile(*s_cfg, id) : -1;
        if (!id || !id[0]) s_panel->disarm_all();
        else if (p < 0) r = AlarmReply::NO_PROFILE;
        else s_panel->disarm(static_cast<uint8_t>(p));
        s_boot_stop_now = s_boot_stop_now || r == AlarmReply::OK;
    });
    return r;
}

AlarmReply alarm_silence(const char* id) {
    AlarmReply r = AlarmReply::NO_PROFILE;
    act([&] {
        const int p = alarm_cfg_profile(*s_cfg, id);
        if (p < 0) return;
        s_panel->silence(static_cast<uint8_t>(p));
        r = AlarmReply::OK;
    });
    return r;
}

bool alarm_config_set(const char* json, size_t len, char* err, size_t cap) {
    auto* next  = new (std::nothrow) AlarmConfig();
    auto* panel = next ? new (std::nothrow) AlarmPanel(next) : nullptr;
    auto* dev   = new (std::nothrow) ZapDevice();
    auto* seeds = static_cast<Seed*>(heap_caps_malloc(kAlarmMaxSensors * sizeof(Seed), MALLOC_CAP_SPIRAM));
    char* buf   = static_cast<char*>(heap_caps_malloc(kAlarmStoredMax, MALLOC_CAP_SPIRAM));
    bool ok = panel && dev && seeds && buf;
    if (!ok) std::snprintf(err, cap, "out of memory");
    if (ok) {
        Lock l;   // the stored codes are read while parsing
        // First: no edit meets a running timer or a sounding output.
        ok = !s_panel->running();
        if (!ok) std::snprintf(err, cap, "an alarm is running: disarm or wait, then save");
        else ok = alarm_cfg_from_json(json, len, s_cfg, next, err, cap, psram_json());
        // A profile that keeps its id keeps its class: the summary, the outputs' order and the user's rules hang on it.
        for (uint8_t i = 0; ok && i < next->n_profiles; i++) {
            const int was = alarm_cfg_profile(*s_cfg, next->profiles[i].id);
            if (was < 0 || s_cfg->profiles[was].cls == next->profiles[i].cls) continue;
            std::snprintf(err, cap, "profiles[%u]: the class of a saved profile cannot change", i);
            ok = false;
        }
    }
    // Every entry is a device on this hub; a sensor has something to watch, an output something to set.
    char where[48];
    auto known = [&](uint64_t ieee) {
        if (ieee != kAlarmIeee && zigbee_pool_snapshot(ieee, dev) && !zap_dev_is_removed(dev)) return true;
        char id[19];
        ieee_str(ieee, id);
        std::snprintf(err, cap, "%s: %s is not a device on this hub", where, id);
        return false;
    };
    auto exposes = [&] {   // of `dev`, into buf
        const size_t n = zhac_adapter_build_exposes_json(dev->ieee_addr, dev->model_id, dev->manufacturer_name, buf, kAlarmStoredMax);
        return n < kAlarmStoredMax ? n : 0;
    };
    for (uint8_t k = 0; ok && k < next->n_sensors; k++) {
        // Where the page has it: its first profile, and the row it listed it at there.
        const uint8_t in = next->sensors[k].profiles;
        const unsigned i = in ? static_cast<unsigned>(__builtin_ctz(in)) : 0;
        std::snprintf(where, sizeof(where), "profiles[%u].sensors[%u]", i, static_cast<unsigned>(next->sensors[k].row));
        ok = known(next->sensors[k].ieee);
        if (ok && !has_sensor_key(buf, exposes())) {
            std::snprintf(err, cap, "%s: %s has no contact, occupancy, presence, vibration, water_leak, smoke, gas or carbon_monoxide",
                          where, dev->friendly_name);
            ok = false;
        }
    }
    unsigned place = 0;   // of an output in its profile: they are grouped by profile, each group in the listed order
    for (uint8_t k = 0; ok && k < next->n_outputs; k++) {
        const AlarmOutput& o = next->outputs[k];
        place = k && next->outputs[k - 1].profile == o.profile ? place + 1 : 0;
        std::snprintf(where, sizeof(where), "profiles[%u].outputs[%u]", o.profile, place);
        ok = known(o.ieee);
        if (!ok) break;
        // A smoke sensor's `alarm` is only a reading: the device needs something settable for the kind.
        if (const char* why = alarm_output_misfit(o.kind, buf, exposes())) {
            std::snprintf(err, cap, "%s: %s %s", where, dev->friendly_name, why);
            ok = false;
        }
    }
    for (uint8_t i = 0; ok && i < next->n_keypads; i++) {
        std::snprintf(where, sizeof(where), "keypads[%u]", i);
        ok = known(next->keypads[i].ieee);
    }
    if (ok) {   // "cfg2" only: the stage 1 "cfg" stays as it is, for a hub that falls back to that firmware
        const size_t n = alarm_cfg_pack(*next, buf, kAlarmStoredMax);
        ok = n && nvs_put("cfg2", buf, n);
        if (!ok) std::snprintf(err, cap, "could not save the config");
        else ESP_LOGI(TAG, "config saved: %u profile(s), %u sensor(s), %u output(s), %u keypad(s), %u code(s)", next->n_profiles,
                      next->n_sensors, next->n_outputs, next->n_keypads, next->n_codes);
    }
    if (ok) {
        const uint8_t n_seeds = next->n_sensors;
        for (uint8_t i = 0; i < n_seeds; i++) seeds[i].ieee = next->sensors[i].ieee;
        read_seeds(seeds, n_seeds);
        // The states go to flash with the config, beside the "st" that flash holds (ruling 17): a power cut before the
        // task's next pass brings every profile back as the save left it, a new one too.
        uint8_t st[2];
        const bool have_st = nvs_get("st", st, sizeof(st)) == sizeof(st);
        size_t states = 0;
        const uint32_t now = mono_s();
        {
            Lock l;
            // The new panel: every sensor's last value from the shadow, then what the old panel knows better
            // (states and counters by profile id, last values and bypass by ieee).
            apply_seeds(panel, *next, seeds, next->n_sensors);
            panel->adopt(*s_panel, *s_cfg);
            AlarmSeen seen[kAlarmMaxProfiles] = {};
            uint32_t since[kAlarmMaxProfiles] = {}, kept_alarms[kAlarmMaxProfiles] = {};
            bool ids = next->n_profiles != s_cfg->n_profiles;
            for (uint8_t i = 0; i < next->n_profiles; i++) {
                const int j = alarm_cfg_profile(*s_cfg, next->profiles[i].id);
                ids = ids || j != i;   // another list or another order (a class never changes: refused above)
                if (j < 0) {
                    // A new profile that is not intrusion guards at once, like any arm without force: the arm
                    // delay runs, and an open contact leaves it disarmed.
                    if (next->profiles[i].cls != AlarmClass::INTRUSION) panel->arm(i, false, now, nullptr);
                    continue;
                }
                seen[i]  = s_seen[j];
                since[i] = s_since[j];
                kept_alarms[i] = s_panel->at(j).alarms;   // its alarms of before are queued below
            }
            // An output that still follows an alarm (a disarm the task has not acted on yet, or an alarm that began
            // while the devices were checked) is stopped by its old entry; the new plan then starts what still sounds.
            for (int8_t& k : s_follows) {
                if (k >= 0 && !s_cfg->outputs[k].keep && s_n_orphans < kAlarmMaxOutputs)
                    s_orphans[s_n_orphans++] = {s_cfg->outputs[k].ieee, s_cfg->outputs[k].kind, s_cfg->outputs[k].value, false};
                k = -1;
            }
            // An alarm the task has not acted on is over (nothing runs at a save), but what it leaves as set is set: as
            // the config it went off under has it, whatever the new one says (W12).
            for (uint8_t k = 0; k < s_cfg->n_outputs; k++) {
                const AlarmOutput& o = s_cfg->outputs[k];
                if (o.kind == AlarmOutKind::STATE && o.keep && s_panel->at(o.profile).alarms != s_kept_alarms[o.profile] &&
                    s_n_orphans < kAlarmMaxOutputs)
                    s_orphans[s_n_orphans++] = {o.ieee, o.kind, o.value, true};
            }
            for (uint8_t i = 0; i < kAlarmMaxProfiles; i++) {
                s_seen[i]  = seen[i];
                s_since[i] = since[i];
                s_kept_alarms[i] = kept_alarms[i];
            }
            std::memset(s_pad, 0, sizeof(s_pad));
            s_resync        = true;
            s_ids_changed   = s_ids_changed || ids;
            std::swap(s_cfg, next);   // `next` and `panel` now hold the old ones, freed below
            std::swap(s_panel, panel);
            states = alarm_states_pack(*s_cfg, *s_panel, buf, kStatesMax, have_st ? st : nullptr);
        }
        if (states) nvs_put("st2", buf, states);   // if it fails, the task's next pass writes them
        // A sensor the save adds starts from its last values in the shadow: they go to flash at the shadow's next
        // sweep, as a sensor's change does (on_attr), or a power cut brings back older ones.
        for (uint8_t i = 0; i < n_seeds; i++)
            if (alarm_cfg_sensor(*next, seeds[i].ieee) < 0) device_shadow_persist_soon(seeds[i].ieee);
        wake();   // the task saves the states, publishes the attributes and announces the device
    }
    heap_caps_free(buf);
    heap_caps_free(seeds);
    delete dev;
    delete panel;
    delete next;
    return ok;
}

void alarm_status_json(JsonObject o) {
    uint64_t cause[kAlarmMaxProfiles] = {}, top = 0;
    char id[19];
    JsonArray list;
    const uint32_t now = mono_s();
    {
        Lock l;
        char summary[kSummaryMax];
        alarm_summary_name(*s_cfg, *s_panel, summary, sizeof(summary));
        o["state"] = static_cast<const char*>(summary);
        list = o["profiles"].to<JsonArray>();
        for (uint8_t i = 0; i < s_cfg->n_profiles; i++) {
            const AlarmProfile& p = s_cfg->profiles[i];
            const AlarmProfileStatus& st = s_panel->at(i);
            JsonObject j = list.add<JsonObject>();
            j["id"]    = static_cast<const char*>(p.id);   // a copy: the array would be kept by pointer
            j["name"]  = static_cast<const char*>(p.name);
            j["class"] = alarm_class_name(p.cls);
            j["state"] = alarm_state_name(st.state);
            j["since"] = s_since[i];
            if (st.state != AlarmState::DISARMED && st.state != AlarmState::ARMED) j["left"] = st.deadline > now ? st.deadline - now : 0;
            if ((st.state == AlarmState::PENDING || st.state == AlarmState::TRIGGERED) && st.cause) {
                cause[i] = st.cause;
                ieee_str(st.cause, id);
                j["cause"] = static_cast<const char*>(id);
            }
            JsonArray b = j["bypassed"].to<JsonArray>();
            for (uint8_t slot = 0; slot < s_cfg->n_sensors; slot++) {
                if (!(st.bypass >> slot & 1)) continue;
                ieee_str(s_cfg->sensors[slot].ieee, id);
                b.add(static_cast<const char*>(id));
            }
        }
        if (const int sp = s_panel->summary_profile(); sp >= 0) top = cause[sp];
    }
    // The names come from the device pool: never under s_mu.
    char name[32];
    if (top) {
        name_of(top, name);
        o["sensor"] = static_cast<const char*>(name);
    }
    for (uint8_t i = 0; i < kAlarmMaxProfiles; i++) {
        if (!cause[i]) continue;
        name_of(cause[i], name);
        list[i]["sensor"] = static_cast<const char*>(name);
    }
}

void alarm_config_json(JsonObject o) {
    Lock l;
    alarm_cfg_write(*s_cfg, false, o);
}
