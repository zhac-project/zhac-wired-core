// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Alarm panel: one state machine per profile, the keypad code policy and the output fit.
// Spec: extra/docs/superpowers/specs/2026-11-24-safety-profiles-design.md.
#include "alarm_core.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>

#include <ArduinoJson.h>

namespace {

const char* const kStates[] = {"disarmed", "arming", "armed", "pending", "triggered"};
// The config parser's words (the parser returns with the stored config).
[[maybe_unused]] const char* const kKinds[] = {"warning", "alarm", "state"};
// z2m's warning object (toZigbee.ts warning / modernExtend iasWarning).
[[maybe_unused]] const char* const kWdModes[] = {"burglar", "fire", "emergency", "police_panic", "fire_panic", "emergency_panic"};
[[maybe_unused]] const char* const kWdLevels[] = {"low", "medium", "high", "very_high"};

template <size_t N>
int word_index(const char* s, const char* const (&words)[N]) {
    if (!s) return -1;
    for (size_t i = 0; i < N; i++) if (std::strcmp(s, words[i]) == 0) return static_cast<int>(i);
    return -1;
}

// The JSON helpers below wait for the config parser and writer.

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// "0x" and 1-16 hex digits, not zero.
[[maybe_unused]] bool parse_ieee(JsonVariantConst v, uint64_t* out) {
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

[[maybe_unused]] void ieee_str(uint64_t ieee, char (&buf)[19]) { std::snprintf(buf, sizeof(buf), "0x%016" PRIX64, ieee); }

[[maybe_unused]] bool is_code(JsonVariantConst v) {
    const char* s = v.is<const char*>() ? v.as<const char*>() : nullptr;
    if (!s) return false;
    const size_t n = std::strlen(s);
    if (n < 4 || n >= kAlarmCodeMax) return false;
    for (size_t i = 0; i < n; i++) if (s[i] < '0' || s[i] > '9') return false;
    return true;
}

// A number field in [lo, hi]; absent keeps `*out`.
[[maybe_unused]] bool num_field(JsonObjectConst o, const char* key, long lo, long hi, uint16_t* out) {
    JsonVariantConst v = o[key];
    if (v.isNull()) return true;
    if (!v.is<long>() || v.as<long>() < lo || v.as<long>() > hi) return false;
    *out = static_cast<uint16_t>(v.as<long>());
    return true;
}

// A bool field; absent keeps `*out`.
[[maybe_unused]] bool bool_field(JsonObjectConst o, const char* key, bool* out) {
    JsonVariantConst v = o[key];
    if (v.isNull()) return true;
    if (!v.is<bool>()) return false;
    *out = v.as<bool>();
    return true;
}

// A string field that fits `cap` (NUL included); absent leaves `out` empty.
[[maybe_unused]] bool str_field(JsonObjectConst o, const char* key, char* out, size_t cap) {
    JsonVariantConst v = o[key];
    out[0] = '\0';
    if (v.isNull()) return true;
    if (!v.is<const char*>() || std::strlen(v.as<const char*>()) >= cap) return false;
    std::snprintf(out, cap, "%s", v.as<const char*>());
    return true;
}

// Common to every list: an array (or absent) of at most `max` objects.
[[maybe_unused]] bool list_field(JsonObjectConst root, const char* key, size_t max, JsonArrayConst* out, char* err,
                                 size_t cap) {
    JsonVariantConst v = root[key];
    if (v.isNull()) { *out = JsonArrayConst(); return true; }
    if (!v.is<JsonArrayConst>()) { std::snprintf(err, cap, "%s must be a list", key); return false; }
    *out = v.as<JsonArrayConst>();
    if (out->size() > max) { std::snprintf(err, cap, "%s: at most %u", key, static_cast<unsigned>(max)); return false; }
    return true;
}

bool in_alarm(AlarmState s) { return s == AlarmState::PENDING || s == AlarmState::TRIGGERED; }

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

int alarm_cfg_profile(const AlarmConfig& c, const char* id) {
    for (uint8_t i = 0; id && i < c.n_profiles; i++) if (std::strcmp(c.profiles[i].id, id) == 0) return i;
    return -1;
}

int alarm_cfg_role(const AlarmConfig& c, AlarmRole r) {
    for (uint8_t i = 0; r != AlarmRole::NONE && i < c.n_profiles; i++)
        if (c.profiles[i].cls == AlarmClass::INTRUSION && c.profiles[i].role == r) return i;
    return -1;
}

int alarm_cfg_sensor(const AlarmConfig& c, uint64_t ieee) {
    for (uint8_t i = 0; i < c.n_sensors; i++) if (c.sensors[i].ieee == ieee) return i;
    return -1;
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

const char* alarm_output_misfit(AlarmOutKind kind, const char* exposes, size_t len) {
    static constexpr struct { const char* key; const char* why; } kNeed[] = {   // by AlarmOutKind
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

bool AlarmPanel::intrusion(uint8_t p) const { return cfg_->profiles[p].cls == AlarmClass::INTRUSION; }

bool AlarmPanel::remember(uint8_t slot, AlarmKey key, bool active) {
    Last& l = last_[slot];
    if (key == AlarmKey::MOTION) return active;   // every report counts, nothing kept
    if (key == AlarmKey::CONTACT) {
        const bool was = l.contact == Contact::OPEN;
        l.contact = active ? Contact::OPEN : Contact::CLOSED;
        return active && !was;
    }
    const uint8_t bit = static_cast<uint8_t>(1u << (static_cast<uint8_t>(key) - static_cast<uint8_t>(AlarmKey::WATER_LEAK)));
    const bool was = l.hazards & bit;
    l.hazards = active ? (l.hazards | bit) : (l.hazards & ~bit);
    return active && !was;
}

void AlarmPanel::seed(uint8_t slot, AlarmKey key, bool active) {
    if (slot < cfg_->n_sensors) remember(slot, key, active);
}

void AlarmPanel::sensor(uint8_t slot, AlarmKey key, bool active, uint32_t now) {
    if (slot >= cfg_->n_sensors || !remember(slot, key, active)) return;
    for (uint8_t i = 0; i < cfg_->n_profiles; i++) go_off(i, slot, now);
}

// Sensor `slot` went off in profile `p` (if it is in it and not bypassed).
void AlarmPanel::go_off(uint8_t p, uint8_t slot, uint32_t now) {
    AlarmProfileStatus& s = st_[p];
    const AlarmSensor& sen = cfg_->sensors[slot];
    if (!(sen.profiles >> p & 1) || (s.bypass >> slot & 1)) return;
    const uint16_t delay = (sen.entry >> p & 1) ? cfg_->profiles[p].trigger_delay : 0;
    if (s.state == AlarmState::ARMED && delay) {
        s.state = AlarmState::PENDING;
        s.cause = sen.ieee;
        s.deadline = now + delay;
    } else if (s.state == AlarmState::ARMED || (s.state == AlarmState::PENDING && !delay)) {
        trigger(p, sen.ieee, now);   // a sensor that is not delayed ends pending at once
    }
}

// The arm reached armed: every hazard sensor of the profile that is still active goes off now.
void AlarmPanel::hazard_check(uint8_t p, uint32_t now) {
    check_due_[p] = false;
    for (uint8_t slot = 0; slot < cfg_->n_sensors; slot++) if (last_[slot].hazards) go_off(p, slot, now);
}

void AlarmPanel::trigger(uint8_t p, uint64_t cause, uint32_t now) {
    AlarmProfileStatus& s = st_[p];
    s.rests_armed = s.state != AlarmState::DISARMED;
    s.state = AlarmState::TRIGGERED;
    s.cause = s.alarm_cause = cause;
    s.deadline = now + cfg_->profiles[p].siren_time;
    s.alarms++;
}

// Pending or triggered is over: back to where it rests.
void AlarmPanel::end_alarm(uint8_t p) {
    AlarmProfileStatus& s = st_[p];
    if (!s.rests_armed) { stop(p); return; }
    s.state = AlarmState::ARMED;
    s.cause = 0;
    s.deadline = 0;
}

// To disarmed, counting nothing.
void AlarmPanel::stop(uint8_t p) {
    AlarmProfileStatus& s = st_[p];
    s.state = AlarmState::DISARMED;
    s.rests_armed = false;
    s.cause = 0;
    s.deadline = 0;
    s.bypass = 0;
    check_due_[p] = false;
}

AlarmArmResult AlarmPanel::arm(uint8_t p, bool force, uint32_t now, uint64_t* open) {
    if (open) *open = 0;
    if (p >= cfg_->n_profiles) return AlarmArmResult::BUSY;   // not a profile: the caller checks ids first
    AlarmProfileStatus& s = st_[p];
    if (in_alarm(s.state)) return AlarmArmResult::BUSY;
    for (uint8_t i = 0; intrusion(p) && i < cfg_->n_profiles; i++)
        if (i != p && intrusion(i) && in_alarm(st_[i].state)) return AlarmArmResult::BUSY;
    if (s.state == AlarmState::ARMED || s.state == AlarmState::ARMING) return AlarmArmResult::OK;
    uint64_t o = 0;
    for (uint8_t slot = 0; slot < cfg_->n_sensors; slot++)
        if ((cfg_->sensors[slot].profiles >> p & 1) && last_[slot].contact == Contact::OPEN) o |= 1ull << slot;
    if (open) *open = o;
    if (o && !force) return AlarmArmResult::OPEN;
    for (uint8_t i = 0; intrusion(p) && i < cfg_->n_profiles; i++) if (i != p && intrusion(i)) stop(i);
    s.rests_armed = true;
    s.cause = 0;
    s.bypass = o;
    check_due_[p] = true;
    if (cfg_->profiles[p].arm_delay) {
        s.state = AlarmState::ARMING;
        s.deadline = now + cfg_->profiles[p].arm_delay;
    } else {
        s.state = AlarmState::ARMED;
        s.deadline = 0;
        hazard_check(p, now);
    }
    return AlarmArmResult::OK;
}

void AlarmPanel::disarm(uint8_t p) {
    if (p >= cfg_->n_profiles) return;
    if (st_[p].state != AlarmState::DISARMED) st_[p].disarms++;
    stop(p);
}

void AlarmPanel::disarm_all() {
    for (uint8_t i = 0; i < cfg_->n_profiles; i++) intrusion(i) ? disarm(i) : silence(i);
}

void AlarmPanel::silence(uint8_t p) {
    if (p < cfg_->n_profiles && in_alarm(st_[p].state)) end_alarm(p);
}

bool AlarmPanel::panic(uint64_t source, uint32_t now) {
    int p = summary_profile();
    for (uint8_t i = 0; p < 0 && i < cfg_->n_profiles; i++) if (intrusion(i)) p = i;
    if (p < 0) return false;
    if (st_[p].state != AlarmState::TRIGGERED) trigger(static_cast<uint8_t>(p), source, now);
    return true;
}

void AlarmPanel::tick(uint32_t now) {
    for (uint8_t i = 0; i < cfg_->n_profiles; i++) {
        AlarmProfileStatus& s = st_[i];
        if (now >= s.deadline) {
            if (s.state == AlarmState::ARMING) { s.state = AlarmState::ARMED; s.deadline = 0; }
            else if (s.state == AlarmState::PENDING) trigger(i, s.cause, now);
            else if (s.state == AlarmState::TRIGGERED) end_alarm(i);
        }
        if (s.state == AlarmState::ARMED && check_due_[i]) hazard_check(i, now);
    }
}

void AlarmPanel::restore(uint8_t p, bool armed) {
    if (p >= cfg_->n_profiles) return;
    stop(p);   // fail safe: a restart guards bypassed sensors again
    if (armed) {
        st_[p].state = AlarmState::ARMED;
        st_[p].rests_armed = true;
    }
}

const AlarmProfileStatus& AlarmPanel::at(uint8_t p) const {
    static const AlarmProfileStatus kNone;
    return p < kAlarmMaxProfiles ? st_[p] : kNone;
}

bool AlarmPanel::running() const {
    for (uint8_t i = 0; i < cfg_->n_profiles; i++)
        if (st_[i].state != AlarmState::DISARMED && st_[i].state != AlarmState::ARMED) return true;
    return false;
}

int AlarmPanel::summary_profile() const {
    for (uint8_t i = 0; i < cfg_->n_profiles; i++) if (intrusion(i) && st_[i].state != AlarmState::DISARMED) return i;
    return -1;
}
