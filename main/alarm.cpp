// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Alarm panel glue. Every change to the panel happens under s_mu in act(), which
// hands the state change to the "alarm" task; that task does everything that can
// wait on the radio or the network: sirens, notifications, the Alarm device, NVS.
#include "alarm.h"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <new>

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
constexpr size_t   kCfgJsonMax = 8192;
constexpr size_t   kExposesMax = 2048;
constexpr uint32_t kBootAlertDelayS = 60;   // the network and the cloud link come up first

SemaphoreHandle_t s_mu = nullptr;   // guards everything below up to s_since
struct Lock {
    Lock() { xSemaphoreTake(s_mu, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(s_mu); }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
};

AlarmConfig* s_cfg = nullptr;    // heap: ~2 KB
AlarmCore*   s_core = nullptr;
int8_t       s_contact[kAlarmMaxSensors];   // each sensor's last contact: -1 unknown, 0 open, 1 closed
struct PadInput {
    char action[24];
    char code[16];   // a secret: cleared once used
    bool have;
};
PadInput     s_pad[kAlarmMaxKeypads];
uint32_t     s_since = 0;        // wall clock of the last change; 0 while the clock is unset

QueueHandle_t s_q = nullptr;     // Change, to the task
TaskHandle_t  s_task = nullptr;
bool          s_boot_alert = false;   // restarted during pending or triggered
uint8_t       s_saved[2] = {0xFF, 0xFF};   // the state record last written

struct Change {
    AlarmState from, to, resting;
    AlarmMode  mode;
    uint64_t   cause;
    bool       timed;   // an ended delay or siren time, not a person
};

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

size_t nvs_get(const char* key, void* data, size_t cap) {
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READONLY, &h) != ESP_OK) return 0;
    size_t len = cap;
    const esp_err_t e = nvs_get_blob(h, key, data, &len);
    nvs_close(h);
    return e == ESP_OK ? len : 0;
}

// -1 unknown, 0 open, 1 closed: the last contact report the shadow holds.
int8_t contact_now(uint64_t ieee) {
    ShadowAttr a{};
    if (!device_shadow_get_attr(ieee, "contact", &a) || (a.val_type != VAL_BOOL && a.val_type != VAL_INT)) return -1;
    return a.int_val ? 1 : 0;
}

// Run `f` on the panel under the lock; a state change goes to the task.
template <typename F>
void act(bool timed, F&& f) {
    Change c{};
    {
        Lock l;
        c.from = s_core->state();
        f();
        c.to      = s_core->state();
        c.resting = s_core->resting();
        c.mode    = s_core->mode();
        c.cause   = s_core->cause();
        if (c.from != c.to) s_since = wall_s();
    }
    if (c.from == c.to) return;
    c.timed = timed;
    if (xQueueSend(s_q, &c, 0) != pdTRUE)
        ESP_LOGE(TAG, "%s -> %s: not acted on, queue full", alarm_state_name(c.from), alarm_state_name(c.to));
}

// ── Out: the Alarm device, named events, notifications, sirens ──

void publish_state(AlarmState s, uint64_t cause) {
    char name[32] = "";
    if (cause && (s == AlarmState::PENDING || s == AlarmState::TRIGGERED)) name_of(cause, name);
    ZclAttribute a[2]{};
    zcl_attr_set_str(&a[0], "alarm_sensor", name);   // first: a handler of alarm_state can read it
    zcl_attr_set_str(&a[1], "alarm_state", alarm_state_name(s));
    device_shadow_process_ieee(kAlarmIeee, a, 2);
}

void rule_event(AlarmState s) {
    Event ev{};
    ev.type = EventType::RULE_EVENT;
    auto* p = reinterpret_cast<RuleEventPayload*>(ev.data);
    std::snprintf(p->name, sizeof(p->name), "alarm_%s", alarm_state_name(s));
    event_bus_publish(ev);
}

