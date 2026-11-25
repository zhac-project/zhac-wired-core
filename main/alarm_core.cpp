// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Alarm panel: one state machine per profile, the config JSON, the keypad code policy and the output fit.
// Spec: extra/docs/superpowers/specs/2026-11-24-safety-profiles-design.md.
#include "alarm_core.h"

#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <new>

#include "utf8_seq.h"

namespace {

const char* const kStates[] = {"disarmed", "arming", "armed", "pending", "triggered"};
const char* const kClasses[] = {"intrusion", "fire", "water", "gas", "other"};   // by AlarmClass
const char* const kModes[] = {"away", "home", "night"};                         // a role is index + 1; stage 1's modes
const char* const kReserved[] = {"alarm_state", "alarm_sensor", "armed", "disarmed", "arming", "pending", "triggered"};
const char* const kKinds[] = {"warning", "alarm", "state"};
const char* const kOnOff[] = {"on", "off"};
// z2m's warning object (toZigbee.ts warning / modernExtend iasWarning).
const char* const kWdModes[] = {"burglar", "fire", "emergency", "police_panic", "fire_panic", "emergency_panic"};
const char* const kWdLevels[] = {"low", "medium", "high", "very_high"};
// Stage 1's defaults, which a hub with no config keeps.
constexpr uint16_t kExitDefault = 30, kEntryDefault = 30, kSirenDefault = 180;

template <size_t N>
int word_index(const char* s, const char* const (&words)[N]) {
    if (!s) return -1;
    for (size_t i = 0; i < N; i++) if (std::strcmp(s, words[i]) == 0) return static_cast<int>(i);
    return -1;
}

// Writes the message into `err` and returns false, so a check reads `return fail(...)`.
__attribute__((format(printf, 3, 4))) bool fail(char* err, size_t cap, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(err, cap, fmt, ap);
    va_end(ap);
    return false;
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

// A string field that fits `cap` (NUL included): UTF-8 (replies carry it as it is, and a text frame that is not UTF-8
// is dropped), without control characters (each takes 6 bytes in the stored form, and the largest config must fit
// 8 KB there); absent leaves `out` empty.
bool str_field(JsonObjectConst o, const char* key, char* out, size_t cap) {
    JsonVariantConst v = o[key];
    out[0] = '\0';
    if (v.isNull()) return true;
    if (!v.is<const char*>()) return false;
    const char* s = v.as<const char*>();
    const size_t n = std::strlen(s);
    if (n >= cap || !utf8_valid(s, n)) return false;
    for (const char* p = s; *p; p++)
        if (static_cast<unsigned char>(*p) < 0x20 || *p == 0x7F) return false;
    std::snprintf(out, cap, "%s", s);
    return true;
}

// Common to every list: an array (or absent) of at most `max` objects. `prefix` is the path of `root`
// ("" or "profiles[2].") in the messages.
bool list_field(JsonObjectConst root, const char* prefix, const char* key, size_t max, JsonArrayConst* out,
                char* err, size_t cap) {
    JsonVariantConst v = root[key];
    if (v.isNull()) { *out = JsonArrayConst(); return true; }
    if (!v.is<JsonArrayConst>()) return fail(err, cap, "%s%s must be a list", prefix, key);
    *out = v.as<JsonArrayConst>();
    if (out->size() > max) return fail(err, cap, "%s%s: at most %u", prefix, key, static_cast<unsigned>(max));
    return true;
}

// One output for profile `profile`, appended to `c`. `where` names it in messages; the outputs from `first` on
// are the profile's own. A stage 1 siren has no value or keep: they are not read.
bool parse_output(JsonObjectConst o, const char* where, bool stage1, uint8_t profile, uint8_t first,
                  AlarmConfig* c, char* err, size_t cap) {
    AlarmOutput s{};
    s.profile = profile;
    if (!parse_ieee(o["ieee"], &s.ieee)) return fail(err, cap, "%s: ieee must be 0x and up to 16 hex digits", where);
    for (uint8_t k = first; k < c->n_outputs; k++)
        if (c->outputs[k].ieee == s.ieee) return fail(err, cap, "%s: listed twice", where);
    const int kind = word_index(o["kind"].is<const char*>() ? o["kind"].as<const char*>() : nullptr, kKinds);
    if (kind < 0) return fail(err, cap, "%s: kind must be warning, alarm or state", where);
    s.kind = static_cast<AlarmOutKind>(kind);
    if (s.kind == AlarmOutKind::WARNING) {
        std::snprintf(s.wd_mode, sizeof(s.wd_mode), "burglar");
        std::snprintf(s.wd_level, sizeof(s.wd_level), "high");
        s.strobe = true;
        JsonVariantConst m = o["mode"], l = o["level"];
        if (!m.isNull()) {
            if (word_index(m.is<const char*>() ? m.as<const char*>() : nullptr, kWdModes) < 0)
                return fail(err, cap, "%s: mode must be burglar, fire, emergency, police_panic, fire_panic or emergency_panic", where);
            std::snprintf(s.wd_mode, sizeof(s.wd_mode), "%s", m.as<const char*>());
        }
        if (!l.isNull()) {
            if (word_index(l.is<const char*>() ? l.as<const char*>() : nullptr, kWdLevels) < 0)
                return fail(err, cap, "%s: level must be low, medium, high or very_high", where);
            std::snprintf(s.wd_level, sizeof(s.wd_level), "%s", l.as<const char*>());
        }
        if (!bool_field(o, "strobe", &s.strobe)) return fail(err, cap, "%s: strobe must be true or false", where);
    } else if (s.kind == AlarmOutKind::ALARM) {
        if (!str_field(o, "melody", s.melody, sizeof(s.melody)))
            return fail(err, cap, "%s: melody must be plain text of up to %u bytes", where, static_cast<unsigned>(kAlarmEnumMax - 1));
        if (!str_field(o, "volume", s.volume, sizeof(s.volume)))
            return fail(err, cap, "%s: volume must be plain text of up to %u bytes", where, static_cast<unsigned>(kAlarmEnumMax - 1));
    } else {
        s.value = true;
    }
    if (!stage1) {
        if (s.kind != AlarmOutKind::STATE) {
            if (!o["value"].isNull() || !o["keep"].isNull()) return fail(err, cap, "%s: value and keep are for kind state", where);
        } else {
            JsonVariantConst v = o["value"];
            if (!v.isNull()) {
                const char* w = v.is<const char*>() ? v.as<const char*>() : nullptr;
                if (word_index(w, kOnOff) < 0) return fail(err, cap, "%s: value must be on or off", where);
                s.value = std::strcmp(w, "on") == 0;
            }
            if (!bool_field(o, "keep", &s.keep)) return fail(err, cap, "%s: keep must be true or false", where);
            // The outputs before `first` are other profiles': one switch must mean the same in all of them.
            for (uint8_t k = 0; k < first; k++) {
                const AlarmOutput& e = c->outputs[k];
                if (e.ieee == s.ieee && e.kind == AlarmOutKind::STATE && (e.value != s.value || e.keep != s.keep))
                    return fail(err, cap, "%s: the same switch has another value or another keep in %s", where, c->profiles[e.profile].id);
            }
        }
    }
    if (c->n_outputs >= kAlarmMaxOutputs) return fail(err, cap, "%s: more than %u outputs in all", where, static_cast<unsigned>(kAlarmMaxOutputs));
    c->outputs[c->n_outputs++] = s;
    return true;
}

// 1-13 of a-z, 0-9, _, starting with a letter.
bool valid_id(const char* s) {
    const size_t n = std::strlen(s);
    if (n < 1 || n >= kAlarmIdMax || s[0] < 'a' || s[0] > 'z') return false;
    for (size_t i = 1; i < n; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '_')) return false;
    return true;
}

// 1-24 bytes of UTF-8 without control characters (a NUL from "\u0000" counts as one).
bool valid_name(JsonVariantConst v) {
    if (!v.is<const char*>()) return false;
    const char* s = v.as<const char*>();
    const size_t n = v.as<JsonString>().size();
    if (n < 1 || n >= kAlarmNameMax || !utf8_valid(s, n)) return false;
    for (size_t i = 0; i < n; i++)
        if (static_cast<unsigned char>(s[i]) < 0x20 || s[i] == 0x7F) return false;
    return true;
}

// The sensors of profile `pi`. A device is stored once, with a bit per profile.
bool parse_profile_sensors(JsonObjectConst po, uint8_t pi, AlarmConfig* c, char* err, size_t cap) {
    char prefix[16];
    std::snprintf(prefix, sizeof(prefix), "profiles[%u].", static_cast<unsigned>(pi));
    JsonArrayConst arr;
    if (!list_field(po, prefix, "sensors", kAlarmMaxSensors, &arr, err, cap)) return false;
    unsigned j = 0;
    for (JsonVariantConst item : arr) {
        char where[40];
        std::snprintf(where, sizeof(where), "%ssensors[%u]", prefix, j++);
        JsonObjectConst o = item.as<JsonObjectConst>();
        uint64_t ieee;
        if (!parse_ieee(o["ieee"], &ieee)) return fail(err, cap, "%s: ieee must be 0x and up to 16 hex digits", where);
        int slot = alarm_cfg_sensor(*c, ieee);
        if (slot >= 0 && (c->sensors[slot].profiles >> pi & 1)) return fail(err, cap, "%s: listed twice", where);
        bool entry = false;
        if (!bool_field(o, "entry", &entry)) return fail(err, cap, "%s: entry must be true or false", where);
        if (slot < 0) {
            if (c->n_sensors >= kAlarmMaxSensors)
                return fail(err, cap, "%s: more than %u different sensors", where, static_cast<unsigned>(kAlarmMaxSensors));
            slot = c->n_sensors++;
            c->sensors[slot].ieee = ieee;
        }
        c->sensors[slot].profiles |= static_cast<uint8_t>(1u << pi);
        if (entry) c->sensors[slot].entry |= static_cast<uint8_t>(1u << pi);
    }
    return true;
}

bool parse_profile_outputs(JsonObjectConst po, uint8_t pi, AlarmConfig* c, char* err, size_t cap) {
    char prefix[16];
    std::snprintf(prefix, sizeof(prefix), "profiles[%u].", static_cast<unsigned>(pi));
    JsonArrayConst arr;
    if (!list_field(po, prefix, "outputs", kAlarmMaxProfileOutputs, &arr, err, cap)) return false;
    const uint8_t first = c->n_outputs;
    unsigned j = 0;
    for (JsonVariantConst item : arr) {
        char where[40];
        std::snprintf(where, sizeof(where), "%soutputs[%u]", prefix, j++);
        if (!parse_output(item.as<JsonObjectConst>(), where, false, pi, first, c, err, cap)) return false;
    }
    return true;
}

bool parse_profiles(JsonObjectConst root, AlarmConfig* c, char* err, size_t cap) {
    JsonArrayConst arr;
    if (!list_field(root, "", "profiles", kAlarmMaxProfiles, &arr, err, cap)) return false;
    for (JsonVariantConst item : arr) {
        const uint8_t pi = c->n_profiles;
        const unsigned i = pi;
        JsonObjectConst o = item.as<JsonObjectConst>();
        AlarmProfile& p = c->profiles[pi];
        p.siren_time = kSirenDefault;
        const char* id = o["id"].is<const char*>() ? o["id"].as<const char*>() : "";
        if (!valid_id(id)) return fail(err, cap, "profiles[%u]: id must be 1-13 of a-z, 0-9 and _, starting with a letter", i);
        if (word_index(id, kReserved) >= 0) return fail(err, cap, "profiles[%u]: id %s is reserved", i, id);
        if (alarm_cfg_profile(*c, id) >= 0) return fail(err, cap, "profiles[%u]: id %s is taken", i, id);
        std::snprintf(p.id, sizeof(p.id), "%s", id);
        // valid_name leaves no null; the target compiler does not see that across the two lookups and refuses the %s.
        const char* name = o["name"].as<const char*>();
        if (!valid_name(o["name"]) || !name) return fail(err, cap, "profiles[%u]: name must be 1-24 bytes of text", i);
        std::snprintf(p.name, sizeof(p.name), "%s", name);
        const int cls = word_index(o["class"].is<const char*>() ? o["class"].as<const char*>() : nullptr, kClasses);
        if (cls < 0) return fail(err, cap, "profiles[%u]: class must be intrusion, fire, water, gas or other", i);
        p.cls = static_cast<AlarmClass>(cls);
        JsonVariantConst rv = o["role"];   // "" is none
        if (!rv.isNull()) {
            const char* r = rv.is<const char*>() ? rv.as<const char*>() : nullptr;
            const int k = word_index(r, kModes);
            if (!r || (*r && (k < 0 || p.cls != AlarmClass::INTRUSION)))
                return fail(err, cap, "profiles[%u]: role is away, home or night, for an intrusion profile only", i);
            if (*r) {
                p.role = static_cast<AlarmRole>(k + 1);
                const int other = alarm_cfg_role(*c, p.role);
                if (other >= 0) return fail(err, cap, "profiles[%u]: role %s is taken by %s", i, r, c->profiles[other].id);
            }
        }
        if (!num_field(o, "arm_delay", 0, kAlarmDelayMax, &p.arm_delay))
            return fail(err, cap, "profiles[%u]: arm_delay must be 0-%u s", i, kAlarmDelayMax);
        if (!num_field(o, "trigger_delay", 0, kAlarmDelayMax, &p.trigger_delay))
            return fail(err, cap, "profiles[%u]: trigger_delay must be 0-%u s", i, kAlarmDelayMax);
        if (!num_field(o, "siren_time", kAlarmSirenMin, kAlarmSirenMax, &p.siren_time))
            return fail(err, cap, "profiles[%u]: siren_time must be %u-%u s", i, kAlarmSirenMin, kAlarmSirenMax);
        if (!bool_field(o, "notify_arm", &p.notify_arm)) return fail(err, cap, "profiles[%u]: notify_arm must be true or false", i);
        if (!parse_profile_sensors(o, pi, c, err, cap) || !parse_profile_outputs(o, pi, c, err, cap)) return false;
        c->n_profiles++;
    }
    return true;
}

// ── Stage 1: sensors with modes, sirens, one delay set. Its messages stay; the result is three profiles. ──

// away, home, night, as a hub with no config has them: only away has an arm delay.
void three_profiles(AlarmConfig* c, uint16_t exit_delay, uint16_t entry_delay, uint16_t siren_time, bool notify_arm) {
    static const char* const kNames[] = {"Away", "Home", "Night"};
    for (uint8_t i = 0; i < 3; i++) {
        AlarmProfile& p = c->profiles[i];
        std::snprintf(p.id, sizeof(p.id), "%s", kModes[i]);
        std::snprintf(p.name, sizeof(p.name), "%s", kNames[i]);
        p.cls = AlarmClass::INTRUSION;
        p.role = static_cast<AlarmRole>(i + 1);
        p.arm_delay = i ? 0 : exit_delay;
        p.trigger_delay = entry_delay;
        p.siren_time = siren_time;
        p.notify_arm = notify_arm;
    }
    c->n_profiles = 3;
}

// A sensor's modes are its profiles (away, home, night are profiles 0, 1, 2); `entry` delays it in all of them.
bool parse_sensors_v1(JsonObjectConst root, AlarmConfig* c, char* err, size_t cap) {
    JsonArrayConst arr;
    if (!list_field(root, "", "sensors", kAlarmMaxSensors, &arr, err, cap)) return false;
    for (JsonVariantConst item : arr) {
        const unsigned i = c->n_sensors;
        JsonObjectConst o = item.as<JsonObjectConst>();
        AlarmSensor s{};
        if (!parse_ieee(o["ieee"], &s.ieee)) return fail(err, cap, "sensors[%u]: ieee must be 0x and up to 16 hex digits", i);
        if (alarm_cfg_sensor(*c, s.ieee) >= 0) {
            char id[19];
            ieee_str(s.ieee, id);
            return fail(err, cap, "sensors[%u]: %s is listed twice", i, id);
        }
        JsonVariantConst modes = o["modes"];
        if (!modes.is<JsonArrayConst>()) return fail(err, cap, "sensors[%u]: modes must be a list", i);
        for (JsonVariantConst m : modes.as<JsonArrayConst>()) {
            const int k = word_index(m.is<const char*>() ? m.as<const char*>() : nullptr, kModes);
            if (k < 0) return fail(err, cap, "sensors[%u]: mode must be away, home or night", i);
            s.profiles |= static_cast<uint8_t>(1u << k);
        }
        if (!s.profiles) return fail(err, cap, "sensors[%u]: no mode, so it never guards", i);
        bool entry = false;
        if (!bool_field(o, "entry", &entry)) return fail(err, cap, "sensors[%u]: entry must be true or false", i);
        if (entry) s.entry = s.profiles;
        c->sensors[c->n_sensors++] = s;
    }
    return true;
}

// Each siren sounds in all three profiles: away's group first, then home's, then night's.
bool parse_sirens_v1(JsonObjectConst root, AlarmConfig* c, char* err, size_t cap) {
    JsonArrayConst arr;
    if (!list_field(root, "", "sirens", kAlarmMaxProfileOutputs, &arr, err, cap)) return false;
    for (JsonVariantConst item : arr) {
        char where[16];
        std::snprintf(where, sizeof(where), "sirens[%u]", static_cast<unsigned>(c->n_outputs));
        if (!parse_output(item.as<JsonObjectConst>(), where, true, 0, 0, c, err, cap)) return false;
    }
    const uint8_t n = c->n_outputs;
    for (uint8_t p = 1; p < 3; p++) {
        for (uint8_t k = 0; k < n; k++) {
            c->outputs[p * n + k] = c->outputs[k];
            c->outputs[p * n + k].profile = p;
        }
    }
    c->n_outputs = static_cast<uint8_t>(3 * n);
    return true;
}

bool parse_stage1(JsonObjectConst root, AlarmConfig* c, char* err, size_t cap) {
    uint16_t exit_delay = kExitDefault, entry_delay = kEntryDefault, siren_time = kSirenDefault;
    bool notify_arm = false;
    if (!num_field(root, "exit_delay", 0, kAlarmDelayMax, &exit_delay)) return fail(err, cap, "exit_delay must be 0-%u s", kAlarmDelayMax);
    if (!num_field(root, "entry_delay", 0, kAlarmDelayMax, &entry_delay)) return fail(err, cap, "entry_delay must be 0-%u s", kAlarmDelayMax);
    if (!num_field(root, "siren_time", kAlarmSirenMin, kAlarmSirenMax, &siren_time))
        return fail(err, cap, "siren_time must be %u-%u s", kAlarmSirenMin, kAlarmSirenMax);
    if (!bool_field(root, "notify_arm", &notify_arm)) return fail(err, cap, "notify_arm must be true or false");
    three_profiles(c, exit_delay, entry_delay, siren_time, notify_arm);
    return parse_sensors_v1(root, c, err, cap) && parse_sirens_v1(root, c, err, cap);
}

bool parse_keypads(JsonObjectConst root, AlarmConfig* c, char* err, size_t cap) {
    JsonArrayConst arr;
    if (!list_field(root, "", "keypads", kAlarmMaxKeypads, &arr, err, cap)) return false;
    for (JsonVariantConst item : arr) {
        const unsigned i = c->n_keypads;
        JsonObjectConst o = item.as<JsonObjectConst>();
        AlarmKeypad k{};
        if (!parse_ieee(o["ieee"], &k.ieee)) return fail(err, cap, "keypads[%u]: ieee must be 0x and up to 16 hex digits", i);
        if (alarm_cfg_keypad(*c, k.ieee)) return fail(err, cap, "keypads[%u]: listed twice", i);
        if (!bool_field(o, "trusted", &k.trusted)) return fail(err, cap, "keypads[%u]: trusted must be true or false", i);
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
    if (!list_field(root, "", "codes", kAlarmMaxCodes, &arr, err, cap)) return false;
    for (JsonVariantConst item : arr) {
        const unsigned i = c->n_codes;
        if (!is_code(item)) return fail(err, cap, "codes[%u]: a code is 4-8 digits, as text", i);
        for (uint8_t k = 0; k < c->n_codes; k++)
            if (std::strcmp(c->codes[k], item.as<const char*>()) == 0) return fail(err, cap, "codes[%u]: listed twice", i);
        std::snprintf(c->codes[c->n_codes++], kAlarmCodeMax, "%s", item.as<const char*>());
    }
    return true;
}

bool alerting(AlarmState s) { return s == AlarmState::PENDING || s == AlarmState::TRIGGERED; }

// ── The stored forms ──

// Appends to `buf`. Once something does not fit `ok` stays false and nothing more is written: a record is whole or not at all.
struct Appender {
    char*  buf;
    size_t cap;
    size_t n = 0;
    bool   ok = true;

    __attribute__((format(printf, 2, 3))) void f(const char* fmt, ...) {
        if (!ok) return;
        va_list ap;
        va_start(ap, fmt);
        const int w = std::vsnprintf(buf + n, cap - n, fmt, ap);
        va_end(ap);
        if (w < 0 || static_cast<size_t>(w) >= cap - n) ok = false;
        else n += static_cast<size_t>(w);
    }
    // A JSON string: `"` and `\` escaped, control characters as \u00XX, other bytes (UTF-8) as they are.
    void str(const char* s) {
        f("\"");
        for (; *s; s++) {
            const unsigned char ch = static_cast<unsigned char>(*s);
            if (ch == '"' || ch == '\\') f("\\%c", ch);
            else if (ch < 0x20) f("\\u%04x", static_cast<unsigned>(ch));
            else f("%c", ch);
        }
        f("\"");
    }
    void ieee(uint64_t v) { f("\"%016" PRIX64 "\"", v); }
    size_t length() const { return ok ? n : 0; }
};

// Row item `i`: a whole number in [0, hi].
bool row_num(JsonArrayConst row, size_t i, long hi, unsigned* out) {
    JsonVariantConst v = row[i];
    if (!v.is<long>() || v.as<long>() < 0 || v.as<long>() > hi) return false;
    *out = static_cast<unsigned>(v.as<long>());
    return true;
}

// Row item `i`: a string that fits `cap` with its NUL.
bool row_str(JsonArrayConst row, size_t i, char* out, size_t cap) {
    JsonVariantConst v = row[i];
    if (!v.is<const char*>() || std::strlen(v.as<const char*>()) >= cap) return false;
    std::snprintf(out, cap, "%s", v.as<const char*>());
    return true;
}

// Row item `i`: an ieee as exactly 16 hex digits.
bool row_ieee(JsonArrayConst row, size_t i, uint64_t* out) {
    JsonVariantConst v = row[i];
    const char* s = v.is<const char*>() ? v.as<const char*>() : nullptr;
    if (!s || std::strlen(s) != 16) return false;
    uint64_t x = 0;
    for (; *s; s++) {
        const int d = hex_digit(*s);
        if (d < 0) return false;
        x = x << 4 | static_cast<uint64_t>(d);
    }
    *out = x;
    return true;
}

// The rows under `key`: absent is none; not a list, or more than `max`, is damage.
bool rows(JsonObjectConst root, const char* key, size_t max, JsonArrayConst* out) {
    char unused[8];
    return list_field(root, "", key, max, out, unused, sizeof(unused));
}

// Stage 1's number for an armed profile: 2 away, 3 home, 4 night. By id, else by role, else away.
uint8_t legacy_mode(const AlarmProfile& p) {
    const int k = word_index(p.id, kModes);
    if (k >= 0) return static_cast<uint8_t>(k + 2);
    return p.role == AlarmRole::NONE ? 2 : static_cast<uint8_t>(static_cast<uint8_t>(p.role) + 1);
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

static bool cfg_from_json(const char* json, size_t len, const AlarmConfig* keep, AlarmConfig* out, char* err, size_t cap,
                          ArduinoJson::Allocator* pool, bool stage1) {
    err[0] = '\0';
    JsonDocument doc = pool ? JsonDocument(pool) : JsonDocument();
    if (!json || deserializeJson(doc, json, len) || !doc.is<JsonObjectConst>()) return fail(err, cap, "the config must be a JSON object");
    JsonObjectConst root = doc.as<JsonObjectConst>();
    // Before `out` is touched: a body without its profiles changes nothing.
    if (!stage1 && !root["profiles"].is<JsonArrayConst>()) return fail(err, cap, "profiles must be a list");
    // The defaults, in place: a config is ~4.4 KB, too much for a boot-time stack.
    AlarmConfig& c = *new (out) AlarmConfig();
    return (root["profiles"].isNull() ? parse_stage1(root, &c, err, cap) : parse_profiles(root, &c, err, cap)) &&
           parse_keypads(root, &c, err, cap) && parse_codes(root, keep, &c, err, cap);
}

bool alarm_cfg_from_json(const char* json, size_t len, const AlarmConfig* keep, AlarmConfig* out, char* err, size_t cap,
                         ArduinoJson::Allocator* pool) {
    return cfg_from_json(json, len, keep, out, err, cap, pool, false);
}

bool alarm_cfg_from_stage1(const char* json, size_t len, AlarmConfig* out, char* err, size_t cap, ArduinoJson::Allocator* pool) {
    return cfg_from_json(json, len, nullptr, out, err, cap, pool, true);
}

void alarm_cfg_write(const AlarmConfig& c, bool with_codes, JsonObject doc) {
    char id[19];
    JsonArray profiles = doc["profiles"].to<JsonArray>();
    for (uint8_t i = 0; i < c.n_profiles; i++) {
        const AlarmProfile& p = c.profiles[i];
        JsonObject o = profiles.add<JsonObject>();
        o["id"] = static_cast<const char*>(p.id);   // a copy: the array would be kept by pointer
        o["name"] = static_cast<const char*>(p.name);
        o["class"] = kClasses[static_cast<size_t>(p.cls)];
        if (p.cls == AlarmClass::INTRUSION) o["role"] = p.role == AlarmRole::NONE ? "" : kModes[static_cast<size_t>(p.role) - 1];
        o["arm_delay"] = p.arm_delay;
        o["trigger_delay"] = p.trigger_delay;
        o["siren_time"] = p.siren_time;
        o["notify_arm"] = p.notify_arm;
        JsonArray sensors = o["sensors"].to<JsonArray>();
        for (uint8_t k = 0; k < c.n_sensors; k++) {
            if (!(c.sensors[k].profiles >> i & 1)) continue;
            JsonObject s = sensors.add<JsonObject>();
            ieee_str(c.sensors[k].ieee, id);
            s["ieee"] = static_cast<const char*>(id);   // a copy: id is reused
            s["entry"] = static_cast<bool>(c.sensors[k].entry >> i & 1);
        }
        JsonArray outputs = o["outputs"].to<JsonArray>();
        for (uint8_t k = 0; k < c.n_outputs; k++) {
            const AlarmOutput& out = c.outputs[k];
            if (out.profile != i) continue;
            JsonObject s = outputs.add<JsonObject>();
            ieee_str(out.ieee, id);
            s["ieee"] = static_cast<const char*>(id);
            s["kind"] = kKinds[static_cast<size_t>(out.kind)];
            if (out.kind == AlarmOutKind::WARNING) {
                s["mode"] = static_cast<const char*>(out.wd_mode);
                s["level"] = static_cast<const char*>(out.wd_level);
                s["strobe"] = out.strobe;
            } else if (out.kind == AlarmOutKind::ALARM) {
                if (out.melody[0]) s["melody"] = static_cast<const char*>(out.melody);
                if (out.volume[0]) s["volume"] = static_cast<const char*>(out.volume);
            } else {
                s["value"] = kOnOff[out.value ? 0 : 1];
                s["keep"] = out.keep;
            }
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

void alarm_cfg_defaults(AlarmConfig* out) { three_profiles(new (out) AlarmConfig(), kExitDefault, kEntryDefault, kSirenDefault, false); }

// ── Flash forms ──

size_t alarm_cfg_pack(const AlarmConfig& c, char* buf, size_t cap) {
    Appender w{buf, cap};
    w.f("{\"v\":2,\"p\":[");
    for (uint8_t i = 0; i < c.n_profiles; i++) {
        const AlarmProfile& p = c.profiles[i];
        w.f("%s[", i ? "," : "");
        w.str(p.id);
        w.f(",");
        w.str(p.name);
        w.f(",%u,%u,%u,%u,%u,%u]", static_cast<unsigned>(p.cls), static_cast<unsigned>(p.role), static_cast<unsigned>(p.arm_delay),
            static_cast<unsigned>(p.trigger_delay), static_cast<unsigned>(p.siren_time), static_cast<unsigned>(p.notify_arm));
    }
    w.f("],\"s\":[");
    for (uint8_t i = 0; i < c.n_sensors; i++) {
        w.f("%s[", i ? "," : "");
        w.ieee(c.sensors[i].ieee);
        w.f(",%u,%u]", static_cast<unsigned>(c.sensors[i].profiles), static_cast<unsigned>(c.sensors[i].entry));
    }
    w.f("],\"o\":[");
    for (uint8_t i = 0; i < c.n_outputs; i++) {
        const AlarmOutput& o = c.outputs[i];
        const char* text1 = "";
        const char* text2 = "";
        bool flag1 = false, flag2 = false;
        if (o.kind == AlarmOutKind::WARNING)    { text1 = o.wd_mode; text2 = o.wd_level; flag1 = o.strobe; }
        else if (o.kind == AlarmOutKind::ALARM) { text1 = o.melody;  text2 = o.volume; }
        else                                    { flag1 = o.value;   flag2 = o.keep; }
        w.f("%s[%u,", i ? "," : "", static_cast<unsigned>(o.profile));
        w.ieee(o.ieee);
        w.f(",%u,", static_cast<unsigned>(o.kind));
        w.str(text1);
        w.f(",");
        w.str(text2);
        w.f(",%u,%u]", static_cast<unsigned>(flag1), static_cast<unsigned>(flag2));
    }
    w.f("],\"k\":[");
    for (uint8_t i = 0; i < c.n_keypads; i++) {
        w.f("%s[", i ? "," : "");
        w.ieee(c.keypads[i].ieee);
        w.f(",%u]", static_cast<unsigned>(c.keypads[i].trusted));
    }
    w.f("],\"c\":[");
    for (uint8_t i = 0; i < c.n_codes; i++) {
        w.f("%s", i ? "," : "");
        w.str(c.codes[i]);
    }
    w.f("]}");
    return w.length();
}

// Memory safety is checked (counts, indexes, the tables a number points into, text that fits its array); the limits of the
// API (delays, siren time, ids) are not: a record that a later firmware wrote with wider limits must still load.
bool alarm_cfg_unpack(const char* json, size_t len, AlarmConfig* out, ArduinoJson::Allocator* pool) {
    JsonDocument doc = pool ? JsonDocument(pool) : JsonDocument();
    if (!json || deserializeJson(doc, json, len) || !doc.is<JsonObjectConst>()) return false;
    JsonObjectConst root = doc.as<JsonObjectConst>();
    JsonArrayConst ps, ss, os, ks, cs;
    if (!(root["v"] == 2) || !root["p"].is<JsonArrayConst>() || !rows(root, "p", kAlarmMaxProfiles, &ps) ||
        !rows(root, "s", kAlarmMaxSensors, &ss) || !rows(root, "o", kAlarmMaxOutputs, &os) ||
        !rows(root, "k", kAlarmMaxKeypads, &ks) || !rows(root, "c", kAlarmMaxCodes, &cs))
        return false;
    AlarmConfig& c = *new (out) AlarmConfig();
    unsigned cls, role, arm, trig, siren, notify, profiles, entry, kind, flag1, flag2, pi;
    for (JsonVariantConst item : ps) {
        JsonArrayConst r = item.as<JsonArrayConst>();
        AlarmProfile& p = c.profiles[c.n_profiles];
        if (!row_str(r, 0, p.id, sizeof(p.id)) || !row_str(r, 1, p.name, sizeof(p.name)) || !row_num(r, 2, 4, &cls) ||
            !row_num(r, 3, 3, &role) || !row_num(r, 4, UINT16_MAX, &arm) || !row_num(r, 5, UINT16_MAX, &trig) ||
            !row_num(r, 6, UINT16_MAX, &siren) || !row_num(r, 7, 1, &notify))
            return false;
        p.cls = static_cast<AlarmClass>(cls);
        p.role = static_cast<AlarmRole>(role);
        p.arm_delay = static_cast<uint16_t>(arm);
        p.trigger_delay = static_cast<uint16_t>(trig);
        p.siren_time = static_cast<uint16_t>(siren);
        p.notify_arm = notify;
        c.n_profiles++;
    }
    for (JsonVariantConst item : ss) {
        JsonArrayConst r = item.as<JsonArrayConst>();
        AlarmSensor& s = c.sensors[c.n_sensors];
        if (!row_ieee(r, 0, &s.ieee) || !row_num(r, 1, UINT8_MAX, &profiles) || !row_num(r, 2, UINT8_MAX, &entry)) return false;
        s.profiles = static_cast<uint8_t>(profiles);
        s.entry = static_cast<uint8_t>(entry);
        c.n_sensors++;
    }
    for (JsonVariantConst item : os) {
        JsonArrayConst r = item.as<JsonArrayConst>();
        AlarmOutput& o = c.outputs[c.n_outputs];
        if (!row_num(r, 0, UINT8_MAX, &pi) || pi >= c.n_profiles || !row_ieee(r, 1, &o.ieee) || !row_num(r, 2, 2, &kind) ||
            !row_num(r, 5, 1, &flag1) || !row_num(r, 6, 1, &flag2))
            return false;
        o.profile = static_cast<uint8_t>(pi);
        o.kind = static_cast<AlarmOutKind>(kind);
        if (o.kind == AlarmOutKind::WARNING) {
            if (!row_str(r, 3, o.wd_mode, sizeof(o.wd_mode)) || !row_str(r, 4, o.wd_level, sizeof(o.wd_level))) return false;
            o.strobe = flag1;
        } else if (o.kind == AlarmOutKind::ALARM) {
            if (!row_str(r, 3, o.melody, sizeof(o.melody)) || !row_str(r, 4, o.volume, sizeof(o.volume))) return false;
        } else {
            o.value = flag1;
            o.keep = flag2;
        }
        c.n_outputs++;
    }
    for (JsonVariantConst item : ks) {
        JsonArrayConst r = item.as<JsonArrayConst>();
        AlarmKeypad& k = c.keypads[c.n_keypads];
        if (!row_ieee(r, 0, &k.ieee) || !row_num(r, 1, 1, &flag1)) return false;
        k.trusted = flag1;
        c.n_keypads++;
    }
    for (JsonVariantConst item : cs) {
        if (!is_code(item)) return false;
        std::snprintf(c.codes[c.n_codes++], kAlarmCodeMax, "%s", item.as<const char*>());
    }
    return true;
}

size_t alarm_states_pack(const AlarmConfig& c, const AlarmPanel& p, char* buf, size_t cap, const uint8_t* st) {
    AlarmState state[kAlarmMaxProfiles];
    bool rests[kAlarmMaxProfiles];
    for (uint8_t i = 0; i < c.n_profiles; i++) {
        state[i] = p.at(i).state;
        rests[i] = p.rests_armed(i);
    }
    return alarm_states_pack(c.profiles, state, rests, c.n_profiles, buf, cap, st);
}

size_t alarm_states_pack(const AlarmProfile* prof, const AlarmState* state, const bool* rests, uint8_t n, char* buf, size_t cap,
                         const uint8_t* st) {
    Appender w{buf, cap};
    w.f("{\"v\":1,");
    if (st) w.f("\"l\":[%u,%u],", static_cast<unsigned>(st[0]), static_cast<unsigned>(st[1]));
    w.f("\"p\":[");
    for (uint8_t i = 0; i < n; i++) {
        w.f("%s[", i ? "," : "");
        w.str(prof[i].id);
        w.f(",%u,%u]", static_cast<unsigned>(state[i]), static_cast<unsigned>(rests[i]));
    }
    w.f("]}");
    return w.length();
}

bool alarm_states_unpack(const char* json, size_t len, const AlarmConfig& c, AlarmPanel* p, uint8_t* in_alarm, uint8_t* sounding) {
    *in_alarm = *sounding = 0;
    JsonDocument doc;
    if (!json || deserializeJson(doc, json, len) || !(doc["v"] == 1) || !doc["p"].is<JsonArrayConst>()) return false;
    // Read it all first: a record damaged half way restores nothing.
    constexpr uint8_t kNamed = 0x80;   // set in saved[i]: the record names profile i
    uint8_t saved[kAlarmMaxProfiles] = {};   // kNamed | state, bit 3: rests armed
    for (JsonVariantConst item : doc["p"].as<JsonArrayConst>()) {
        JsonArrayConst r = item.as<JsonArrayConst>();
        unsigned state, rests;
        if (!r[0].is<const char*>() || !row_num(r, 1, static_cast<long>(AlarmState::TRIGGERED), &state) || !row_num(r, 2, 1, &rests)) return false;
        const int i = alarm_cfg_profile(c, r[0].as<const char*>());
        if (i >= 0) saved[i] = static_cast<uint8_t>(kNamed | state | rests << 3);
    }
    for (uint8_t i = 0; i < c.n_profiles; i++) {
        if (!(saved[i] & kNamed)) continue;
        const AlarmState state = static_cast<AlarmState>(saved[i] & 7);
        p->restore(i, saved[i] >> 3 & 1);
        if (alerting(state)) *in_alarm |= static_cast<uint8_t>(1u << i);
        if (state == AlarmState::TRIGGERED) *sounding |= static_cast<uint8_t>(1u << i);
    }
    return true;
}

void alarm_legacy_record(const AlarmConfig& c, const AlarmPanel& p, uint8_t rec[2]) {
    rec[0] = rec[1] = 0;
    const int i = p.summary_profile();
    if (i < 0) return;
    const uint8_t mode = legacy_mode(c.profiles[i]);
    switch (p.at(static_cast<uint8_t>(i)).state) {
    case AlarmState::DISARMED:  break;   // summary_profile is never one
    case AlarmState::ARMING:    rec[0] = 1; break;
    case AlarmState::ARMED:     rec[0] = mode; break;
    case AlarmState::PENDING:   rec[0] = 5; break;
    case AlarmState::TRIGGERED: rec[0] = 6; break;
    }
    rec[1] = p.rests_armed(static_cast<uint8_t>(i)) ? mode : 0;
}

bool alarm_legacy_restore(const uint8_t rec[2], const AlarmConfig& c, AlarmPanel* p, uint8_t* in_alarm, uint8_t* sounding) {
    *in_alarm = *sounding = 0;
    if (rec[0] > 6 || rec[1] > 6) return false;   // the bound stage 1 itself read with
    const bool alarmed = rec[0] >= 5;
    int i = -1;
    if (rec[1] >= 2 && rec[1] <= 4) {   // rests armed, in that mode: the profile by id, else by role
        i = alarm_cfg_profile(c, kModes[rec[1] - 2]);
        if (i < 0 || c.profiles[i].cls != AlarmClass::INTRUSION) i = alarm_cfg_role(c, static_cast<AlarmRole>(rec[1] - 1));
        if (i >= 0) p->restore(static_cast<uint8_t>(i), true);
    } else if (alarmed) {   // a panic from disarmed names no profile: the first intrusion profile was in alarm
        for (uint8_t k = 0; i < 0 && k < c.n_profiles; k++) if (c.profiles[k].cls == AlarmClass::INTRUSION) i = k;
    }
    if (i >= 0 && alarmed) {
        *in_alarm = static_cast<uint8_t>(1u << i);
        if (rec[0] == 6) *sounding = *in_alarm;
    }
    return true;
}

AlarmRestored alarm_states_restore(const char* st2, size_t len, const uint8_t* st, const AlarmConfig& c, AlarmPanel* p,
                                   uint8_t* in_alarm, uint8_t* sounding) {
    *in_alarm = *sounding = 0;
    if (st && (st[0] > 6 || st[1] > 6)) st = nullptr;   // a stage 1 record that does not read is no record
    if (!len || !alarm_states_unpack(st2, len, c, p, in_alarm, sounding))
        return st && alarm_legacy_restore(st, c, p, in_alarm, sounding) ? AlarmRestored::OLD : AlarmRestored::NOTHING;
    // Every profile is restored. Is "st" still the one this "st2" was written beside?
    JsonDocument doc;
    unsigned was[2];
    if (!st || deserializeJson(doc, st2, len)) return AlarmRestored::NEW;
    JsonArrayConst l = doc["l"].as<JsonArrayConst>();
    if (!row_num(l, 0, 6, &was[0]) || !row_num(l, 1, 6, &was[1]) || (was[0] == st[0] && was[1] == st[1])) return AlarmRestored::NEW;
    // No: the stage 1 firmware wrote it since. What it says of the intrusion profiles is newer.
    uint8_t intrusion = 0, alarm = 0, sound = 0;
    for (uint8_t i = 0; i < c.n_profiles; i++) {
        if (c.profiles[i].cls != AlarmClass::INTRUSION) continue;
        intrusion |= static_cast<uint8_t>(1u << i);
        p->restore(i, false);
    }
    alarm_legacy_restore(st, c, p, &alarm, &sound);
    *in_alarm = static_cast<uint8_t>((*in_alarm & ~intrusion) | alarm);
    *sounding = static_cast<uint8_t>((*sounding & ~intrusion) | sound);
    return AlarmRestored::MIXED;
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

bool AlarmPanel::remember(uint8_t slot, AlarmKey key, bool active, bool* changed) {
    Last& l = last_[slot];
    *changed = false;
    if (key == AlarmKey::MOTION) return active;   // every report counts, nothing kept
    if (key == AlarmKey::CONTACT) {
        const Contact now = active ? Contact::OPEN : Contact::CLOSED;
        const bool was = l.contact == Contact::OPEN;
        *changed = l.contact != now;
        l.contact = now;
        return active && !was;
    }
    const uint8_t bit = static_cast<uint8_t>(1u << (static_cast<uint8_t>(key) - static_cast<uint8_t>(AlarmKey::WATER_LEAK)));
    const bool was = l.hazards & bit;
    *changed = was != active;
    l.hazards = active ? (l.hazards | bit) : (l.hazards & ~bit);
    return active && !was;
}

void AlarmPanel::seed(uint8_t slot, AlarmKey key, bool active) {
    bool changed;
    if (slot < cfg_->n_sensors) remember(slot, key, active, &changed);
}

bool AlarmPanel::sensor(uint8_t slot, AlarmKey key, bool active, uint32_t now) {
    bool changed = false;
    if (slot >= cfg_->n_sensors) return false;
    if (remember(slot, key, active, &changed))
        for (uint8_t i = 0; i < cfg_->n_profiles; i++) go_off(i, slot, now);
    return changed;
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
    if (alerting(s.state)) return AlarmArmResult::BUSY;
    for (uint8_t i = 0; intrusion(p) && i < cfg_->n_profiles; i++)
        if (i != p && intrusion(i) && alerting(st_[i].state)) return AlarmArmResult::BUSY;
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
    if (p < cfg_->n_profiles && alerting(st_[p].state)) end_alarm(p);
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

void AlarmPanel::adopt(const AlarmPanel& old, const AlarmConfig& old_cfg) {
    int8_t was[kAlarmMaxSensors];   // the slot a sensor had in the old config, -1
    for (uint8_t k = 0; k < cfg_->n_sensors; k++) {
        was[k] = static_cast<int8_t>(alarm_cfg_sensor(old_cfg, cfg_->sensors[k].ieee));
        if (was[k] >= 0) last_[k] = old.last_[was[k]];
    }
    for (uint8_t i = 0; i < cfg_->n_profiles; i++) {
        const int j = alarm_cfg_profile(old_cfg, cfg_->profiles[i].id);
        if (j < 0) continue;
        st_[i] = old.st_[j];
        check_due_[i] = old.check_due_[j];   // a hazard check still owed stays owed
        st_[i].bypass = 0;
        for (uint8_t k = 0; k < cfg_->n_sensors; k++)   // only where the sensor is still in profile i
            if (was[k] >= 0 && (old.st_[j].bypass >> was[k] & 1) && (cfg_->sensors[k].profiles >> i & 1)) st_[i].bypass |= 1ull << k;
    }
}

bool AlarmPanel::rests_armed(uint8_t p) const {
    if (p >= cfg_->n_profiles) return false;
    const AlarmProfileStatus& s = st_[p];
    return s.state == AlarmState::ARMING || s.state == AlarmState::ARMED || (alerting(s.state) && s.rests_armed);
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

// ── The work lists of one pass of the alarm task ──

namespace {

// Which class wins an output device that several triggered profiles list: fire, gas, water, other, intrusion.
constexpr uint8_t kClassRank[] = {4, 0, 2, 1, 3};   // by AlarmClass: intrusion fire water gas other

// The event names of one pass: a name already in the list is not added again; nothing is added past `cap`.
struct EventList {
    char (*out)[kAlarmEventMax];
    uint8_t cap;
    uint8_t n = 0;

    __attribute__((format(printf, 2, 3))) void add(const char* fmt, ...) {
        char name[kAlarmEventMax];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(name, sizeof(name), fmt, ap);
        va_end(ap);
        for (uint8_t i = 0; i < n; i++) if (std::strcmp(out[i], name) == 0) return;
        if (n < cap) std::memcpy(out[n++], name, sizeof(name));
    }
};

}  // namespace

bool alarm_key_parse(const char* key, AlarmKey* out) {
    static constexpr struct { const char* name; AlarmKey key; } kKeys[] = {
        {"contact", AlarmKey::CONTACT},      {"occupancy", AlarmKey::MOTION}, {"presence", AlarmKey::MOTION},
        {"vibration", AlarmKey::MOTION},     {"water_leak", AlarmKey::WATER_LEAK}, {"smoke", AlarmKey::SMOKE},
        {"gas", AlarmKey::GAS},              {"carbon_monoxide", AlarmKey::CO},
    };
    for (const auto& k : kKeys) {
        if (key && std::strcmp(key, k.name) == 0) { *out = k.key; return true; }
    }
    return false;
}

void alarm_summary_name(const AlarmConfig& c, const AlarmPanel& p, char* out, size_t cap) {
    const int i = p.summary_profile();
    if (i < 0) { std::snprintf(out, cap, "disarmed"); return; }
    const AlarmState s = p.at(static_cast<uint8_t>(i)).state;
    if (s == AlarmState::ARMED) std::snprintf(out, cap, "armed_%s", c.profiles[i].id);
    else std::snprintf(out, cap, "%s", alarm_state_name(s));
}

uint8_t alarm_changes(const AlarmPanel& p, const AlarmSeen* seen, AlarmChange* out) {
    uint8_t n = 0;
    for (uint8_t i = 0; i < kAlarmMaxProfiles; i++) {   // slots past the config's profiles never move
        const AlarmProfileStatus& s = p.at(i);
        const bool alarm = s.alarms != seen[i].alarms;
        if (s.state == seen[i].state && !alarm) continue;   // a disarm and an arm in between leave no trace to act on
        out[n++] = {i, seen[i].state, s.state, alarm ? s.alarm_cause : alerting(s.state) ? s.cause : 0, alarm,
                    s.state == AlarmState::DISARMED && s.disarms != seen[i].disarms};
    }
    return n;
}

void alarm_seen(const AlarmPanel& p, AlarmSeen* seen) {
    for (uint8_t i = 0; i < kAlarmMaxProfiles; i++) seen[i] = {p.at(i).state, p.at(i).alarms, p.at(i).disarms};
}

uint8_t alarm_events(const AlarmConfig& c, const AlarmChange* ch, uint8_t n, const char* summary_was, const char* summary_now,
                     char out[][kAlarmEventMax], uint8_t cap) {
    EventList ev{out, cap};
    bool intrusion_alarm = false;
    for (uint8_t i = 0; i < n; i++) {
        if (ch[i].profile >= c.n_profiles) continue;
        const AlarmProfile& pr = c.profiles[ch[i].profile];
        if (ch[i].alarm) {
            ev.add("alarm_triggered_%s", pr.id);
            intrusion_alarm |= pr.cls == AlarmClass::INTRUSION;
        }
        if (ch[i].to != ch[i].from) ev.add("alarm_%s_%s", alarm_state_name(ch[i].to), pr.id);   // the same name as above: once
    }
    if (intrusion_alarm) ev.add("alarm_triggered");
    if (std::strcmp(summary_was, summary_now) != 0) ev.add("alarm_%s", summary_now);
    return ev.n;
}

size_t alarm_text(AlarmText t, const char* profile, const char* device, unsigned more, char* out, size_t cap) {
    if (!cap) return 0;
    if (!profile) profile = "";
    if (!device) device = "";
    out[0] = '\0';
    int w = 0;
    switch (t) {
    case AlarmText::TRIGGERED: w = std::snprintf(out, cap, "%s (%s)", device, profile); break;
    case AlarmText::PANIC:     w = std::snprintf(out, cap, "Panic button on %s", device); break;
    case AlarmText::REFUSED:
        w = more ? std::snprintf(out, cap, "Not armed (%s): %s and %u more are open", profile, device, more)
                 : std::snprintf(out, cap, "Not armed (%s): %s is open", profile, device);
        break;
    case AlarmText::ARMED:     w = std::snprintf(out, cap, "Armed (%s)", profile); break;
    case AlarmText::DISARMED:  w = std::snprintf(out, cap, "Disarmed (%s)", profile); break;
    case AlarmText::RESTARTED: w = std::snprintf(out, cap, "The hub restarted during an alarm"); break;
    }
    if (w < 0) { out[0] = '\0'; return 0; }
    size_t n = static_cast<size_t>(w);
    if (n >= cap) {   // cut: back off to the start of the last character, and drop it unless it is whole
        n = cap - 1;
        const auto* u = reinterpret_cast<const unsigned char*>(out);
        size_t lead = n;
        while (lead > 0) {
            lead--;
            if ((u[lead] & 0xC0) != 0x80 || n - lead >= 4) break;   // a lead byte, or too far back to belong to one
        }
        if (n && utf8_seq_len(u + lead, n - lead) != n - lead) n = lead;
        out[n] = '\0';
    }
    return n;
}

uint8_t alarm_drive_plan(const AlarmConfig& c, const AlarmPanel& p, uint32_t now, AlarmDrive* out) {
    uint8_t n = 0;
    for (uint8_t k = 0; k < c.n_outputs; k++) {
        const AlarmOutput& o = c.outputs[k];
        uint8_t e = 0;
        while (e < n && out[e].ieee != o.ieee) e++;
        if (e == n) out[n++] = {o.ieee, -1, 0};
        if (o.profile >= c.n_profiles || p.at(o.profile).state != AlarmState::TRIGGERED) continue;
        if (out[e].output >= 0) {   // another triggered profile has this device: the higher class, then the lower index, keeps it
            const uint8_t held = c.outputs[out[e].output].profile;
            const uint8_t rank_held = kClassRank[static_cast<size_t>(c.profiles[held].cls)];
            const uint8_t rank_new = kClassRank[static_cast<size_t>(c.profiles[o.profile].cls)];
            if (rank_new > rank_held || (rank_new == rank_held && o.profile > held)) continue;
        }
        const uint32_t end = p.at(o.profile).deadline;
        out[e].output = static_cast<int8_t>(k);
        out[e].secs = end > now ? static_cast<uint16_t>(end - now) : 0;   // never more than the profile's siren time
    }
    return n;
}

AlarmOutAct alarm_output_step(const AlarmConfig& c, int8_t was, int8_t next) {
    const AlarmOutput* w = was >= 0 && was < c.n_outputs ? &c.outputs[was] : nullptr;
    const AlarmOutput* x = next >= 0 && next < c.n_outputs ? &c.outputs[next] : nullptr;
    if (x) {
        if (x == w) return AlarmOutAct::NONE;
        if (w && w->kind == AlarmOutKind::STATE && x->kind == AlarmOutKind::STATE && w->value == x->value) return AlarmOutAct::NONE;
        return AlarmOutAct::START;
    }
    return w && !w->keep ? AlarmOutAct::STOP : AlarmOutAct::NONE;
}
