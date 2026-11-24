// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Alarm panel: the state machine, the keypad code policy and the config JSON.
// Spec: extra/docs/superpowers/specs/2026-11-24-security-mode-design.md.
#include "alarm_core.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <new>

namespace {

const char* const kStates[] = {"disarmed", "arming", "armed_away", "armed_home", "armed_night", "pending", "triggered"};
const char* const kModes[] = {"away", "home", "night"};
const char* const kKinds[] = {"warning", "alarm", "state"};
// z2m's warning object (toZigbee.ts warning / modernExtend iasWarning).
const char* const kWdModes[] = {"burglar", "fire", "emergency", "police_panic", "fire_panic", "emergency_panic"};
const char* const kWdLevels[] = {"low", "medium", "high", "very_high"};

template <size_t N>
int word_index(const char* s, const char* const (&words)[N]) {
    if (!s) return -1;
    for (size_t i = 0; i < N; i++) if (std::strcmp(s, words[i]) == 0) return static_cast<int>(i);
    return -1;
}

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// "0x" and 1-16 hex digits, not zero.
bool parse_ieee(JsonVariantConst v, uint64_t* out) {
    const char* s = v.is<const char*>() ? v.as<const char*>() : nullptr;
    if (!s || s[0] != '0' || (s[1] != 'x' && s[1] != 'X')) return false;
    uint64_t x = 0;
    size_t n = 0;
    for (const char* p = s + 2; *p; p++, n++) {
        const int d = hex_digit(*p);
        if (d < 0 || n >= 16) return false;
        x = x << 4 | static_cast<uint64_t>(d);
    }
    if (!n || !x) return false;
    *out = x;
    return true;
}

void ieee_str(uint64_t ieee, char (&buf)[19]) { std::snprintf(buf, sizeof(buf), "0x%016" PRIX64, ieee); }

bool is_code(JsonVariantConst v) {
    const char* s = v.is<const char*>() ? v.as<const char*>() : nullptr;
    if (!s) return false;
    const size_t n = std::strlen(s);
    if (n < 4 || n >= kAlarmCodeMax) return false;
    for (size_t i = 0; i < n; i++) if (s[i] < '0' || s[i] > '9') return false;
    return true;
}

// A number field in [lo, hi]; absent keeps `*out`.
bool num_field(JsonObjectConst o, const char* key, long lo, long hi, uint16_t* out) {
    JsonVariantConst v = o[key];
    if (v.isNull()) return true;
    if (!v.is<long>() || v.as<long>() < lo || v.as<long>() > hi) return false;
    *out = static_cast<uint16_t>(v.as<long>());
    return true;
}

// A bool field; absent keeps `*out`.
bool bool_field(JsonObjectConst o, const char* key, bool* out) {
    JsonVariantConst v = o[key];
    if (v.isNull()) return true;
    if (!v.is<bool>()) return false;
    *out = v.as<bool>();
    return true;
}

// A string field that fits `cap` (NUL included); absent leaves `out` empty.
bool str_field(JsonObjectConst o, const char* key, char* out, size_t cap) {
    JsonVariantConst v = o[key];
    out[0] = '\0';
    if (v.isNull()) return true;
    if (!v.is<const char*>() || std::strlen(v.as<const char*>()) >= cap) return false;
    std::snprintf(out, cap, "%s", v.as<const char*>());
    return true;
}

// Common to every list: an array (or absent) of at most `max` objects.
bool list_field(JsonObjectConst root, const char* key, size_t max, JsonArrayConst* out, char* err, size_t cap) {
    JsonVariantConst v = root[key];
    if (v.isNull()) { *out = JsonArrayConst(); return true; }
    if (!v.is<JsonArrayConst>()) { std::snprintf(err, cap, "%s must be a list", key); return false; }
    *out = v.as<JsonArrayConst>();
    if (out->size() > max) { std::snprintf(err, cap, "%s: at most %u", key, static_cast<unsigned>(max)); return false; }
    return true;
}

bool parse_sensors(JsonObjectConst root, AlarmConfig* c, char* err, size_t cap) {
    JsonArrayConst arr;
    if (!list_field(root, "sensors", kAlarmMaxSensors, &arr, err, cap)) return false;
    for (JsonVariantConst item : arr) {
        const unsigned i = c->n_sensors;
        JsonObjectConst o = item.as<JsonObjectConst>();
        AlarmSensor s{};
        if (!parse_ieee(o["ieee"], &s.ieee)) {
            std::snprintf(err, cap, "sensors[%u]: ieee must be 0x and up to 16 hex digits", i);
            return false;
        }
        if (alarm_cfg_sensor(*c, s.ieee)) {
            char id[19];
            ieee_str(s.ieee, id);
            std::snprintf(err, cap, "sensors[%u]: %s is listed twice", i, id);
            return false;
        }
        JsonVariantConst modes = o["modes"];
        if (!modes.is<JsonArrayConst>()) { std::snprintf(err, cap, "sensors[%u]: modes must be a list", i); return false; }
        for (JsonVariantConst m : modes.as<JsonArrayConst>()) {
            const int k = word_index(m.is<const char*>() ? m.as<const char*>() : nullptr, kModes);
            if (k < 0) { std::snprintf(err, cap, "sensors[%u]: mode must be away, home or night", i); return false; }
            s.modes |= static_cast<uint8_t>(1u << k);
        }
        if (!s.modes) { std::snprintf(err, cap, "sensors[%u]: no mode, so it never guards", i); return false; }
        if (!bool_field(o, "entry", &s.entry)) { std::snprintf(err, cap, "sensors[%u]: entry must be true or false", i); return false; }
        c->sensors[c->n_sensors++] = s;
    }
    return true;
}

bool parse_sirens(JsonObjectConst root, AlarmConfig* c, char* err, size_t cap) {
    JsonArrayConst arr;
    if (!list_field(root, "sirens", kAlarmMaxSirens, &arr, err, cap)) return false;
    for (JsonVariantConst item : arr) {
        const unsigned i = c->n_sirens;
        JsonObjectConst o = item.as<JsonObjectConst>();
        AlarmSiren s{};
        if (!parse_ieee(o["ieee"], &s.ieee)) {
            std::snprintf(err, cap, "sirens[%u]: ieee must be 0x and up to 16 hex digits", i);
            return false;
        }
        for (uint8_t k = 0; k < c->n_sirens; k++) {
            if (c->sirens[k].ieee == s.ieee) { std::snprintf(err, cap, "sirens[%u]: listed twice", i); return false; }
        }
        const int kind = word_index(o["kind"].is<const char*>() ? o["kind"].as<const char*>() : nullptr, kKinds);
        if (kind < 0) { std::snprintf(err, cap, "sirens[%u]: kind must be warning, alarm or state", i); return false; }
        s.kind = static_cast<AlarmSirenKind>(kind);
        if (s.kind == AlarmSirenKind::WARNING) {
            std::snprintf(s.wd_mode, sizeof(s.wd_mode), "burglar");
            std::snprintf(s.wd_level, sizeof(s.wd_level), "high");
            s.strobe = true;
            JsonVariantConst m = o["mode"], l = o["level"];
            if (!m.isNull()) {
                if (word_index(m.is<const char*>() ? m.as<const char*>() : nullptr, kWdModes) < 0) {
                    std::snprintf(err, cap, "sirens[%u]: mode must be burglar, fire, emergency, police_panic, "
                                  "fire_panic or emergency_panic", i);
                    return false;
                }
                std::snprintf(s.wd_mode, sizeof(s.wd_mode), "%s", m.as<const char*>());
            }
            if (!l.isNull()) {
                if (word_index(l.is<const char*>() ? l.as<const char*>() : nullptr, kWdLevels) < 0) {
                    std::snprintf(err, cap, "sirens[%u]: level must be low, medium, high or very_high", i);
                    return false;
                }
                std::snprintf(s.wd_level, sizeof(s.wd_level), "%s", l.as<const char*>());
            }
            if (!bool_field(o, "strobe", &s.strobe)) {
                std::snprintf(err, cap, "sirens[%u]: strobe must be true or false", i);
                return false;
            }
        } else if (s.kind == AlarmSirenKind::ALARM) {
            if (!str_field(o, "melody", s.melody, sizeof(s.melody))) {
                std::snprintf(err, cap, "sirens[%u]: melody must be text of up to %u characters", i,
                              static_cast<unsigned>(kAlarmEnumMax - 1));
                return false;
            }
            if (!str_field(o, "volume", s.volume, sizeof(s.volume))) {
                std::snprintf(err, cap, "sirens[%u]: volume must be text of up to %u characters", i,
                              static_cast<unsigned>(kAlarmEnumMax - 1));
                return false;
            }
        }
        c->sirens[c->n_sirens++] = s;
    }
    return true;
}

bool parse_keypads(JsonObjectConst root, AlarmConfig* c, char* err, size_t cap) {
    JsonArrayConst arr;
    if (!list_field(root, "keypads", kAlarmMaxKeypads, &arr, err, cap)) return false;
    for (JsonVariantConst item : arr) {
        const unsigned i = c->n_keypads;
        JsonObjectConst o = item.as<JsonObjectConst>();
        AlarmKeypad k{};
        if (!parse_ieee(o["ieee"], &k.ieee)) {
            std::snprintf(err, cap, "keypads[%u]: ieee must be 0x and up to 16 hex digits", i);
            return false;
        }
        if (alarm_cfg_keypad(*c, k.ieee)) { std::snprintf(err, cap, "keypads[%u]: listed twice", i); return false; }
        if (!bool_field(o, "trusted", &k.trusted)) {
            std::snprintf(err, cap, "keypads[%u]: trusted must be true or false", i);
            return false;
        }
        c->keypads[c->n_keypads++] = k;
    }
    return true;
}

bool parse_codes(JsonObjectConst root, const AlarmConfig* keep, AlarmConfig* c, char* err, size_t cap) {
    JsonVariantConst v = root["codes"];
    if (v.isNull()) {
        if (keep) {
            c->n_codes = keep->n_codes;
            std::memcpy(c->codes, keep->codes, sizeof(c->codes));
        }
        return true;
    }
    JsonArrayConst arr;
    if (!list_field(root, "codes", kAlarmMaxCodes, &arr, err, cap)) return false;
    for (JsonVariantConst item : arr) {
        const unsigned i = c->n_codes;
        if (!is_code(item)) { std::snprintf(err, cap, "codes[%u]: a code is 4-8 digits, as text", i); return false; }
        for (uint8_t k = 0; k < c->n_codes; k++) {
            if (std::strcmp(c->codes[k], item.as<const char*>()) == 0) {
                std::snprintf(err, cap, "codes[%u]: listed twice", i);
                return false;
            }
        }
        std::snprintf(c->codes[c->n_codes++], kAlarmCodeMax, "%s", item.as<const char*>());
    }
    return true;
}

}  // namespace