// The cloud's `alert` (push + inbox) and Telegram. Both queue; neither waits on the network.
void notify(const char* kind, uint64_t ieee, const char* message, const char* tg_text) {
    JsonDocument d;
    d["kind"]    = kind;
    d["state"]   = true;
    d["message"] = message;
    char id[19];
    if (ieee) {
        ieee_str(ieee, id);
        d["ieee"] = static_cast<const char*>(id);
    }
    char buf[320];
    const size_t n = serializeJson(d, buf, sizeof(buf));
    if (n && n < sizeof(buf)) remote_client_publish_event("alert", buf, n);
    tg_gw_send(tg_text, nullptr, nullptr);
}

void notify_change(const Change& c) {
    const char* mode = alarm_mode_name(c.mode);
    char msg[96], tg[112];
    if (c.to == AlarmState::TRIGGERED) {
        bool panic;
        {
            Lock l;
            panic = alarm_cfg_keypad(*s_cfg, c.cause) && !alarm_cfg_sensor(*s_cfg, c.cause);
        }
        char name[32];
        name_of(c.cause, name);
        if (panic) std::snprintf(msg, sizeof(msg), "Panic button on %s", name);
        else       std::snprintf(msg, sizeof(msg), "%s (armed %s)", name, mode);
        std::snprintf(tg, sizeof(tg), "Alarm: %s", msg);
        notify("alarm", c.cause, msg, tg);
        return;
    }
    bool on;
    {
        Lock l;
        on = s_cfg->notify_arm;
    }
    if (!on) return;
    const bool armed = c.to == AlarmState::ARMED_AWAY || c.to == AlarmState::ARMED_HOME || c.to == AlarmState::ARMED_NIGHT;
    if (armed && c.from != AlarmState::PENDING && c.from != AlarmState::TRIGGERED) {
        std::snprintf(msg, sizeof(msg), "Armed (%s)", mode);
        std::snprintf(tg, sizeof(tg), "Alarm armed (%s)", mode);
        notify("alarm_armed", 0, msg, tg);
    } else if (c.to == AlarmState::DISARMED && !c.timed) {
        notify("alarm_disarmed", 0, "Disarmed", "Alarm disarmed");
    }
}

