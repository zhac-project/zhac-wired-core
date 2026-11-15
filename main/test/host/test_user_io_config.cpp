// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Hub pin configuration (spec extra/docs/superpowers/specs/2026-11-15-gpio-control-design.md §1/§6):
// what PUT /api/io accepts, the defaults it fills in, and every reason it refuses.
#include "user_io_config.h"

#include <cstdio>
#include <cstring>

static int g_fail = 0;
#define CHECK(c, m) do { if (c) std::printf("PASS: %s\n", m); else { std::printf("FAIL: %s\n", m); g_fail++; } } while (0)

static UserIoCaps caps() {
    UserIoCaps c{};
    const uint8_t pins[] = {2, 3, 4, 20, 21, 22, 23};
    for (uint8_t p : pins) c.allowed[c.n_allowed++] = p;
    c.adc_mask = (1ULL << 20) | (1ULL << 21);
    return c;
}

static bool parse(const char* json, UserIoConfig* out, char* err = nullptr) {
    static char sink[160];
    return user_io_cfg_from_json(json, std::strlen(json), caps(), out, err ? err : sink, 160);
}

static bool refuses(const char* json, const char* want) {
    UserIoConfig c{};
    char err[160] = "";
    const bool ok = parse(json, &c, err);
    if (ok || !std::strstr(err, want)) std::printf("  got ok=%d err='%s'\n", ok, err);
    return !ok && std::strstr(err, want) != nullptr;
}