const char* alarm_state_name(AlarmState s) {
    const auto i = static_cast<size_t>(s);
    return i < sizeof(kStates) / sizeof(kStates[0]) ? kStates[i] : "disarmed";
}

bool alarm_state_parse(const char* s, AlarmState* out) {
    const int i = word_index(s, kStates);
    if (i < 0) return false;
    *out = static_cast<AlarmState>(i);
    return true;
}

const char* alarm_mode_name(AlarmMode m) {
    const auto i = static_cast<size_t>(m);
    return i < sizeof(kModes) / sizeof(kModes[0]) ? kModes[i] : "away";
}

bool alarm_mode_parse(const char* s, AlarmMode* out) {
    const int i = word_index(s, kModes);
    if (i < 0) return false;
    *out = static_cast<AlarmMode>(i);
    return true;
}

AlarmState alarm_armed_state(AlarmMode m) {
    switch (m) {
    case AlarmMode::HOME:  return AlarmState::ARMED_HOME;
    case AlarmMode::NIGHT: return AlarmState::ARMED_NIGHT;
    case AlarmMode::AWAY:  break;
    }
    return AlarmState::ARMED_AWAY;
}

bool alarm_cfg_from_json(const char* json, size_t len, const AlarmConfig* keep,
                         AlarmConfig* out, char* err, size_t cap) {
    err[0] = '\0';
    JsonDocument doc;
    if (!json || deserializeJson(doc, json, len) || !doc.is<JsonObjectConst>()) {
        std::snprintf(err, cap, "the config must be a JSON object");
        return false;
    }
    JsonObjectConst root = doc.as<JsonObjectConst>();
    // The defaults, in place: a config is ~2 KB, too much for a boot-time stack.
    AlarmConfig& c = *new (out) AlarmConfig();
    if (!num_field(root, "exit_delay", 0, kAlarmDelayMax, &c.exit_delay)) {
        std::snprintf(err, cap, "exit_delay must be 0-%u s", kAlarmDelayMax);
        return false;
    }
    if (!num_field(root, "entry_delay", 0, kAlarmDelayMax, &c.entry_delay)) {
        std::snprintf(err, cap, "entry_delay must be 0-%u s", kAlarmDelayMax);
        return false;
    }
    if (!num_field(root, "siren_time", kAlarmSirenMin, kAlarmSirenMax, &c.siren_time)) {
        std::snprintf(err, cap, "siren_time must be %u-%u s", kAlarmSirenMin, kAlarmSirenMax);
        return false;
    }
    if (!bool_field(root, "notify_arm", &c.notify_arm)) {
        std::snprintf(err, cap, "notify_arm must be true or false");
        return false;
    }
    return parse_sensors(root, &c, err, cap) && parse_sirens(root, &c, err, cap) &&
           parse_keypads(root, &c, err, cap) && parse_codes(root, keep, &c, err, cap);
}

