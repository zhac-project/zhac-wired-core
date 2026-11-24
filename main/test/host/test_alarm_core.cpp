// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Alarm panel (spec extra/docs/superpowers/specs/2026-11-24-security-mode-design.md):
// every state transition, the delays on a fake clock, the arming check and force,
// the bypass, panic, restore, the keypad code policy and the config JSON.
#include "alarm_core.h"

#include <cstdio>
#include <cstring>

static int g_fail = 0;
#define CHECK(c, m) do { if (c) std::printf("PASS: %s\n", m); else { std::printf("FAIL: %s\n", m); g_fail++; } } while (0)

constexpr uint64_t kDoor   = 0x00158D0000000001ULL;   // entry contact, every mode
constexpr uint64_t kWindow = 0x00158D0000000002ULL;   // contact, every mode
constexpr uint64_t kHall   = 0x00158D0000000003ULL;   // motion, away only
constexpr uint64_t kShock  = 0x00158D0000000004ULL;   // vibration, away + night
constexpr uint64_t kStray  = 0x00158D00000000FFULL;   // not configured
constexpr uint64_t kPad    = 0x00158D0000000010ULL;
constexpr uint64_t kFob    = 0x00158D0000000011ULL;

constexpr uint8_t kAll = 0x07, kAway = 0x01, kNight = 0x04;

static AlarmConfig config() {
    AlarmConfig c{};
    c.sensors[c.n_sensors++] = {kDoor, kAll, true};
    c.sensors[c.n_sensors++] = {kWindow, kAll, false};
    c.sensors[c.n_sensors++] = {kHall, kAway, false};
    c.sensors[c.n_sensors++] = {kShock, static_cast<uint8_t>(kAway | kNight), false};
    c.keypads[c.n_keypads++] = {kPad, false};
    c.keypads[c.n_keypads++] = {kFob, true};
    return c;
}

static void transitions() {
    AlarmConfig c = config();
    AlarmCore a(&c);
    CHECK(a.state() == AlarmState::DISARMED, "starts disarmed");

    // Home and night arm at once; away waits for the exit delay.
    CHECK(a.arm(AlarmMode::HOME, false, nullptr, 0, 100) == AlarmArmResult::OK && a.state() == AlarmState::ARMED_HOME,
          "arm home: armed at once");
    a.disarm();
    CHECK(a.state() == AlarmState::DISARMED, "disarm");
    CHECK(a.arm(AlarmMode::AWAY, false, nullptr, 0, 100) == AlarmArmResult::OK && a.state() == AlarmState::ARMING &&
          a.mode() == AlarmMode::AWAY && a.deadline() == 130, "arm away: arming until the exit delay ends");
    a.sensor_active(kHall, 110);
    CHECK(a.state() == AlarmState::ARMING, "sensors are ignored while arming (people are leaving)");
    a.tick(129);
    CHECK(a.state() == AlarmState::ARMING, "still arming a second before the end");
    a.tick(130);
    CHECK(a.state() == AlarmState::ARMED_AWAY, "armed away when the exit delay ends");

    // A non-entry sensor: the alarm at once, for the siren time.
    a.sensor_active(kWindow, 200);
    CHECK(a.state() == AlarmState::TRIGGERED && a.cause() == kWindow && a.deadline() == 380,
          "a window: triggered at once, for the siren time");
    CHECK(a.in_alarm() && a.resting() == AlarmState::ARMED_AWAY, "triggered rests at armed away");
    a.sensor_active(kHall, 210);
    CHECK(a.cause() == kWindow && a.deadline() == 380, "more sensors while triggered change nothing");
    a.tick(380);
    CHECK(a.state() == AlarmState::ARMED_AWAY && a.cause() == 0, "the siren time ends: back to armed away");

    // An entry sensor: the entry delay first.
    a.sensor_active(kDoor, 400);
    CHECK(a.state() == AlarmState::PENDING && a.cause() == kDoor && a.deadline() == 430, "the door: pending");
    a.sensor_active(kDoor, 405);
    CHECK(a.state() == AlarmState::PENDING && a.deadline() == 430, "the door again while pending changes nothing");
    a.tick(430);
    CHECK(a.state() == AlarmState::TRIGGERED && a.cause() == kDoor && a.deadline() == 610,
          "the entry delay ends: triggered");
    a.disarm();
    CHECK(a.state() == AlarmState::DISARMED && a.cause() == 0 && !a.in_alarm(), "disarm stops the alarm");

    // Pending, then a non-entry sensor: no more waiting.
    a.arm(AlarmMode::NIGHT, false, nullptr, 0, 700);
    CHECK(a.state() == AlarmState::ARMED_NIGHT, "arm night: armed at once");
    a.sensor_active(kHall, 701);
    CHECK(a.state() == AlarmState::ARMED_NIGHT, "the hall motion is not in night mode");
    a.sensor_active(kDoor, 702);
    CHECK(a.state() == AlarmState::PENDING && a.resting() == AlarmState::ARMED_NIGHT, "pending rests at armed night");
    a.sensor_active(kShock, 703);
    CHECK(a.state() == AlarmState::TRIGGERED && a.cause() == kShock, "a non-entry sensor while pending: triggered");
    a.disarm();

    // Disarm during the entry delay.
    a.arm(AlarmMode::HOME, false, nullptr, 0, 800);
    a.sensor_active(kDoor, 801);
    a.disarm();
    CHECK(a.state() == AlarmState::DISARMED, "disarm during the entry delay");
    a.tick(2000);
    CHECK(a.state() == AlarmState::DISARMED, "a stale deadline does nothing once disarmed");

    a.arm(AlarmMode::HOME, false, nullptr, 0, 900);
    a.sensor_active(kStray, 901);
    CHECK(a.state() == AlarmState::ARMED_HOME, "an unconfigured sensor is ignored");
    a.disarm();
    a.sensor_active(kWindow, 902);
    CHECK(a.state() == AlarmState::DISARMED, "sensors are ignored while disarmed");
}