int main() {
    UserIoConfig c{};

    // ── defaults and the round trip through the per-pin JSON ──
    CHECK(parse(R"({"pins":[{"gpio":21,"mode":"out","name":"relay"}]})", &c) && c.count == 1, "minimal out pin parses");
    const UserIoPin& p = c.pins[0];
    CHECK(p.gpio == 21 && p.mode == UserIoMode::OUT && std::strcmp(p.name, "relay") == 0 && !p.invert &&
          p.pull == UserIoPull::NONE && p.boot == UserIoBoot::OFF && p.freq == 1000 && p.interval == 5 &&
          p.delta == 50 && p.leds == 30, "omitted fields take the spec defaults");
    char js[256];
    CHECK(user_io_cfg_pin_json(p, js, sizeof(js)) > 0 &&
          std::strcmp(js, R"({"gpio":21,"mode":"out","name":"relay","invert":false,"pull":"none","boot":"off","freq":1000,"interval":5,"delta":50,"leds":30})") == 0,
          "pin JSON carries every config field");
    UserIoConfig back{};
    char body[300];
    std::snprintf(body, sizeof(body), "{\"pins\":[%s]}", js);
    CHECK(parse(body, &back) && back.count == 1 && std::memcmp(&back.pins[0], &p, sizeof(p)) == 0, "pin JSON parses back to the same pin");

    CHECK(parse(R"({"pins":[
        {"gpio":21,"mode":"pwm","name":"fan","invert":true,"boot":"last","freq":25000},
        {"gpio":4,"mode":"in","name":"button","pull":"up"},
        {"gpio":20,"mode":"adc","name":"pot","interval":0,"delta":4095},
        {"gpio":2,"mode":"strip","name":"leds","leds":1024,"value":7,"err":"x"}]})", &c) && c.count == 4,
          "every mode parses; runtime fields echoed back by a UI are ignored");
    CHECK(c.pins[0].mode == UserIoMode::PWM && c.pins[0].invert && c.pins[0].boot == UserIoBoot::LAST && c.pins[0].freq == 25000 &&
          c.pins[1].pull == UserIoPull::UP && c.pins[2].interval == 0 && c.pins[2].delta == 4095 && c.pins[3].leds == 1024,
          "explicit fields are kept");
    CHECK(parse(R"({"pins":[]})", &c) && c.count == 0, "an empty list clears the config");

    // ── refusals ──
    CHECK(refuses(R"({"pins":{}})", "pins"), "pins must be an array");
    CHECK(refuses(R"(not json)", "pins"), "garbage is refused");
    CHECK(refuses(R"({"pins":[{"gpio":7,"mode":"out","name":"a"}]})", "row 1: gpio 7 is not allowed"), "a pin outside the allowlist");
    CHECK(refuses(R"({"pins":[{"gpio":"21","mode":"out","name":"a"}]})", "row 1: gpio"), "gpio must be a number");
    CHECK(refuses(R"({"pins":[{"gpio":21,"mode":"out","name":"a"},{"gpio":21,"mode":"in","name":"b"}]})",
                  "row 2: gpio 21 is used twice"), "duplicate gpio");
    CHECK(refuses(R"({"pins":[{"gpio":21,"mode":"out","name":"a"},{"gpio":22,"mode":"in","name":"a"}]})",
                  "row 2: name 'a' is used twice"), "duplicate name");
    CHECK(refuses(R"({"pins":[{"gpio":21,"mode":"out","name":"Relay"}]})", "row 1: name") &&
          refuses(R"({"pins":[{"gpio":21,"mode":"out","name":""}]})", "row 1: name") &&
          refuses(R"({"pins":[{"gpio":21,"mode":"out","name":"abcdefghijklmnop"}]})", "row 1: name") &&
          refuses(R"({"pins":[{"gpio":21,"mode":"out","name":"9a"}]})", "row 1: name") &&
          refuses(R"({"pins":[{"gpio":21,"mode":"out"}]})", "row 1: name"), "name: a letter, then up to 14 of a-z 0-9 _");
    CHECK(refuses(R"({"pins":[{"gpio":21,"mode":"blink","name":"a"}]})", "row 1: mode"), "unknown mode");
    CHECK(refuses(R"({"pins":[{"gpio":22,"mode":"adc","name":"a"}]})", "row 1: gpio 22 has no ADC"), "adc on a non-ADC pin");
    CHECK(refuses(R"({"pins":[{"gpio":21,"mode":"pwm","name":"a","freq":0}]})", "row 1: freq") &&
          refuses(R"({"pins":[{"gpio":21,"mode":"pwm","name":"a","freq":40001}]})", "row 1: freq"), "freq 1-40000");
    CHECK(refuses(R"({"pins":[{"gpio":21,"mode":"strip","name":"a","leds":0}]})", "row 1: leds") &&
          refuses(R"({"pins":[{"gpio":21,"mode":"strip","name":"a","leds":1025}]})", "row 1: leds"), "leds 1-1024");
    CHECK(refuses(R"({"pins":[{"gpio":20,"mode":"adc","name":"a","interval":3601}]})", "row 1: interval") &&
          refuses(R"({"pins":[{"gpio":20,"mode":"adc","name":"a","delta":4096}]})", "row 1: delta"), "interval 0-3600, delta 0-4095");
    CHECK(refuses(R"({"pins":[{"gpio":21,"mode":"in","name":"a","pull":"sideways"}]})", "row 1: pull") &&
          refuses(R"({"pins":[{"gpio":21,"mode":"out","name":"a","boot":"maybe"}]})", "row 1: boot") &&
          refuses(R"({"pins":[{"gpio":21,"mode":"out","name":"a","invert":1}]})", "row 1: invert"), "pull, boot, invert values");
    {
        char many[2048] = "{\"pins\":[";
        for (int i = 0; i < 17; i++) {
            char one[80];
            std::snprintf(one, sizeof(one), "%s{\"gpio\":%d,\"mode\":\"out\",\"name\":\"p%d\"}", i ? "," : "", 2, i);
            std::strcat(many, one);
        }
        std::strcat(many, "]}");
        CHECK(refuses(many, "too many pins (max 16)"), "more than 16 pins");
    }

    // ── boot: rows this build no longer allows are dropped, the rest kept ──
    {
        const char* stored = R"({"pins":[{"gpio":7,"mode":"out","name":"gone"},{"gpio":21,"mode":"out","name":"relay"},)"
                             R"({"gpio":22,"mode":"adc","name":"lvl"},{"gpio":4,"mode":"in","name":"btn"}]})";
        char err[160] = "";
        UserIoConfig b{};
        CHECK(user_io_cfg_from_json(stored, std::strlen(stored), caps(), &b, err, sizeof(err), true) && b.count == 2 &&
              std::strcmp(b.pins[0].name, "relay") == 0 && std::strcmp(b.pins[1].name, "btn") == 0 &&
              std::strstr(err, "row 1: gpio 7 is not allowed") != nullptr,
              "skip_bad_rows keeps the valid rows and names the first dropped one");
        CHECK(!parse(stored, &b), "strict (PUT) still refuses the whole body");
        CHECK(!user_io_cfg_from_json("garbage", 7, caps(), &b, err, sizeof(err), true), "skip_bad_rows still refuses a broken body");
        err[0] = 'x';
        CHECK(user_io_cfg_from_json(R"({"pins":[{"gpio":21,"mode":"out","name":"relay"}]})", 51, caps(), &b, err, sizeof(err), true) &&
              b.count == 1 && err[0] == '\0', "nothing dropped: err is empty");
    }

    // ── a save leaves pins whose hardware did not change alone ──
    {
        UserIoPin a{};
        a.gpio = 21; a.mode = UserIoMode::PWM; a.freq = 1000; a.leds = 30; a.interval = 5; a.delta = 50;
        std::snprintf(a.name, sizeof(a.name), "fan");
        UserIoPin b = a;
        std::snprintf(b.name, sizeof(b.name), "cooler");
        b.boot = UserIoBoot::LAST; b.interval = 9; b.delta = 7;
        CHECK(user_io_pin_same_hw(a, b), "name, boot and adc publishing are not hardware");
        b = a; b.freq = 2000;              CHECK(!user_io_pin_same_hw(a, b), "freq is hardware");
        b = a; b.invert = true;            CHECK(!user_io_pin_same_hw(a, b), "invert is hardware");
        b = a; b.gpio = 22;                CHECK(!user_io_pin_same_hw(a, b), "gpio is hardware");
        b = a; b.mode = UserIoMode::OUT;   CHECK(!user_io_pin_same_hw(a, b), "mode is hardware");
        b = a; b.pull = UserIoPull::UP;    CHECK(!user_io_pin_same_hw(a, b), "pull is hardware");
        b = a; b.leds = 60;                CHECK(!user_io_pin_same_hw(a, b), "strip length is hardware");
    }

    // ── lookups the rules and Lua use ──
    parse(R"({"pins":[{"gpio":21,"mode":"out","name":"relay"},{"gpio":4,"mode":"in","name":"b4"}]})", &c);
    CHECK(user_io_cfg_find(c, "relay") == 0 && user_io_cfg_find(c, "21") == 0 && user_io_cfg_find(c, "4") == 1 &&
          user_io_cfg_find(c, "b4") == 1, "find by name or by GPIO number");
    CHECK(user_io_cfg_find(c, "22") == -1 && user_io_cfg_find(c, "lamp") == -1 && user_io_cfg_find(c, "") == -1 &&
          user_io_cfg_find(c, "21x") == -1, "unknown refs are not found");

    bool on = false;
    CHECK(user_io_parse_level("ON", &on) && on && user_io_parse_level("high", &on) && on && user_io_parse_level("True", &on) && on &&
          user_io_parse_level("1", &on) && on && user_io_parse_level("255", &on) && on && user_io_parse_level("0.5", &on) && on,
          "on words and non-zero numbers are on");
    CHECK(user_io_parse_level("off", &on) && !on && user_io_parse_level("LOW", &on) && !on && user_io_parse_level("false", &on) && !on &&
          user_io_parse_level("0", &on) && !on, "off words and zero are off");
    CHECK(!user_io_parse_level("", &on) && !user_io_parse_level("maybe", &on) && !user_io_parse_level(nullptr, &on), "anything else is refused");

    uint8_t list[64];
    CHECK(user_io_parse_pins("2, 3,4,,20 ,x,99,53", list, 64) == 5 && list[0] == 2 && list[1] == 3 && list[2] == 4 &&
          list[3] == 20 && list[4] == 53, "Kconfig pin list: numbers 0-63, junk skipped");

    if (g_fail) { std::printf("%d check(s) failed\n", g_fail); return 1; }
    std::printf("user_io config: all checks passed\n");
    return 0;
}