void set(uint64_t ieee, const char* key, const DevCmdValue& v) {
    const DevCmdResult r = device_cmd_set_attr(ieee, 0, key, &v);
    if (r != DEVCMD_OK) ESP_LOGW(TAG, "siren 0x%016" PRIX64 " %s: %s", ieee, key, device_cmd_result_str(r));
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
void tuya_siren_setup(const AlarmSiren& s, uint16_t secs) {
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

void siren(const AlarmSiren& s, bool on, uint16_t secs) {
    switch (s.kind) {
    case AlarmSirenKind::WARNING: {
        // embedded-zhc's warning encoder: "mode,level,strobe,duration" (z2m's warning object).
        char v[48];
        std::snprintf(v, sizeof(v), "%s,%s,%d,%u", s.wd_mode, s.wd_level, s.strobe ? 1 : 0, secs);
        set(s.ieee, "warning", on ? device_cmd_str(v) : device_cmd_bool(false));
        break;
    }
    case AlarmSirenKind::ALARM:
        if (on) tuya_siren_setup(s, secs);
        set(s.ieee, "alarm", device_cmd_bool(on));
        break;
    case AlarmSirenKind::SWITCH:
        set(s.ieee, "state", device_cmd_bool(on));
        break;
    }
}

void sirens(bool on) {
    AlarmSiren list[kAlarmMaxSirens];
    uint8_t n;
    uint16_t secs;
    {
        Lock l;
        n = s_cfg->n_sirens;
        std::memcpy(list, s_cfg->sirens, n * sizeof(AlarmSiren));
        secs = s_cfg->siren_time;
    }
    for (uint8_t i = 0; i < n; i++) siren(list[i], on, secs);
}

void push_changed() {
    JsonDocument d;
    alarm_status_json(d.to<JsonObject>());
    ws_push("alarm.changed", d);
}

void save_state(AlarmState state, AlarmState resting) {
    const uint8_t rec[2] = {static_cast<uint8_t>(state), static_cast<uint8_t>(resting)};
    if (std::memcmp(rec, s_saved, sizeof(rec)) == 0) return;
    if (nvs_put("st", rec, sizeof(rec))) std::memcpy(s_saved, rec, sizeof(rec));
    else ESP_LOGW(TAG, "state not saved: a restart comes back to the previous one");
}

void run_change(const Change& c) {
    ESP_LOGI(TAG, "%s -> %s", alarm_state_name(c.from), alarm_state_name(c.to));
    save_state(c.to, c.resting);
    notify_change(c);   // first: it only queues
    publish_state(c.to, c.cause);
    rule_event(c.to);
    if (c.to == AlarmState::TRIGGERED) sirens(true);
    else if (c.from == AlarmState::TRIGGERED) sirens(false);
    push_changed();
}

void touch_last_seen() {   // the device is always there
    const uint32_t t = wall_s();
    if (!t) return;
    zigbee_pool_lock();
    if (ZapDevice* d = pool_find_by_ieee(kAlarmIeee)) d->last_seen = t;
    zigbee_pool_unlock();
}

void alarm_task(void*) {
    uint32_t boot_alert_at = s_boot_alert ? mono_s() + kBootAlertDelayS : 0;
    uint32_t next_seen = 0;
    bool stack_logged = false;
    for (;;) {
        bool timed;
        {
            Lock l;
            const AlarmState s = s_core->state();
            timed = s == AlarmState::ARMING || s == AlarmState::PENDING || s == AlarmState::TRIGGERED;
        }
        Change c;
        if (xQueueReceive(s_q, &c, pdMS_TO_TICKS(timed || boot_alert_at ? 1000 : 30000)) == pdTRUE) {
            run_change(c);
            if (!stack_logged) {   // for the hardware check: sirens and NVS are this task's deepest paths
                stack_logged = true;
                ESP_LOGI(TAG, "task: %u B of stack never used", static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
            }
        }
        const uint32_t now = mono_s();
        act(true, [&] { s_core->tick(now); });
        if (boot_alert_at && now >= boot_alert_at) {
            boot_alert_at = 0;
            notify("alarm", 0, "The hub restarted during an alarm", "Alarm: the hub restarted during an alarm");
        }
        if (now >= next_seen) {
            next_seen = now + 60;
            touch_last_seen();
        }
    }
}

// ── In: sensors and keypads ──

bool momentary_key(const char* k) {
    return std::strcmp(k, "occupancy") == 0 || std::strcmp(k, "presence") == 0 || std::strcmp(k, "vibration") == 0;
}
bool sensor_key(const char* k) { return std::strcmp(k, "contact") == 0 || momentary_key(k); }

// In the publisher's task: only the configured sensors' and keypads' values reach the queue,
// so a chatty device elsewhere cannot push them out of it.
bool attr_filter(const Event& e) {
    const auto& z = *reinterpret_cast<const ZclAttrEvent*>(e.data);
    if (z.ieee == kAlarmIeee) return false;
    const bool skey = sensor_key(z.key);
    Lock l;
    return (skey && alarm_cfg_sensor(*s_cfg, z.ieee)) || alarm_cfg_keypad(*s_cfg, z.ieee);
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
    // IAS ACE arm modes as embedded-zhc names them (z2m's armMode).
    static constexpr struct { const char* action; AlarmMode mode; } kArm[] = {
        {"arm_all_zones", AlarmMode::AWAY}, {"arm_day_zones", AlarmMode::HOME}, {"arm_night_zones", AlarmMode::NIGHT}};
    if (std::strcmp(in.action, "disarm") == 0) {
        bool ok;
        {
            Lock l;
            ok = alarm_keypad_may_disarm(*s_cfg, z.ieee, in.code);
        }
        if (ok) alarm_disarm();
        else ESP_LOGW(TAG, "keypad 0x%016" PRIX64 ": disarm refused, wrong or no code", z.ieee);
    } else if (std::strcmp(in.action, "emergency") == 0 || std::strcmp(in.action, "panic") == 0 ||
               std::strcmp(in.action, "fire") == 0) {
        const uint32_t now = mono_s();
        act(false, [&] { s_core->panic(z.ieee, now); });
    }
    for (const auto& a : kArm) {
        if (std::strcmp(in.action, a.action) != 0) continue;
        uint64_t open[kAlarmMaxSensors];
        uint8_t n = 0;
        const AlarmArmResult r = alarm_arm(a.mode, false, open, &n);
        if (r != AlarmArmResult::OK)
            ESP_LOGW(TAG, "keypad 0x%016" PRIX64 ": arm %s refused (%s)", z.ieee, alarm_mode_name(a.mode),
                     r == AlarmArmResult::OPEN ? "a door or window is open" : "the alarm is on");
    }
    std::memset(&in, 0, sizeof(in));
}

void on_attr(const Event& e) {
    const auto& z = *reinterpret_cast<const ZclAttrEvent*>(e.data);
    if (sensor_key(z.key) && (z.val_type == VAL_BOOL || z.val_type == VAL_INT)) {
        const uint32_t now = mono_s();
        act(false, [&] {
            const AlarmSensor* s = alarm_cfg_sensor(*s_cfg, z.ieee);
            if (!s) return;
            bool active;
            if (std::strcmp(z.key, "contact") == 0) {
                // Opened: a repeated "open" report is not a new opening.
                int8_t& last = s_contact[s - s_cfg->sensors];
                active = z.int_val == 0 && last != 0;
                last = z.int_val ? 1 : 0;
            } else {
                active = z.int_val != 0;   // motion, presence, vibration: every report of it
            }
            if (active) s_core->sensor_active(z.ieee, now);
        });
    }
    on_keypad(z);
}

// ── The Alarm device ──

DevCmdResult local_cmd(uint64_t ieee, const char* key, const DevCmdValue* v) {
    if (ieee != kAlarmIeee) return DEVCMD_NOT_FOUND;
    if (std::strcmp(key, "alarm_state") != 0) return DEVCMD_NO_CONVERTER;
    AlarmState s;
    if (v->kind != DEVCMD_STR || !v->s || !alarm_state_parse(v->s, &s)) return DEVCMD_BAD_VALUE;
    AlarmMode m;
    switch (s) {
    case AlarmState::DISARMED:    alarm_disarm(); return DEVCMD_OK;
    case AlarmState::ARMED_AWAY:  m = AlarmMode::AWAY; break;
    case AlarmState::ARMED_HOME:  m = AlarmMode::HOME; break;
    case AlarmState::ARMED_NIGHT: m = AlarmMode::NIGHT; break;
    default:                      return DEVCMD_BAD_VALUE;   // arming, pending, triggered: the panel's own
    }
    uint64_t open[kAlarmMaxSensors];
    uint8_t n = 0;
    const AlarmArmResult r = alarm_arm(m, false, open, &n);
    if (r == AlarmArmResult::OK) return DEVCMD_OK;
    ESP_LOGW(TAG, "arm %s refused: %s", alarm_mode_name(m),
             r == AlarmArmResult::OPEN ? "a door or window is open (the hub page can force it)" : "the alarm is on, disarm first");
    return DEVCMD_BAD_VALUE;
}

size_t local_exposes(uint64_t ieee, char* buf, size_t cap) {
    if (ieee != kAlarmIeee) return 0;
    const int n = std::snprintf(buf, cap,
        "[{\"name\":\"alarm_state\",\"type\":\"enum\",\"access\":7,\"values\":[\"disarmed\",\"arming\","
        "\"armed_away\",\"armed_home\",\"armed_night\",\"pending\",\"triggered\"]},"
        "{\"name\":\"alarm_sensor\",\"type\":\"text\",\"access\":1}]");
    return n > 0 && static_cast<size_t>(n) < cap ? static_cast<size_t>(n) : 0;
}

}  // namespace

// ── Public ──

void alarm_early_init() {
    s_mu   = xSemaphoreCreateMutex();
    s_q    = xQueueCreate(8, sizeof(Change));
    s_cfg  = new (std::nothrow) AlarmConfig();
    s_core = s_cfg ? new (std::nothrow) AlarmCore(s_cfg) : nullptr;
    configASSERT(s_mu && s_q && s_core);
    std::memset(s_contact, -1, sizeof(s_contact));

    char* json = static_cast<char*>(heap_caps_malloc(kCfgJsonMax, MALLOC_CAP_SPIRAM));
    const size_t len = json ? nvs_get("cfg", json, kCfgJsonMax) : 0;
    char err[96] = "";
    // Nothing else reads s_cfg yet: parse straight into it (too big for app_main's stack).
    if (len && !alarm_cfg_from_json(json, len, nullptr, s_cfg, err, sizeof(err))) {
        ESP_LOGE(TAG, "stored config unreadable (%s): no sensors this boot", err);
        new (s_cfg) AlarmConfig();
    }
    heap_caps_free(json);

    uint8_t st[2];
    if (nvs_get("st", st, sizeof(st)) == sizeof(st) && st[1] <= static_cast<uint8_t>(AlarmState::TRIGGERED)) {
        s_core->restore(static_cast<AlarmState>(st[1]));
        s_boot_alert = st[0] == static_cast<uint8_t>(AlarmState::PENDING) ||
                       st[0] == static_cast<uint8_t>(AlarmState::TRIGGERED);
        std::memcpy(s_saved, st, sizeof(st));
    }
    local_dev_register(kAlarmIeee, local_cmd, local_exposes);
    ESP_LOGI(TAG, "%s%s; %u sensor(s), %u siren(s), %u keypad(s)", alarm_state_name(s_core->state()),
             s_boot_alert ? " (restarted during an alarm)" : "", s_cfg->n_sensors, s_cfg->n_sirens, s_cfg->n_keypads);
}

void alarm_attach_device() {
    static ZapDevice snap, before;   // app_main's stack is tight
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
}

void alarm_start() {
    // Contacts read outside s_mu: the shadow's lock is never taken under it.
    static uint64_t ids[kAlarmMaxSensors];   // app_main's stack is tight
    static int8_t seed[kAlarmMaxSensors];
    uint8_t n;
    {
        Lock l;
        n = s_cfg->n_sensors;
        for (uint8_t i = 0; i < n; i++) ids[i] = s_cfg->sensors[i].ieee;
    }
    for (uint8_t i = 0; i < n; i++) seed[i] = contact_now(ids[i]);
    AlarmState st;
    {
        Lock l;
        if (s_cfg->n_sensors == n) std::memcpy(s_contact, seed, n);
        st = s_core->state();
    }
    publish_state(st, 0);
    if (event_bus_subscribe(EventType::ZCL_ATTR, on_attr, attr_filter) == EVENT_SUB_INVALID)
        ESP_LOGE(TAG, "not subscribed: sensors and keypads are not watched");
    // Internal RAM (xTaskCreate): it writes NVS, so its stack cannot sit in PSRAM.
    if (xTaskCreate(alarm_task, "alarm", 8192, nullptr, 3, &s_task) != pdPASS)
        ESP_LOGE(TAG, "task not started: no sirens, notifications or delays");
}

AlarmArmResult alarm_arm(AlarmMode m, bool force, uint64_t* open, uint8_t* n_open) {
    uint64_t ids[kAlarmMaxSensors];
    uint8_t n = 0;
    {
        Lock l;
        for (uint8_t i = 0; i < s_cfg->n_sensors; i++)
            if (s_cfg->sensors[i].modes & (1u << static_cast<uint8_t>(m))) ids[n++] = s_cfg->sensors[i].ieee;
    }
    uint8_t no = 0;
    for (uint8_t i = 0; i < n; i++) if (contact_now(ids[i]) == 0) open[no++] = ids[i];   // only contacts block
    AlarmArmResult r = AlarmArmResult::OK;
    const uint32_t now = mono_s();
    act(false, [&] { r = s_core->arm(m, force, open, no, now); });
    *n_open = no;
    return r;
}

void alarm_disarm() {
    act(false, [] { s_core->disarm(); });
}

bool alarm_config_set(const char* json, size_t len, char* err, size_t cap) {
    auto* next = new (std::nothrow) AlarmConfig();
    auto* dev  = new (std::nothrow) ZapDevice();
    char* buf  = static_cast<char*>(heap_caps_malloc(kCfgJsonMax, MALLOC_CAP_SPIRAM));
    bool ok = next && dev && buf;
    if (!ok) std::snprintf(err, cap, "out of memory");
    if (ok) {
        Lock l;   // the stored codes are read while parsing
        ok = alarm_cfg_from_json(json, len, s_cfg, next, err, cap);
    }
    // Every entry is a device on this hub; a sensor has something to watch.
    auto known = [&](const char* list, unsigned i, uint64_t ieee) {
        if (ieee != kAlarmIeee && zigbee_pool_snapshot(ieee, dev) && !zap_dev_is_removed(dev)) return true;
        char id[19];
        ieee_str(ieee, id);
        std::snprintf(err, cap, "%s[%u]: %s is not a device on this hub", list, i, id);
        return false;
    };
    for (uint8_t i = 0; ok && i < next->n_sensors; i++) {
        ok = known("sensors", i, next->sensors[i].ieee);
        if (!ok) break;
        const size_t n = zhac_adapter_build_exposes_json(dev->ieee_addr, dev->model_id, dev->manufacturer_name,
                                                         buf, kCfgJsonMax);
        buf[n < kCfgJsonMax ? n : 0] = '\0';
        ok = std::strstr(buf, "\"name\":\"contact\"") || std::strstr(buf, "\"name\":\"occupancy\"") ||
             std::strstr(buf, "\"name\":\"presence\"") || std::strstr(buf, "\"name\":\"vibration\"");
        if (!ok) std::snprintf(err, cap, "sensors[%u]: %s has no contact, occupancy, presence or vibration", i,
                               dev->friendly_name);
    }
    for (uint8_t i = 0; ok && i < next->n_sirens; i++) ok = known("sirens", i, next->sirens[i].ieee);
    for (uint8_t i = 0; ok && i < next->n_keypads; i++) ok = known("keypads", i, next->keypads[i].ieee);
    if (ok) {
        const size_t n = alarm_cfg_to_json(*next, true, buf, kCfgJsonMax);
        ok = n && nvs_put("cfg", buf, n);
        if (!ok) std::snprintf(err, cap, "could not save the config");
    }
    if (ok) {
        int8_t seed[kAlarmMaxSensors];
        for (uint8_t i = 0; i < next->n_sensors; i++) seed[i] = contact_now(next->sensors[i].ieee);
        Lock l;
        *s_cfg = *next;
        std::memcpy(s_contact, seed, sizeof(seed));
        std::memset(s_pad, 0, sizeof(s_pad));
        ESP_LOGI(TAG, "config saved: %u sensor(s), %u siren(s), %u keypad(s), %u code(s)", s_cfg->n_sensors,
                 s_cfg->n_sirens, s_cfg->n_keypads, s_cfg->n_codes);
    }
    heap_caps_free(buf);
    delete dev;
    delete next;
    return ok;
}

void alarm_status_json(JsonObject o) {
    AlarmState st;
    AlarmMode m;
    uint64_t cause, byp[kAlarmMaxSensors];
    uint32_t deadline, since;
    uint8_t nb;
    {
        Lock l;
        st = s_core->state();
        m = s_core->mode();
        cause = s_core->cause();
        deadline = s_core->deadline();
        since = s_since;
        nb = s_core->bypass_count();
        for (uint8_t i = 0; i < nb; i++) byp[i] = s_core->bypass_at(i);
    }
    o["state"] = alarm_state_name(st);
    o["mode"]  = alarm_mode_name(m);
    o["since"] = since;
    if (st == AlarmState::ARMING || st == AlarmState::PENDING || st == AlarmState::TRIGGERED) {
        const uint32_t now = mono_s();
        o["left"] = deadline > now ? deadline - now : 0;
    }
    char id[19];
    if (cause) {
        char name[32];
        ieee_str(cause, id);
        name_of(cause, name);
        o["cause"]  = static_cast<const char*>(id);
        o["sensor"] = static_cast<const char*>(name);
    }
    JsonArray b = o["bypassed"].to<JsonArray>();
    for (uint8_t i = 0; i < nb; i++) {
        ieee_str(byp[i], id);
        b.add(static_cast<const char*>(id));
    }
}

void alarm_config_json(JsonObject o) {
    Lock l;
    alarm_cfg_write(*s_cfg, false, o);
}
