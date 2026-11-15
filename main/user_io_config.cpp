// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "user_io_config.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>

#include <ArduinoJson.h>

namespace {

constexpr const char* kModes[] = {"out", "pwm", "in", "adc", "strip"};
constexpr const char* kPulls[] = {"none", "up", "down"};
constexpr const char* kBoots[] = {"off", "on", "last"};

template <size_t N>
int word_index(const char* s, const char* const (&words)[N]) {
    if (!s) return -1;
    for (size_t i = 0; i < N; i++) if (std::strcmp(s, words[i]) == 0) return static_cast<int>(i);
    return -1;
}

bool name_ok(const char* s) {
    const size_t n = s ? std::strlen(s) : 0;
    if (n == 0 || n >= kUserIoNameMax || s[0] < 'a' || s[0] > 'z') return false;
    for (size_t i = 0; i < n; i++) {
        const char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    }
    return true;
}

// An optional integer field in [lo, hi]; absent = `def`.
bool int_field(JsonObjectConst o, const char* key, long lo, long hi, long def, long* out) {
    JsonVariantConst v = o[key];
    if (v.isNull()) { *out = def; return true; }
    if (!v.is<long>()) return false;
    const long x = v.as<long>();
    if (x < lo || x > hi) return false;
    *out = x;
    return true;
}

// An optional word field; absent = index 0.
template <size_t N>
bool word_field(JsonObjectConst o, const char* key, const char* const (&words)[N], int* out) {
    JsonVariantConst v = o[key];
    if (v.isNull()) { *out = 0; return true; }
    *out = v.is<const char*>() ? word_index(v.as<const char*>(), words) : -1;
    return *out >= 0;
}

}  // namespace

const char* user_io_mode_name(UserIoMode m) {
    const auto i = static_cast<size_t>(m);
    return i < sizeof(kModes) / sizeof(kModes[0]) ? kModes[i] : "?";
}

bool user_io_cfg_from_json(const char* json, size_t len, const UserIoCaps& caps,
                           UserIoConfig* out, char* err, size_t cap) {
    JsonDocument doc;
    if (!json || deserializeJson(doc, json, len) || !doc["pins"].is<JsonArrayConst>()) {
        std::snprintf(err, cap, "body must be {\"pins\":[...]}");
        return false;
    }
    JsonArrayConst arr = doc["pins"].as<JsonArrayConst>();
    if (arr.size() > kUserIoMaxPins) {
        std::snprintf(err, cap, "too many pins (max %u)", (unsigned)kUserIoMaxPins);
        return false;
    }
    *out = UserIoConfig{};
    unsigned row = 0;
    for (JsonVariantConst item : arr) {
        row++;
        JsonObjectConst o = item.as<JsonObjectConst>();
        if (o.isNull()) { std::snprintf(err, cap, "row %u: not an object", row); return false; }
        UserIoPin p{};

        JsonVariantConst g = o["gpio"];
        if (!g.is<long>() || g.as<long>() < 0 || g.as<long>() > 63) {
            std::snprintf(err, cap, "row %u: gpio must be a number", row);
            return false;
        }
        p.gpio = static_cast<uint8_t>(g.as<long>());
        bool allowed = false;
        for (uint8_t i = 0; i < caps.n_allowed; i++) allowed |= caps.allowed[i] == p.gpio;
        if (!allowed) { std::snprintf(err, cap, "row %u: gpio %u is not allowed on this hub", row, p.gpio); return false; }

        const int mode = o["mode"].is<const char*>() ? word_index(o["mode"].as<const char*>(), kModes) : -1;
        if (mode < 0) { std::snprintf(err, cap, "row %u: mode must be out, pwm, in, adc or strip", row); return false; }
        p.mode = static_cast<UserIoMode>(mode);
        if (p.mode == UserIoMode::ADC && !(caps.adc_mask & (1ULL << p.gpio))) {
            std::snprintf(err, cap, "row %u: gpio %u has no ADC", row, p.gpio);
            return false;
        }

        const char* name = o["name"].is<const char*>() ? o["name"].as<const char*>() : nullptr;
        if (!name_ok(name)) {
            std::snprintf(err, cap, "row %u: name must be a letter, then up to 14 of a-z 0-9 _", row);
            return false;
        }
        std::snprintf(p.name, sizeof(p.name), "%s", name);

        JsonVariantConst inv = o["invert"];
        if (!inv.isNull() && !inv.is<bool>()) { std::snprintf(err, cap, "row %u: invert must be true or false", row); return false; }
        p.invert = inv.as<bool>();

        int w = 0;
        if (!word_field(o, "pull", kPulls, &w)) { std::snprintf(err, cap, "row %u: pull must be none, up or down", row); return false; }
        p.pull = static_cast<UserIoPull>(w);
        if (!word_field(o, "boot", kBoots, &w)) { std::snprintf(err, cap, "row %u: boot must be off, on or last", row); return false; }
        p.boot = static_cast<UserIoBoot>(w);

        long x = 0;
        if (!int_field(o, "freq", 1, 40000, 1000, &x)) { std::snprintf(err, cap, "row %u: freq must be 1-40000", row); return false; }
        p.freq = static_cast<uint32_t>(x);
        if (!int_field(o, "interval", 0, 3600, 5, &x)) { std::snprintf(err, cap, "row %u: interval must be 0-3600", row); return false; }
        p.interval = static_cast<uint16_t>(x);
        if (!int_field(o, "delta", 0, 4095, 50, &x)) { std::snprintf(err, cap, "row %u: delta must be 0-4095", row); return false; }
        p.delta = static_cast<uint16_t>(x);
        if (!int_field(o, "leds", 1, 1024, 30, &x)) { std::snprintf(err, cap, "row %u: leds must be 1-1024", row); return false; }
        p.leds = static_cast<uint16_t>(x);

        for (uint8_t i = 0; i < out->count; i++) {
            if (out->pins[i].gpio == p.gpio) { std::snprintf(err, cap, "row %u: gpio %u is used twice", row, p.gpio); return false; }
            if (std::strcmp(out->pins[i].name, p.name) == 0) {
                std::snprintf(err, cap, "row %u: name '%s' is used twice", row, p.name);
                return false;
            }
        }
        out->pins[out->count++] = p;
    }
    return true;
}