size_t alarm_cfg_to_json(const AlarmConfig& c, bool with_codes, char* buf, size_t cap) {
    JsonDocument doc;
    alarm_cfg_write(c, with_codes, doc.to<JsonObject>());
    if (measureJson(doc) + 1 > cap) return 0;
    return serializeJson(doc, buf, cap);
}

void alarm_cfg_write(const AlarmConfig& c, bool with_codes, JsonObject doc) {
    doc["exit_delay"] = c.exit_delay;
    doc["entry_delay"] = c.entry_delay;
    doc["siren_time"] = c.siren_time;
    doc["notify_arm"] = c.notify_arm;
    char id[19];
    JsonArray sensors = doc["sensors"].to<JsonArray>();
    for (uint8_t i = 0; i < c.n_sensors; i++) {
        JsonObject o = sensors.add<JsonObject>();
        ieee_str(c.sensors[i].ieee, id);
        o["ieee"] = static_cast<const char*>(id);   // a copy: id is reused
        JsonArray modes = o["modes"].to<JsonArray>();
        for (uint8_t k = 0; k < 3; k++) if (c.sensors[i].modes & (1u << k)) modes.add(kModes[k]);
        o["entry"] = c.sensors[i].entry;
    }
    JsonArray sirens = doc["sirens"].to<JsonArray>();
    for (uint8_t i = 0; i < c.n_sirens; i++) {
        const AlarmSiren& s = c.sirens[i];
        JsonObject o = sirens.add<JsonObject>();
        ieee_str(s.ieee, id);
        o["ieee"] = static_cast<const char*>(id);
        o["kind"] = kKinds[static_cast<size_t>(s.kind)];
        if (s.kind == AlarmSirenKind::WARNING) {
            o["mode"] = static_cast<const char*>(s.wd_mode);
            o["level"] = static_cast<const char*>(s.wd_level);
            o["strobe"] = s.strobe;
        } else if (s.kind == AlarmSirenKind::ALARM) {
            if (s.melody[0]) o["melody"] = static_cast<const char*>(s.melody);
            if (s.volume[0]) o["volume"] = static_cast<const char*>(s.volume);
        }
    }
    JsonArray keypads = doc["keypads"].to<JsonArray>();
    for (uint8_t i = 0; i < c.n_keypads; i++) {
        JsonObject o = keypads.add<JsonObject>();
        ieee_str(c.keypads[i].ieee, id);
        o["ieee"] = static_cast<const char*>(id);
        o["trusted"] = c.keypads[i].trusted;
    }
    if (with_codes) {
        JsonArray codes = doc["codes"].to<JsonArray>();
        for (uint8_t i = 0; i < c.n_codes; i++) codes.add(static_cast<const char*>(c.codes[i]));
    } else {
        doc["codes_count"] = c.n_codes;
    }
}