static void arming_rules() {
    AlarmConfig c = config();
    AlarmCore a(&c);
    const uint64_t open[] = {kWindow};
    CHECK(a.arm(AlarmMode::HOME, false, open, 1, 10) == AlarmArmResult::OPEN && a.state() == AlarmState::DISARMED,
          "an open window refuses the arm");
    CHECK(a.arm(AlarmMode::HOME, true, open, 1, 10) == AlarmArmResult::OK && a.state() == AlarmState::ARMED_HOME &&
          a.bypassed(kWindow) && a.bypass_count() == 1 && a.bypass_at(0) == kWindow, "force arms and bypasses it");
    a.sensor_active(kWindow, 11);
    CHECK(a.state() == AlarmState::ARMED_HOME, "a bypassed sensor is ignored");
    a.disarm();
    CHECK(!a.bypassed(kWindow) && a.bypass_count() == 0, "disarm ends the bypass");

    // Changing mode while armed; the same mode again changes nothing.
    a.arm(AlarmMode::HOME, false, nullptr, 0, 20);
    a.arm(AlarmMode::HOME, false, nullptr, 0, 25);
    CHECK(a.state() == AlarmState::ARMED_HOME, "arming the same mode again changes nothing");
    CHECK(a.arm(AlarmMode::HOME, false, open, 1, 26) == AlarmArmResult::OK && a.state() == AlarmState::ARMED_HOME &&
          a.bypass_count() == 0, "the same mode again is ok even with a window open now (nothing to arm)");
    a.arm(AlarmMode::AWAY, false, nullptr, 0, 30);
    CHECK(a.state() == AlarmState::ARMING && a.mode() == AlarmMode::AWAY && a.resting() == AlarmState::ARMED_AWAY,
          "home -> away goes through the exit delay");
    CHECK(a.arm(AlarmMode::AWAY, false, nullptr, 0, 40) == AlarmArmResult::OK && a.deadline() == 60,
          "away again while arming keeps the running exit delay");
    a.arm(AlarmMode::NIGHT, false, nullptr, 0, 41);
    CHECK(a.state() == AlarmState::ARMED_NIGHT, "night while arming away: armed night at once");

    // Busy while the alarm is on.
    a.sensor_active(kWindow, 50);
    CHECK(a.arm(AlarmMode::AWAY, false, nullptr, 0, 51) == AlarmArmResult::BUSY && a.state() == AlarmState::TRIGGERED,
          "no arming while triggered: disarm first");
    a.disarm();

    // No delays configured.
    c.exit_delay = 0;
    c.entry_delay = 0;
    a.arm(AlarmMode::AWAY, false, nullptr, 0, 60);
    CHECK(a.state() == AlarmState::ARMED_AWAY, "exit delay 0: armed away at once");
    a.sensor_active(kDoor, 61);
    CHECK(a.state() == AlarmState::TRIGGERED, "entry delay 0: the door triggers at once");
    a.disarm();
}