size_t user_io_cfg_pin_json(const UserIoPin& p, char* buf, size_t cap) {
    const int n = std::snprintf(buf, cap,
        "{\"gpio\":%u,\"mode\":\"%s\",\"name\":\"%s\",\"invert\":%s,\"pull\":\"%s\",\"boot\":\"%s\","
        "\"freq\":%lu,\"interval\":%u,\"delta\":%u,\"leds\":%u}",
        p.gpio, user_io_mode_name(p.mode), p.name, p.invert ? "true" : "false",
        kPulls[static_cast<size_t>(p.pull) % 3], kBoots[static_cast<size_t>(p.boot) % 3],
        static_cast<unsigned long>(p.freq), p.interval, p.delta, p.leds);
    return (n > 0 && static_cast<size_t>(n) < cap) ? static_cast<size_t>(n) : 0;
}

int user_io_cfg_find(const UserIoConfig& c, const char* ref) {
    if (!ref || !ref[0]) return -1;
    for (uint8_t i = 0; i < c.count; i++) if (std::strcmp(c.pins[i].name, ref) == 0) return i;
    char* end = nullptr;
    const long g = std::strtol(ref, &end, 10);
    if (*end != '\0' || ref[0] < '0' || ref[0] > '9') return -1;
    for (uint8_t i = 0; i < c.count; i++) if (c.pins[i].gpio == g) return i;
    return -1;
}

bool user_io_parse_level(const char* s, bool* on) {
    if (!s || !s[0]) return false;
    static constexpr const char* kOn[]  = {"on", "true", "high"};
    static constexpr const char* kOff[] = {"off", "false", "low"};
    for (const char* w : kOn)  if (strcasecmp(s, w) == 0) { *on = true;  return true; }
    for (const char* w : kOff) if (strcasecmp(s, w) == 0) { *on = false; return true; }
    char* end = nullptr;
    const double d = std::strtod(s, &end);
    if (end == s || *end != '\0' || d != d) return false;
    *on = d != 0.0;
    return true;
}

uint8_t user_io_parse_pins(const char* csv, uint8_t* out, uint8_t cap) {
    uint8_t n = 0;
    const char* p = csv ? csv : "";
    while (*p && n < cap) {
        while (*p == ' ' || *p == ',') p++;
        if (!*p) break;
        char* end = nullptr;
        const long v = std::strtol(p, &end, 10);
        const char* q = end;
        while (*q == ' ') q++;
        const bool clean = end != p && (*q == ',' || *q == '\0');
        if (clean && v >= 0 && v <= 63) out[n++] = static_cast<uint8_t>(v);
        while (*p && *p != ',') p++;   // next entry (junk entries are skipped whole)
    }
    return n;
}