const AlarmSensor* alarm_cfg_sensor(const AlarmConfig& c, uint64_t ieee) {
    for (uint8_t i = 0; i < c.n_sensors; i++) if (c.sensors[i].ieee == ieee) return &c.sensors[i];
    return nullptr;
}

const AlarmKeypad* alarm_cfg_keypad(const AlarmConfig& c, uint64_t ieee) {
    for (uint8_t i = 0; i < c.n_keypads; i++) if (c.keypads[i].ieee == ieee) return &c.keypads[i];
    return nullptr;
}

bool alarm_keypad_may_disarm(const AlarmConfig& c, uint64_t keypad, const char* code) {
    const AlarmKeypad* k = alarm_cfg_keypad(c, keypad);
    if (!k) return false;
    if (k->trusted) return true;
    if (!code || !*code) return false;
    for (uint8_t i = 0; i < c.n_codes; i++) if (std::strcmp(c.codes[i], code) == 0) return true;
    return false;
}

const char* alarm_siren_misfit(AlarmSirenKind kind, const char* exposes, size_t len) {
    static constexpr struct { const char* key; const char* why; } kNeed[] = {   // by AlarmSirenKind
        {"warning", "has no warning the hub can send (kind warning is for IAS sirens)"},
        {"alarm",   "has no alarm switch the hub can set (kind alarm is for Tuya sirens)"},
        {"state",   "has no state the hub can switch (kind state is for plugs and switches)"},
    };
    const auto& need = kNeed[static_cast<uint8_t>(kind)];
    JsonDocument filter;   // a device's exposes run to kilobytes: keep only what is checked
    filter[0]["name"]   = true;
    filter[0]["access"] = true;
    JsonDocument doc;
    if (!exposes || deserializeJson(doc, exposes, len, DeserializationOption::Filter(filter)) != DeserializationError::Ok)
        return "has exposes the hub cannot read";
    for (JsonObjectConst e : doc.as<JsonArrayConst>())
        if (std::strcmp(e["name"] | "", need.key) == 0 && ((e["access"] | 0) & 2)) return nullptr;   // 2: settable
    return need.why;
}