static void panic_and_restore() {
    AlarmConfig c = config();
    AlarmCore a(&c);
    a.panic(kPad, 10);
    CHECK(a.state() == AlarmState::TRIGGERED && a.cause() == kPad && a.deadline() == 190, "panic while disarmed");
    CHECK(a.resting() == AlarmState::DISARMED, "a panic while disarmed rests at disarmed");
    a.tick(190);
    CHECK(a.state() == AlarmState::DISARMED, "after the siren time a panic goes back to disarmed");

    a.arm(AlarmMode::NIGHT, false, nullptr, 0, 200);
    a.panic(kPad, 201);
    a.tick(381);
    CHECK(a.state() == AlarmState::ARMED_NIGHT, "a panic while armed goes back to armed");
    a.disarm();

    a.arm(AlarmMode::AWAY, false, nullptr, 0, 300);
    CHECK(a.resting() == AlarmState::ARMED_AWAY, "arming rests at the armed mode");
    a.panic(kPad, 301);
    a.tick(481);
    CHECK(a.state() == AlarmState::ARMED_AWAY, "a panic during the exit delay goes back to armed");

    AlarmCore b(&c);
    b.restore(AlarmState::ARMED_NIGHT);
    CHECK(b.state() == AlarmState::ARMED_NIGHT && b.mode() == AlarmMode::NIGHT, "restore armed night");
    b.sensor_active(kShock, 5);
    CHECK(b.state() == AlarmState::TRIGGERED, "a restored panel guards");
    AlarmCore d(&c);
    d.restore(AlarmState::DISARMED);
    CHECK(d.state() == AlarmState::DISARMED, "restore disarmed");
    d.restore(AlarmState::TRIGGERED);
    CHECK(d.state() == AlarmState::DISARMED, "a non-resting state is not restored");
}

static void keypad_codes() {
    AlarmConfig c = config();
    CHECK(!alarm_keypad_may_disarm(c, kPad, "1234"), "no codes configured: a keypad may not disarm");
    CHECK(alarm_keypad_may_disarm(c, kFob, ""), "a trusted fob may disarm without a code");
    std::snprintf(c.codes[c.n_codes++], kAlarmCodeMax, "1234");
    std::snprintf(c.codes[c.n_codes++], kAlarmCodeMax, "98765432");
    CHECK(alarm_keypad_may_disarm(c, kPad, "1234") && alarm_keypad_may_disarm(c, kPad, "98765432"), "a right code");
    CHECK(!alarm_keypad_may_disarm(c, kPad, "1235") && !alarm_keypad_may_disarm(c, kPad, "") &&
          !alarm_keypad_may_disarm(c, kPad, nullptr) && !alarm_keypad_may_disarm(c, kPad, "12345"), "a wrong code");
    CHECK(!alarm_keypad_may_disarm(c, kStray, "1234"), "an unconfigured keypad may not disarm");
}

static void names() {
    const AlarmState all[] = {AlarmState::DISARMED, AlarmState::ARMING, AlarmState::ARMED_AWAY, AlarmState::ARMED_HOME,
                              AlarmState::ARMED_NIGHT, AlarmState::PENDING, AlarmState::TRIGGERED};
    bool ok = true;
    for (AlarmState s : all) {
        AlarmState back;
        ok = ok && alarm_state_parse(alarm_state_name(s), &back) && back == s;
    }
    CHECK(ok, "every state name parses back");
    CHECK(std::strcmp(alarm_state_name(AlarmState::ARMED_HOME), "armed_home") == 0, "armed_home is HA's name");
    AlarmState s;
    CHECK(!alarm_state_parse("armed", &s) && !alarm_state_parse(nullptr, &s), "unknown state names are refused");
    AlarmMode m;
    CHECK(alarm_mode_parse("night", &m) && m == AlarmMode::NIGHT && !alarm_mode_parse("vacation", &m),
          "mode names");
    CHECK(alarm_armed_state(AlarmMode::HOME) == AlarmState::ARMED_HOME, "the armed state of a mode");
}

static bool parse(const char* json, AlarmConfig* out, char* err = nullptr, const AlarmConfig* keep = nullptr) {
    static char sink[160];
    return alarm_cfg_from_json(json, std::strlen(json), keep, out, err ? err : sink, 160);
}

static bool refuses(const char* json, const char* want) {
    AlarmConfig c{};
    char err[160] = "";
    const bool ok = parse(json, &c, err);
    const bool hit = !ok && std::strstr(err, want) != nullptr;
    if (!hit) std::printf("   (got ok=%d err=\"%s\", wanted \"%s\")\n", ok, err, want);
    return hit;
}