// ── The panel ──

AlarmArmResult AlarmCore::arm(AlarmMode m, bool force, const uint64_t* open, uint8_t n_open, uint32_t now) {
    if (in_alarm()) return AlarmArmResult::BUSY;
    const AlarmState target = alarm_armed_state(m);
    if (state_ == target || (state_ == AlarmState::ARMING && mode_ == m)) return AlarmArmResult::OK;
    if (n_open && !force) return AlarmArmResult::OPEN;
    n_bypass_ = 0;
    for (uint8_t i = 0; open && i < n_open && n_bypass_ < kAlarmMaxSensors; i++) bypass_[n_bypass_++] = open[i];
    mode_ = m;
    cause_ = 0;
    if (m == AlarmMode::AWAY && cfg_->exit_delay) {
        state_ = AlarmState::ARMING;
        deadline_ = now + cfg_->exit_delay;
    } else {
        state_ = target;
    }
    return AlarmArmResult::OK;
}

void AlarmCore::disarm() {
    state_ = AlarmState::DISARMED;
    cause_ = 0;
    deadline_ = 0;
    n_bypass_ = 0;
}

void AlarmCore::trigger(uint64_t cause, uint32_t now) {
    return_to_ = resting();
    state_ = AlarmState::TRIGGERED;
    cause_ = cause;
    deadline_ = now + cfg_->siren_time;
}

void AlarmCore::sensor_active(uint64_t ieee, uint32_t now) {
    const bool armed = state_ == AlarmState::ARMED_AWAY || state_ == AlarmState::ARMED_HOME ||
                       state_ == AlarmState::ARMED_NIGHT;
    if (!armed && state_ != AlarmState::PENDING) return;
    const AlarmSensor* s = alarm_cfg_sensor(*cfg_, ieee);
    if (!s || bypassed(ieee) || !(s->modes & (1u << static_cast<uint8_t>(mode_)))) return;
    if (state_ == AlarmState::PENDING) {
        if (!s->entry) trigger(ieee, now);   // a window while someone is at the door: no more waiting
        return;
    }
    if (s->entry && cfg_->entry_delay) {
        state_ = AlarmState::PENDING;
        cause_ = ieee;
        deadline_ = now + cfg_->entry_delay;
        return;
    }
    trigger(ieee, now);
}

void AlarmCore::panic(uint64_t source, uint32_t now) {
    if (state_ != AlarmState::TRIGGERED) trigger(source, now);
}

void AlarmCore::tick(uint32_t now) {
    if (now < deadline_) return;
    switch (state_) {
    case AlarmState::ARMING:    state_ = alarm_armed_state(mode_); break;
    case AlarmState::PENDING:   trigger(cause_, now); break;
    case AlarmState::TRIGGERED:
        state_ = return_to_;
        cause_ = 0;
        if (state_ == AlarmState::DISARMED) n_bypass_ = 0;
        break;
    default: break;
    }
}

void AlarmCore::restore(AlarmState saved) {
    cause_ = 0;
    n_bypass_ = 0;   // fail safe: a restart guards bypassed sensors again
    switch (saved) {
    case AlarmState::ARMED_AWAY:  state_ = saved; mode_ = AlarmMode::AWAY; break;
    case AlarmState::ARMED_HOME:  state_ = saved; mode_ = AlarmMode::HOME; break;
    case AlarmState::ARMED_NIGHT: state_ = saved; mode_ = AlarmMode::NIGHT; break;
    default:                      state_ = AlarmState::DISARMED; break;
    }
}

AlarmState AlarmCore::resting() const {
    switch (state_) {
    case AlarmState::ARMING:
    case AlarmState::PENDING:   return alarm_armed_state(mode_);
    case AlarmState::TRIGGERED: return return_to_;
    default:                    return state_;
    }
}

bool AlarmCore::bypassed(uint64_t ieee) const {
    for (uint8_t i = 0; i < n_bypass_; i++) if (bypass_[i] == ieee) return true;
    return false;
}