static void config_json() {
    AlarmConfig c{};
    CHECK(parse("{}", &c) && c.exit_delay == 30 && c.entry_delay == 30 && c.siren_time == 180 && !c.notify_arm &&
          c.n_sensors == 0 && c.n_codes == 0, "{} is the defaults");

    const char* full =
        "{\"exit_delay\":45,\"entry_delay\":20,\"siren_time\":300,\"notify_arm\":true,"
        "\"sensors\":[{\"ieee\":\"0x00158D0000000001\",\"modes\":[\"away\",\"home\",\"night\"],\"entry\":true},"
        "{\"ieee\":\"0x00158d0000000003\",\"modes\":[\"away\"]}],"
        "\"sirens\":[{\"ieee\":\"0x00158D0000000020\",\"kind\":\"warning\",\"mode\":\"fire\",\"level\":\"very_high\","
        "\"strobe\":false},{\"ieee\":\"0x00158D0000000021\",\"kind\":\"alarm\",\"melody\":\"5\",\"volume\":\"high\"},"
        "{\"ieee\":\"0x00158D0000000022\",\"kind\":\"state\"}],"
        "\"keypads\":[{\"ieee\":\"0x00158D0000000010\"},{\"ieee\":\"0x00158D0000000011\",\"trusted\":true}],"
        "\"codes\":[\"1234\",\"00000000\"],\"unknown\":1}";
    char err[160] = "";
    const bool ok = parse(full, &c, err);
    if (!ok) std::printf("   (err \"%s\")\n", err);
    CHECK(ok && c.exit_delay == 45 && c.entry_delay == 20 && c.siren_time == 300 && c.notify_arm, "full config: numbers");
    CHECK(c.n_sensors == 2 && c.sensors[0].ieee == kDoor && c.sensors[0].modes == kAll && c.sensors[0].entry &&
          c.sensors[1].ieee == kHall && c.sensors[1].modes == kAway && !c.sensors[1].entry, "full config: sensors");
    CHECK(c.n_sirens == 3 && c.sirens[0].kind == AlarmSirenKind::WARNING && std::strcmp(c.sirens[0].wd_mode, "fire") == 0 &&
          std::strcmp(c.sirens[0].wd_level, "very_high") == 0 && !c.sirens[0].strobe &&
          c.sirens[1].kind == AlarmSirenKind::ALARM && std::strcmp(c.sirens[1].melody, "5") == 0 &&
          std::strcmp(c.sirens[1].volume, "high") == 0 && c.sirens[2].kind == AlarmSirenKind::SWITCH, "full config: sirens");
    CHECK(c.n_keypads == 2 && !c.keypads[0].trusted && c.keypads[1].trusted, "full config: keypads");
    CHECK(c.n_codes == 2 && std::strcmp(c.codes[1], "00000000") == 0, "full config: codes");

    AlarmConfig w{};
    CHECK(parse("{\"sirens\":[{\"ieee\":\"0x1\",\"kind\":\"warning\"}]}", &w) &&
          std::strcmp(w.sirens[0].wd_mode, "burglar") == 0 && std::strcmp(w.sirens[0].wd_level, "high") == 0 &&
          w.sirens[0].strobe, "a warning siren defaults to burglar, high, strobe");

    // Codes: left out keeps the stored ones, [] clears them.
    AlarmConfig kept{};
    CHECK(parse("{\"siren_time\":60}", &kept, nullptr, &c) && kept.n_codes == 2 && std::strcmp(kept.codes[0], "1234") == 0,
          "codes left out keep the stored codes");
    CHECK(parse("{\"codes\":[]}", &kept, nullptr, &c) && kept.n_codes == 0, "codes [] clears them");

    // Writing: no codes on reads, a round trip with them.
    char buf[4096];
    size_t n = alarm_cfg_to_json(c, false, buf, sizeof(buf));
    CHECK(n > 0 && std::strstr(buf, "\"codes_count\":2") && !std::strstr(buf, "1234") && !std::strstr(buf, "\"codes\""),
          "a read carries codes_count, never the codes");
    CHECK(std::strstr(buf, "\"ieee\":\"0x00158D0000000001\"") != nullptr, "ieee is written as 0x + 16 upper-case hex");
    n = alarm_cfg_to_json(c, true, buf, sizeof(buf));
    AlarmConfig back{};
    CHECK(n > 0 && parse(buf, &back) && back.n_sensors == 2 && back.n_sirens == 3 && back.n_keypads == 2 &&
          back.n_codes == 2 && back.siren_time == 300 && back.notify_arm && back.sensors[1].modes == kAway &&
          std::strcmp(back.sirens[0].wd_mode, "fire") == 0 && std::strcmp(back.sirens[1].volume, "high") == 0 &&
          back.keypads[1].trusted, "a round trip with codes");
    CHECK(alarm_cfg_to_json(c, true, buf, 64) == 0, "too small a buffer writes nothing");
    CHECK(alarm_cfg_sensor(c, kHall) == &c.sensors[1] && !alarm_cfg_sensor(c, kStray) &&
          alarm_cfg_keypad(c, 0x00158D0000000011ULL) == &c.keypads[1], "lookups");

    // Every refusal names the problem.
    CHECK(refuses("[]", "object"), "not an object");
    CHECK(refuses("{\"exit_delay\":301}", "exit_delay"), "exit delay over 300");
    CHECK(refuses("{\"entry_delay\":-1}", "entry_delay"), "negative entry delay");
    CHECK(refuses("{\"siren_time\":5}", "siren_time"), "siren time under 10");
    CHECK(refuses("{\"siren_time\":1801}", "siren_time"), "siren time over 1800");
    CHECK(refuses("{\"sensors\":[{\"modes\":[\"away\"]}]}", "sensors[0]: ieee"), "a sensor without an ieee");
    CHECK(refuses("{\"sensors\":[{\"ieee\":\"kitchen\",\"modes\":[\"away\"]}]}", "sensors[0]: ieee"), "a bad ieee");
    CHECK(refuses("{\"sensors\":[{\"ieee\":\"0x0\",\"modes\":[\"away\"]}]}", "sensors[0]: ieee"), "ieee 0");
    CHECK(refuses("{\"sensors\":[{\"ieee\":\"0x1\",\"modes\":[]}]}", "sensors[0]: no mode"), "a sensor in no mode");
    CHECK(refuses("{\"sensors\":[{\"ieee\":\"0x1\",\"modes\":[\"vacation\"]}]}", "sensors[0]: mode"), "an unknown mode");
    CHECK(refuses("{\"sensors\":[{\"ieee\":\"0x1\",\"modes\":[\"away\"]},{\"ieee\":\"0x01\",\"modes\":[\"home\"]}]}",
                  "sensors[1]: 0x0000000000000001 is listed twice"), "a sensor listed twice");
    CHECK(refuses("{\"sirens\":[{\"ieee\":\"0x1\",\"kind\":\"horn\"}]}", "sirens[0]: kind"), "an unknown siren kind");
    CHECK(refuses("{\"sirens\":[{\"ieee\":\"0x1\",\"kind\":\"warning\",\"mode\":\"loud\"}]}", "sirens[0]: mode"),
          "an unknown warning mode");
    CHECK(refuses("{\"sirens\":[{\"ieee\":\"0x1\",\"kind\":\"warning\",\"level\":\"max\"}]}", "sirens[0]: level"),
          "an unknown warning level");
    CHECK(refuses("{\"sirens\":[{\"ieee\":\"0x1\",\"kind\":\"state\"},{\"ieee\":\"0x1\",\"kind\":\"state\"}]}",
                  "sirens[1]"), "a siren listed twice");
    CHECK(refuses("{\"sirens\":[{\"ieee\":\"0x1\",\"kind\":\"alarm\",\"melody\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}]}",
                  "sirens[0]: melody"), "a melody too long");
    CHECK(refuses("{\"keypads\":[{\"ieee\":\"0x1\"},{\"ieee\":\"0x1\",\"trusted\":true}]}", "keypads[1]"),
          "a keypad listed twice");
    CHECK(refuses("{\"codes\":[\"123\"]}", "codes[0]"), "a code too short");
    CHECK(refuses("{\"codes\":[\"123456789\"]}", "codes[0]"), "a code too long");
    CHECK(refuses("{\"codes\":[\"12a4\"]}", "codes[0]"), "a code with a letter");
    CHECK(refuses("{\"codes\":[1234]}", "codes[0]"), "a code that is a number, not a string");
    CHECK(refuses("{\"codes\":[\"1234\",\"1234\"]}", "codes[1]"), "a code listed twice");

    char many[8192];
    size_t m = std::snprintf(many, sizeof(many), "{\"sensors\":[");
    for (int i = 1; i <= kAlarmMaxSensors + 1; i++)
        m += std::snprintf(many + m, sizeof(many) - m, "%s{\"ieee\":\"0x%X\",\"modes\":[\"away\"]}", i > 1 ? "," : "", i);
    std::snprintf(many + m, sizeof(many) - m, "]}");
    CHECK(refuses(many, "at most 48"), "more than 48 sensors");
}

int main() {
    transitions();
    arming_rules();
    panic_and_restore();
    keypad_codes();
    names();
    config_json();
    std::printf("%s (%d failed)\n", g_fail ? "FAILED" : "ALL PASSED", g_fail);
    return g_fail ? 1 : 0;
}
