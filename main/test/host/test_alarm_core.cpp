// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Alarm panel (spec extra/docs/superpowers/specs/2026-11-24-safety-profiles-design.md):
// every state transition of a profile, the delays on a fake clock, the arming check and
// force, the bypass, edges and hazards, intrusion profiles replacing each other, silence,
// disarm all, panic, restore, the keypad code policy and the output fit; what changed between
// two looks, the rule events, the notification texts and what each output device plays.
#include "alarm_core.h"
#include "utf8_seq.h"

#include <cstdio>
#include <cstring>
#include <string>

static int g_fail = 0;
#define CHECK(c, m) do { if (c) std::printf("PASS: %s\n", m); else { std::printf("FAIL: %s\n", m); g_fail++; } } while (0)

enum : uint8_t { kAway, kHome, kNight, kFlood, kFire };              // profile indexes
enum : uint8_t { sDoor, sWindow, sHall, sShock, sLeak, sSmoke };     // sensor slots
static uint64_t ieee_of(uint8_t slot) { return 0x00158D0000000001ULL + slot; }
constexpr uint64_t kPad = 0x00158D0000000010ULL, kFob = 0x00158D0000000011ULL;
constexpr uint64_t kStray = 0x00158D00000000FFULL;   // not configured

static AlarmProfile prof(const char* id, AlarmClass cls, AlarmRole role, uint16_t arm, uint16_t trig) {
    AlarmProfile p{}; std::snprintf(p.id, sizeof(p.id), "%s", id); std::snprintf(p.name, sizeof(p.name), "%s", id);
    p.cls = cls; p.role = role; p.arm_delay = arm; p.trigger_delay = trig; p.siren_time = 180; return p;
}
static AlarmConfig config() {
    AlarmConfig c{};
    c.profiles[c.n_profiles++] = prof("away",  AlarmClass::INTRUSION, AlarmRole::AWAY,  30, 30);
    c.profiles[c.n_profiles++] = prof("home",  AlarmClass::INTRUSION, AlarmRole::HOME,   0, 30);
    c.profiles[c.n_profiles++] = prof("night", AlarmClass::INTRUSION, AlarmRole::NIGHT,  0, 30);
    c.profiles[c.n_profiles++] = prof("flood", AlarmClass::WATER,     AlarmRole::NONE,   0,  0);
    c.profiles[c.n_profiles++] = prof("fire",  AlarmClass::FIRE,      AlarmRole::NONE,   0,  0);
    c.sensors[c.n_sensors++] = {ieee_of(sDoor),   0x07, 0x07};   // every intrusion profile, delayed
    c.sensors[c.n_sensors++] = {ieee_of(sWindow), 0x07, 0};
    c.sensors[c.n_sensors++] = {ieee_of(sHall),   0x01, 0};      // away only
    c.sensors[c.n_sensors++] = {ieee_of(sShock),  0x05, 0};      // away and night
    c.sensors[c.n_sensors++] = {ieee_of(sLeak),   0x08, 0};
    c.sensors[c.n_sensors++] = {ieee_of(sSmoke),  0x10, 0};
    c.keypads[c.n_keypads++] = {kPad, false};
    c.keypads[c.n_keypads++] = {kFob, true};
    return c;
}
#define ST(i) p.at(i).state

static void transitions() {
    AlarmConfig c = config(); AlarmPanel p(&c); uint64_t open = 0;
    CHECK(ST(kAway) == AlarmState::DISARMED && !p.running() && p.summary_profile() == -1, "starts disarmed");
    CHECK(p.arm(kHome, false, 100, &open) == AlarmArmResult::OK && ST(kHome) == AlarmState::ARMED, "arm delay 0: armed at once");
    p.disarm(kHome);
    CHECK(ST(kHome) == AlarmState::DISARMED && p.at(kHome).disarms == 1, "disarm");
    CHECK(p.arm(kAway, false, 100, &open) == AlarmArmResult::OK && ST(kAway) == AlarmState::ARMING &&
          p.at(kAway).deadline == 130 && p.running() && p.summary_profile() == kAway, "arm away: arming for its arm delay");
    p.sensor(sHall, AlarmKey::MOTION, true, 110);
    CHECK(ST(kAway) == AlarmState::ARMING, "sensors are ignored while arming");
    p.tick(129); CHECK(ST(kAway) == AlarmState::ARMING, "still arming a second before the end");
    p.tick(130); CHECK(ST(kAway) == AlarmState::ARMED, "armed when the arm delay ends");
    p.sensor(sWindow, AlarmKey::CONTACT, true, 200);
    CHECK(ST(kAway) == AlarmState::TRIGGERED && p.at(kAway).cause == ieee_of(sWindow) && p.at(kAway).deadline == 380 &&
          p.at(kAway).alarms == 1 && p.at(kAway).rests_armed, "a window: triggered at once, for the siren time");
    p.sensor(sHall, AlarmKey::MOTION, true, 210);
    CHECK(p.at(kAway).cause == ieee_of(sWindow) && p.at(kAway).alarms == 1, "more sensors while triggered change nothing");
    p.tick(380);
    CHECK(ST(kAway) == AlarmState::ARMED && p.at(kAway).cause == 0 && p.at(kAway).alarm_cause == ieee_of(sWindow),
          "the siren time ends: back to armed; the latest alarm's cause is kept");
    p.sensor(sDoor, AlarmKey::CONTACT, true, 400);
    CHECK(ST(kAway) == AlarmState::PENDING && p.at(kAway).cause == ieee_of(sDoor) && p.at(kAway).deadline == 430, "the door: pending");
    p.tick(430);
    CHECK(ST(kAway) == AlarmState::TRIGGERED && p.at(kAway).deadline == 610 && p.at(kAway).alarms == 2, "the trigger delay ends: triggered");
    p.disarm(kAway);
    CHECK(ST(kAway) == AlarmState::DISARMED && p.at(kAway).cause == 0 && !p.running(), "disarm stops the alarm");
    p.sensor(sWindow, AlarmKey::CONTACT, false, 615); p.sensor(sDoor, AlarmKey::CONTACT, false, 620);
    p.arm(kAway, false, 700, &open); p.tick(730);
    p.sensor(sDoor, AlarmKey::CONTACT, true, 740); p.sensor(sHall, AlarmKey::MOTION, true, 745);
    CHECK(ST(kAway) == AlarmState::TRIGGERED && p.at(kAway).cause == ieee_of(sHall), "a sensor that is not delayed ends pending at once");
    AlarmConfig c0 = config(); c0.profiles[kNight].trigger_delay = 0; AlarmPanel q(&c0);
    q.arm(kNight, false, 0, &open); q.sensor(sDoor, AlarmKey::CONTACT, true, 5);
    CHECK(q.at(kNight).state == AlarmState::TRIGGERED, "trigger delay 0: a delayed sensor triggers at once");
}

static void arming_rules() {
    AlarmConfig c = config(); AlarmPanel p(&c); uint64_t open = 0;
    p.sensor(sWindow, AlarmKey::CONTACT, true, 10);
    CHECK(p.arm(kNight, false, 20, &open) == AlarmArmResult::OPEN && open == (1ull << sWindow) && ST(kNight) == AlarmState::DISARMED,
          "an open contact refuses the arm and is listed");
    CHECK(p.arm(kNight, true, 20, &open) == AlarmArmResult::OK && ST(kNight) == AlarmState::ARMED &&
          p.at(kNight).bypass == (1ull << sWindow), "force arms; the open sensor is bypassed");
    p.sensor(sWindow, AlarmKey::CONTACT, false, 30); p.sensor(sWindow, AlarmKey::CONTACT, true, 40);
    CHECK(ST(kNight) == AlarmState::ARMED, "a bypassed sensor does not trigger");
    CHECK(p.arm(kNight, false, 50, &open) == AlarmArmResult::OK && ST(kNight) == AlarmState::ARMED, "arming what is armed changes nothing");
    p.disarm(kNight);
    CHECK(p.at(kNight).bypass == 0, "disarm clears the bypass");
    p.sensor(sWindow, AlarmKey::CONTACT, false, 55);
    p.arm(kNight, false, 60, &open); p.sensor(sShock, AlarmKey::MOTION, true, 70);
    CHECK(ST(kNight) == AlarmState::TRIGGERED && p.arm(kNight, false, 80, &open) == AlarmArmResult::BUSY, "triggered: arming is refused");
    AlarmPanel q(&c);
    q.seed(sDoor, AlarmKey::CONTACT, true);
    CHECK(q.arm(kAway, false, 0, &open) == AlarmArmResult::OPEN, "a seeded open contact refuses the arm too");
    q.sensor(sHall, AlarmKey::MOTION, true, 1);
    CHECK(q.arm(kAway, true, 2, &open) == AlarmArmResult::OK && q.at(kAway).state == AlarmState::ARMING, "motion at arming is ignored");
}

static void edges() {
    AlarmConfig c = config(); AlarmPanel p(&c); uint64_t open = 0;
    p.seed(sWindow, AlarmKey::CONTACT, false);
    p.arm(kHome, false, 0, &open);
    p.sensor(sWindow, AlarmKey::CONTACT, true, 10); p.tick(190);
    p.sensor(sWindow, AlarmKey::CONTACT, true, 200);
    CHECK(ST(kHome) == AlarmState::ARMED && p.at(kHome).alarms == 1, "a repeated open is not a new opening");
    p.sensor(sWindow, AlarmKey::CONTACT, false, 210); p.sensor(sWindow, AlarmKey::CONTACT, true, 220);
    CHECK(ST(kHome) == AlarmState::TRIGGERED && p.at(kHome).alarms == 2, "closed and opened again: a new alarm");
    AlarmPanel q(&c);
    q.arm(kHome, false, 0, &open); q.sensor(sWindow, AlarmKey::CONTACT, true, 5);
    CHECK(q.at(kHome).state == AlarmState::TRIGGERED, "the first report of an unknown contact, open, counts");
}

static void hazards() {
    AlarmConfig c = config(); AlarmPanel p(&c); uint64_t open = 0;
    p.arm(kFlood, false, 0, &open);
    p.sensor(sLeak, AlarmKey::WATER_LEAK, true, 10);
    CHECK(ST(kFlood) == AlarmState::TRIGGERED && p.at(kFlood).cause == ieee_of(sLeak), "a leak triggers the armed flood profile");
    p.sensor(sLeak, AlarmKey::WATER_LEAK, true, 20); p.tick(190);
    CHECK(ST(kFlood) == AlarmState::ARMED && p.at(kFlood).alarms == 1, "the alarm time ends with the sensor still wet: armed, no new alarm");
    p.sensor(sLeak, AlarmKey::WATER_LEAK, true, 200);
    CHECK(ST(kFlood) == AlarmState::ARMED, "still wet, reported again: no new alarm");
    p.sensor(sLeak, AlarmKey::WATER_LEAK, false, 210); p.sensor(sLeak, AlarmKey::WATER_LEAK, true, 220);
    CHECK(ST(kFlood) == AlarmState::TRIGGERED && p.at(kFlood).alarms == 2, "dry and wet again: a new alarm");
    p.silence(kFlood);
    CHECK(ST(kFlood) == AlarmState::ARMED && p.at(kFlood).alarms == 2, "silenced while wet: armed, no new alarm");
    p.disarm(kFlood);
    CHECK(p.arm(kFlood, false, 300, &open) == AlarmArmResult::OK && ST(kFlood) == AlarmState::TRIGGERED && p.at(kFlood).alarms == 3,
          "armed from disarmed while still wet: the alarm");
    AlarmPanel q(&c);
    q.seed(sLeak, AlarmKey::WATER_LEAK, true); q.restore(kFlood, true); q.tick(5);
    CHECK(q.at(kFlood).state == AlarmState::ARMED && q.at(kFlood).alarms == 0, "restored armed with a wet sensor: no alarm");
    AlarmConfig d = config(); d.profiles[kFire].arm_delay = 10; AlarmPanel r(&d);
    r.seed(sSmoke, AlarmKey::SMOKE, true);
    r.arm(kFire, false, 0, &open);
    CHECK(r.at(kFire).state == AlarmState::ARMING, "the arm delay runs first");
    r.tick(10);
    CHECK(r.at(kFire).state == AlarmState::TRIGGERED && r.at(kFire).cause == ieee_of(sSmoke), "smoke still there when armed: the alarm");
    AlarmPanel s(&c);
    s.arm(kFire, false, 0, &open);
    s.sensor(sSmoke, AlarmKey::SMOKE, true, 5); s.tick(185); s.sensor(sSmoke, AlarmKey::CO, true, 190);
    CHECK(s.at(kFire).alarms == 2, "another hazard key of the same device going active is a new event");
}

// What sensor() reports as a change of a kept value: the glue then has the shadow write it to flash at once (review F1).
static void kept_changes() {
    AlarmConfig c = config(); AlarmPanel p(&c);
    CHECK(p.sensor(sDoor, AlarmKey::CONTACT, true, 1) && !p.sensor(sDoor, AlarmKey::CONTACT, true, 2) &&
          p.sensor(sDoor, AlarmKey::CONTACT, false, 3) && !p.sensor(sDoor, AlarmKey::CONTACT, false, 4),
          "a contact: unknown to open and open to closed change it; the same again does not");
    CHECK(p.sensor(sLeak, AlarmKey::WATER_LEAK, true, 5) && !p.sensor(sLeak, AlarmKey::WATER_LEAK, true, 6) &&
          p.sensor(sLeak, AlarmKey::WATER_LEAK, false, 7), "a hazard: wet and dry change it; wet again does not");
    CHECK(!p.sensor(sHall, AlarmKey::MOTION, true, 8) && !p.sensor(sHall, AlarmKey::MOTION, false, 9), "motion keeps nothing: never a change");
    CHECK(!p.sensor(c.n_sensors, AlarmKey::CONTACT, true, 10), "not a sensor: nothing");
}

static void group() {
    AlarmConfig c = config(); AlarmPanel p(&c); uint64_t open = 0;
    p.arm(kHome, false, 0, &open); p.arm(kFlood, false, 0, &open);
    CHECK(p.arm(kNight, false, 10, &open) == AlarmArmResult::OK && ST(kHome) == AlarmState::DISARMED && ST(kNight) == AlarmState::ARMED &&
          p.at(kHome).disarms == 0 && p.summary_profile() == kNight, "an intrusion profile replaces the armed one; that is not a disarm");
    CHECK(ST(kFlood) == AlarmState::ARMED, "other classes are not touched");
    p.sensor(sLeak, AlarmKey::WATER_LEAK, true, 20);
    CHECK(ST(kFlood) == AlarmState::TRIGGERED && ST(kNight) == AlarmState::ARMED && p.summary_profile() == kNight, "a leak triggers flood only");
    CHECK(p.arm(kAway, false, 30, &open) == AlarmArmResult::OK && ST(kAway) == AlarmState::ARMING && ST(kNight) == AlarmState::DISARMED,
          "an intrusion profile can be armed while another class is triggered");
    p.tick(60); p.sensor(sDoor, AlarmKey::CONTACT, true, 70);
    CHECK(ST(kAway) == AlarmState::PENDING && p.arm(kNight, false, 75, &open) == AlarmArmResult::BUSY && ST(kAway) == AlarmState::PENDING,
          "no switch while an intrusion profile is pending");
    AlarmPanel q(&c);
    q.arm(kAway, false, 0, &open); q.tick(30); q.sensor(sWindow, AlarmKey::CONTACT, true, 40); q.tick(220);
    CHECK(q.arm(kNight, false, 230, &open) == AlarmArmResult::OPEN && q.at(kAway).state == AlarmState::ARMED,
          "a refused switch leaves the armed profile armed");
}

static void silence_and_disarm_all() {
    AlarmConfig c = config(); AlarmPanel p(&c); uint64_t open = 0;
    p.arm(kNight, false, 0, &open); p.arm(kFlood, false, 0, &open); p.arm(kFire, false, 0, &open);
    p.sensor(sLeak, AlarmKey::WATER_LEAK, true, 10);
    p.disarm_all();
    CHECK(ST(kNight) == AlarmState::DISARMED && p.at(kNight).disarms == 1, "disarm all disarms the intrusion profile");
    CHECK(ST(kFlood) == AlarmState::ARMED && p.at(kFlood).disarms == 0, "and silences a sounding profile of another class");
    CHECK(ST(kFire) == AlarmState::ARMED, "and leaves an armed one armed");
    p.arm(kNight, false, 20, &open); p.sensor(sDoor, AlarmKey::CONTACT, true, 30); p.silence(kNight);
    CHECK(ST(kNight) == AlarmState::ARMED && p.at(kNight).cause == 0, "silence ends pending: back to armed");
}

static void panic_and_restore() {
    AlarmConfig c = config(); AlarmPanel p(&c); uint64_t open = 0;
    CHECK(p.panic(kPad, 100) && ST(kAway) == AlarmState::TRIGGERED && !p.at(kAway).rests_armed && p.at(kAway).cause == kPad,
          "panic with everything disarmed: the first intrusion profile");
    p.silence(kAway);
    CHECK(ST(kAway) == AlarmState::DISARMED, "silence after a panic from disarmed: back to disarmed");
    p.panic(kPad, 200); p.tick(380);
    CHECK(ST(kAway) == AlarmState::DISARMED && p.at(kAway).disarms == 0, "the alarm time of such a panic ends in disarmed; not a disarm");
    p.arm(kNight, false, 400, &open);
    CHECK(p.panic(kPad, 410) && ST(kNight) == AlarmState::TRIGGERED && p.at(kNight).rests_armed && ST(kAway) == AlarmState::DISARMED,
          "panic goes to the intrusion profile that is not disarmed");
    p.panic(kPad, 420);
    CHECK(p.at(kNight).alarms == 1, "panic while triggered changes nothing");
    p.disarm(kNight); p.arm(kAway, false, 500, &open);
    CHECK(p.panic(kPad, 510) && ST(kAway) == AlarmState::TRIGGERED && p.at(kAway).rests_armed, "panic during the arm delay: that profile, armed afterwards");
    AlarmConfig d{}; d.profiles[d.n_profiles++] = prof("flood", AlarmClass::WATER, AlarmRole::NONE, 0, 0); AlarmPanel q(&d);
    CHECK(!q.panic(kPad, 0) && q.at(0).state == AlarmState::DISARMED, "no intrusion profile: panic does nothing");
    AlarmPanel r(&c);
    r.restore(kNight, true); r.restore(kFire, true);
    CHECK(r.at(kNight).state == AlarmState::ARMED && r.at(kFire).state == AlarmState::ARMED && r.at(kAway).state == AlarmState::DISARMED &&
          r.at(kNight).bypass == 0 && r.summary_profile() == kNight, "restore");
}

// A panic during the arm delay: the arm has not reached armed, so a hazard still active raises its
// alarm once that panic's alarm ends in armed, by its time or by a silence.
static void panic_before_armed() {
    AlarmConfig c = config(); c.sensors[sSmoke].profiles |= 1u << kAway; uint64_t open = 0;
    AlarmPanel p(&c);
    p.seed(sSmoke, AlarmKey::SMOKE, true);
    p.arm(kAway, false, 0, &open); p.panic(kPad, 10);
    CHECK(ST(kAway) == AlarmState::TRIGGERED && p.at(kAway).cause == kPad && p.at(kAway).alarms == 1, "panic during the arm delay");
    p.tick(190);
    CHECK(ST(kAway) == AlarmState::TRIGGERED && p.at(kAway).cause == ieee_of(sSmoke) && p.at(kAway).alarms == 2 &&
          p.at(kAway).rests_armed, "the panic's alarm ends: the smoke's alarm follows at that tick");
    p.tick(370); p.tick(371);
    CHECK(ST(kAway) == AlarmState::ARMED && p.at(kAway).alarms == 2, "the smoke's alarm ends: armed, no third alarm");
    AlarmPanel q(&c);
    q.seed(sSmoke, AlarmKey::SMOKE, true);
    q.arm(kAway, false, 0, &open); q.panic(kPad, 10); q.silence(kAway);
    CHECK(q.at(kAway).state == AlarmState::ARMED && q.at(kAway).alarms == 1, "the panic silenced: armed");
    q.tick(11);
    CHECK(q.at(kAway).state == AlarmState::TRIGGERED && q.at(kAway).cause == ieee_of(sSmoke) && q.at(kAway).alarms == 2,
          "the next tick raises the smoke's alarm");
    q.tick(191); q.tick(192);
    CHECK(q.at(kAway).state == AlarmState::ARMED && q.at(kAway).alarms == 2, "and only once");
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
    const AlarmState all[] = {AlarmState::DISARMED, AlarmState::ARMING, AlarmState::ARMED, AlarmState::PENDING, AlarmState::TRIGGERED};
    const char* const words[] = {"disarmed", "arming", "armed", "pending", "triggered"};
    bool ok = true;
    for (size_t i = 0; i < 5; i++) {
        AlarmState back;
        ok = ok && std::strcmp(alarm_state_name(all[i]), words[i]) == 0 && alarm_state_parse(words[i], &back) && back == all[i];
    }
    CHECK(ok, "the five state words, both ways");
    AlarmState s;
    CHECK(!alarm_state_parse("armed_away", &s) && !alarm_state_parse(nullptr, &s), "stage 1 and unknown state names are refused");
}

// An output must have something writable for its kind; a reading cannot sound.
static void siren_fit() {
    auto fits = [](AlarmOutKind k, const char* ex) { return alarm_output_misfit(k, ex, std::strlen(ex)) == nullptr; };
    const char* ias  = R"([{"name":"warning","type":"composite","access":2},{"name":"battery","type":"numeric","access":5}])";
    const char* tuya = R"([{"name":"alarm","type":"binary","access":7},{"name":"melody","type":"enum","access":7}])";
    const char* smoke = R"([{"name":"smoke","type":"binary","access":1},{"name":"alarm","type":"binary","access":1}])";
    const char* plug = R"([{"name":"state","type":"binary","access":7},{"name":"power","type":"numeric","access":5}])";
    const char* hubio = R"([{"name":"relay1","type":"binary","access":7}])";
    CHECK(fits(AlarmOutKind::WARNING, ias), "an IAS siren takes kind warning");
    CHECK(!fits(AlarmOutKind::ALARM, ias), "an IAS siren has no alarm switch");
    CHECK(fits(AlarmOutKind::ALARM, tuya), "a Tuya siren takes kind alarm");
    CHECK(!fits(AlarmOutKind::WARNING, tuya), "a Tuya siren takes no warning");
    CHECK(!fits(AlarmOutKind::ALARM, smoke), "a smoke sensor's alarm is a reading, not a siren");
    CHECK(fits(AlarmOutKind::STATE, plug), "a plug takes kind state");
    CHECK(!fits(AlarmOutKind::STATE, hubio), "a Hub I/O output named relay1 has no state");
    CHECK(!fits(AlarmOutKind::STATE, "not json"), "unreadable exposes do not fit");
    const char* why = alarm_output_misfit(AlarmOutKind::ALARM, smoke, std::strlen(smoke));
    CHECK(why && std::strstr(why, "alarm"), "the reason names what is missing");
}

// ── The config JSON ──

static bool parse(const char* j, AlarmConfig* c, char* err, size_t cap, const AlarmConfig* keep = nullptr) {
    return alarm_cfg_from_json(j, std::strlen(j), keep, c, err, cap);
}
static bool parse1(const char* j, AlarmConfig* c, char* err, size_t cap) {   // the stage 1 record, at boot
    return alarm_cfg_from_stage1(j, std::strlen(j), c, err, cap);
}
// Two configs are the same when they write the same: struct padding does not count.
static bool same(const AlarmConfig& a, const AlarmConfig& b) {
    static char x[32768], y[32768]; JsonDocument da, db;
    alarm_cfg_write(a, true, da.to<JsonObject>()); alarm_cfg_write(b, true, db.to<JsonObject>());
    return serializeJson(da, x, sizeof(x)) && serializeJson(db, y, sizeof(y)) && std::strcmp(x, y) == 0;
}
// {"ieee":"0x<first>"},{"ieee":"0x<first+1>"}, ... `count` of them, numbered in hex; `extra` is put after each ieee,
// e.g. ,"kind":"warning".
static std::string items(unsigned first, unsigned count, const char* extra = "") {
    std::string s;
    for (unsigned i = 0; i < count; i++) {
        char b[64]; std::snprintf(b, sizeof(b), "%s{\"ieee\":\"0x%X\"%s}", i ? "," : "", first + i, extra);
        s += b;
    }
    return s;
}
// A stage 1 config with 48 sensors in away, home and night and 8 sirens of kind warning.
static std::string largest_stage1() {
    std::string s = R"({"sensors":[)";
    for (unsigned i = 0; i < 48; i++) {
        char b[96]; std::snprintf(b, sizeof(b), "%s{\"ieee\":\"0x%X\",\"modes\":[\"away\",\"home\",\"night\"]}", i ? "," : "", 1 + i);
        s += b;
    }
    return s + R"(],"sirens":[)" + items(0x100, 8, R"(,"kind":"warning")") + "]}";
}
// M17: the one table of class words; the config reads what it names, and the glue writes it.
static void class_words() {
    static AlarmConfig c; char err[160]; bool ok = true;
    for (uint8_t i = 0; i <= static_cast<uint8_t>(AlarmClass::OTHER); i++) {
        const std::string j = std::string(R"({"profiles":[{"id":"a","name":"A","class":")") + alarm_class_name(static_cast<AlarmClass>(i)) + R"("}]})";
        ok = ok && parse(j.c_str(), &c, err, sizeof(err)) && c.profiles[0].cls == static_cast<AlarmClass>(i);
    }
    CHECK(ok && std::strcmp(alarm_class_name(AlarmClass::INTRUSION), "intrusion") == 0 && std::strcmp(alarm_class_name(AlarmClass::OTHER), "other") == 0,
          "the five class words, as the config reads them");
    CHECK(std::strcmp(alarm_class_name(static_cast<AlarmClass>(9)), "other") == 0, "a value that is not a class reads as other");
}
static void config_json() {
    static AlarmConfig c, k; char err[160];
    const char* j = R"({"profiles":[
      {"id":"flood","name":"Flood","class":"water","siren_time":60,"notify_arm":true,
       "sensors":[{"ieee":"0x00158D0001020304"}],
       "outputs":[{"ieee":"0x00158D0005060708","kind":"state","value":"off","keep":true},
                  {"ieee":"0x00158D00090A0B0C","kind":"warning","mode":"emergency"}]},
      {"id":"night","name":"Ніч","class":"intrusion","role":"night","trigger_delay":20,
       "sensors":[{"ieee":"0x00158D0001020304","entry":true},{"ieee":"0x1"}]}],
     "keypads":[{"ieee":"0x10","trusted":true}],"codes":["1234"]})";
    CHECK(parse(j, &c, err, sizeof(err)), "the API form parses");
    CHECK(c.n_profiles == 2 && c.n_sensors == 2 && c.n_outputs == 2 && c.n_keypads == 1 && c.n_codes == 1, "counts");
    CHECK(c.profiles[0].cls == AlarmClass::WATER && c.profiles[0].siren_time == 60 && c.profiles[0].arm_delay == 0 &&
          c.profiles[0].notify_arm && c.profiles[1].role == AlarmRole::NIGHT && c.profiles[1].siren_time == 180, "fields and defaults");
    CHECK(c.sensors[0].profiles == 0x03 && c.sensors[0].entry == 0x02 && c.sensors[1].profiles == 0x02, "a sensor once, with its profiles and its delays");
    CHECK(c.outputs[0].profile == 0 && c.outputs[0].kind == AlarmOutKind::STATE && !c.outputs[0].value && c.outputs[0].keep, "a valve");
    CHECK(std::strcmp(c.outputs[1].wd_mode, "emergency") == 0 && std::strcmp(c.outputs[1].wd_level, "high") == 0 && c.outputs[1].strobe, "siren defaults");
    JsonDocument d; alarm_cfg_write(c, true, d.to<JsonObject>());
    char buf[2048]; serializeJson(d, buf, sizeof(buf));
    CHECK(parse(buf, &k, err, sizeof(err)) && same(c, k) && k.sensors[0].entry == 0x02 && !k.outputs[0].value && k.outputs[0].keep,
          "written and read back: the same config");
    CHECK(std::strstr(buf, "\"role\":\"night\"") && !std::strstr(buf, "\"role\":\"\""), "role is written for intrusion profiles only");
    JsonDocument e; alarm_cfg_write(c, false, e.to<JsonObject>());
    CHECK(e["codes"].isNull() && e["codes_count"] == 1, "reads never carry the codes");
    CHECK(parse(R"({"profiles":[]})", &k, err, sizeof(err), &c) && k.n_codes == 1 && k.n_profiles == 0, "codes left out are kept; no profile is allowed");
    CHECK(parse(R"({"profiles":[],"codes":[]})", &k, err, sizeof(err), &c) && k.n_codes == 0, "an empty list clears the codes");
    alarm_cfg_defaults(&k);
    CHECK(k.n_profiles == 3 && std::strcmp(k.profiles[0].id, "away") == 0 && k.profiles[0].arm_delay == 30 &&
          k.profiles[2].role == AlarmRole::NIGHT && k.profiles[2].arm_delay == 0 && k.profiles[1].trigger_delay == 30 && k.n_sensors == 0, "defaults");
}
static void config_errors() {
    static AlarmConfig c; char err[160];
    auto bad = [&](const char* j, const char* part, const char* what) {
        const bool ok = parse(j, &c, err, sizeof(err));
        CHECK(!ok && std::strstr(err, part), what);
    };
    bad(R"({"profiles":[{"id":"Away","name":"A","class":"intrusion"}]})", "id must be", "an id in capitals");
    bad(R"({"profiles":[{"id":"abcdefghijklmn","name":"A","class":"fire"}]})", "id must be", "an id of 14 characters");
    bad(R"({"profiles":[{"id":"armed","name":"A","class":"fire"}]})", "is reserved", "a state word as id");
    bad(R"({"profiles":[{"id":"a","name":"A","class":"fire"},{"id":"a","name":"B","class":"gas"}]})", "profiles[1]: id a is taken", "an id twice");
    bad(R"({"profiles":[{"id":"a","name":"","class":"fire"}]})", "name must be", "an empty name");
    bad("{\"profiles\":[{\"id\":\"a\",\"name\":\"\xC3\x28\",\"class\":\"fire\"}]}", "name must be", "a name that is not UTF-8");
    bad(R"({"profiles":[{"id":"a","name":"1234567890123456789012345","class":"fire"}]})", "name must be", "a name of 25 bytes");
    bad(R"({"profiles":[{"id":"a","name":"A\nB","class":"fire"}]})", "name must be", "a name with a control character");
    bad(R"({"profiles":[{"id":"a","name":"A","class":"burglary"}]})", "class must be", "an unknown class");
    bad(R"({"profiles":[{"id":"a","name":"A","class":"fire","role":"away"}]})", "role is", "a role outside intrusion");
    bad(R"({"profiles":[{"id":"a","name":"A","class":"intrusion","role":"away"},{"id":"b","name":"B","class":"intrusion","role":"away"}]})",
        "role away is taken by a", "a role twice");
    bad(R"({"profiles":[{"id":"a","name":"A","class":"fire","arm_delay":301}]})", "arm_delay must be 0-300", "an arm delay over the limit");
    bad(R"({"profiles":[{"id":"a","name":"A","class":"fire","siren_time":9}]})", "siren_time must be 10-1800", "a siren time under the limit");
    bad(R"({"profiles":[{"id":"a","name":"A","class":"fire","sensors":[{"ieee":"0x1"},{"ieee":"0x1"}]}]})", "sensors[1]: listed twice", "a sensor twice");
    bad(R"({"profiles":[{"id":"a","name":"A","class":"fire","outputs":[{"ieee":"0x1","kind":"warning","keep":true}]}]})", "for kind state", "keep on a siren");
    bad(R"({"profiles":[{"id":"a","name":"A","class":"water","outputs":[{"ieee":"0x1","kind":"state","value":"off","keep":true}]},
                        {"id":"b","name":"B","class":"fire","outputs":[{"ieee":"0x1","kind":"state","value":"off"}]}]})",
        "profiles[1].outputs[0]: the same switch has another value or another keep in a", "a valve another profile would reopen");
    bad(R"({"profiles":[{"id":"a","name":"A","class":"fire","outputs":[{"ieee":"0x1","kind":"alarm","melody":"a\u0001b"}]}]})", "melody must be plain text",
        "a melody with a control character (it would not fit the stored form)");
    // Not UTF-8: alarm.get and every reply carrying it would be text frames a browser drops (review F7). A lone surrogate
    // gets here as raw bytes (ED A0 80, what a WTF-8 encoder sends); as an escape, ArduinoJson drops a lone high one
    // and makes a lone low one U+10000, valid either way (checked below).
    bad("{\"profiles\":[{\"id\":\"a\",\"name\":\"A\",\"class\":\"fire\",\"outputs\":[{\"ieee\":\"0x1\",\"kind\":\"alarm\",\"melody\":\"a\xED\xA0\x80\"}]}]}",
        "melody must be plain text", "a melody with a lone surrogate (not UTF-8)");
    bad("{\"profiles\":[{\"id\":\"a\",\"name\":\"A\",\"class\":\"fire\",\"outputs\":[{\"ieee\":\"0x1\",\"kind\":\"alarm\",\"volume\":\"\xC3\x28\"}]}]}",
        "volume must be plain text", "a volume that is not UTF-8");
    CHECK(parse(R"({"profiles":[{"id":"a","name":"A","class":"fire","outputs":[{"ieee":"0x1","kind":"alarm","melody":"a\uD800b","volume":"\uDC00"}]}]})",
                &c, err, sizeof(err)) &&
          utf8_valid(c.outputs[0].melody, std::strlen(c.outputs[0].melody)) && utf8_valid(c.outputs[0].volume, std::strlen(c.outputs[0].volume)),
          "lone surrogate escapes: what the parser makes of them is UTF-8, and kept");
    const std::string p1 = R"({"id":"a","name":"A","class":"fire",)", w = R"(,"kind":"warning")";
    bad((R"({"profiles":[)" + p1 + R"("sensors":[)" + items(1, 48) + R"(]},{"id":"b","name":"B","class":"gas","sensors":[)" + items(49, 1) + "]}]}").c_str(),
        "profiles[1].sensors[0]: more than 48 different sensors", "a 49th different sensor");
    // M5: the 49th sensor of one profile reads as the 49th different one: one message for both
    CHECK(!parse((R"({"profiles":[)" + p1 + R"("sensors":[)" + items(1, 49) + R"(]}]})").c_str(), &c, err, sizeof(err)) &&
          std::strcmp(err, "profiles[0].sensors[48]: more than 48 different sensors") == 0, "a 49th sensor in one profile: the same message");
    CHECK(!parse((R"({"profiles":[)" + p1 + R"("sensors":[)" + items(1, 48) + R"(]},{"id":"b","name":"B","class":"gas","sensors":[)" + items(49, 1) + "]}]}").c_str(),
                 &c, err, sizeof(err)) && std::strcmp(err, "profiles[1].sensors[0]: more than 48 different sensors") == 0,
          "49 different sensors over two profiles: its message, as it was");
    CHECK(!parse((R"({"profiles":[)" + p1 + R"("sensors":[)" + items(1, 48) + R"(,{"ieee":"0x1"}]}]})").c_str(), &c, err, sizeof(err)) &&
          std::strcmp(err, "profiles[0].sensors[48]: listed twice") == 0, "a 49th item that is a sensor already listed: listed twice");
    // M34: the attribute names the device layer already uses (pin_code, action_code and color_hex are generic attributes)
    for (const char* id : {"pin_code", "action_code", "color_hex"}) {
        const std::string j = std::string(R"({"profiles":[{"id":")") + id + R"(","name":"A","class":"fire"}]})";
        const std::string msg = std::string("profiles[0]: id ") + id + " is reserved";
        CHECK(!parse(j.c_str(), &c, err, sizeof(err)) && msg == err, ("a reserved id: " + std::string(id)).c_str());
    }
    bad((R"({"profiles":[)" + p1 + R"("outputs":[)" + items(1, 9, w.c_str()) + "]}]}").c_str(), "profiles[0].outputs: at most 8", "nine outputs in a profile");
    std::string five = R"({"profiles":[)";
    for (int i = 0; i < 5; i++)
        five += std::string(i ? "," : "") + R"({"id":"p)" + char('a' + i) + R"(","name":"P","class":"other","outputs":[)" +
                items(1 + 8 * i, i < 4 ? 8 : 1, w.c_str()) + "]}";
    bad((five + "]}").c_str(), "profiles[4].outputs[0]: more than 32 outputs in all", "a 33rd output");
    static AlarmConfig big;
    CHECK(parse((R"({"profiles":[)" + p1 + R"("sensors":[)" + items(1, 48) + "]}]}").c_str(), &big, err, sizeof(err)) && big.n_sensors == 48, "48 sensors are allowed");
    CHECK(!parse1(R"({"sensors":[{"ieee":"0x1","modes":["away"]},{"ieee":"0x1","modes":["home"]}]})", &c, err, sizeof(err)) &&
          std::strstr(err, "sensors[1]: 0x0000000000000001 is listed twice"), "stage 1: a sensor twice keeps its message");
    CHECK(!parse1(R"({"sirens":[{"ieee":"0x1","kind":"horn"}]})", &c, err, sizeof(err)) &&
          std::strstr(err, "sirens[0]: kind must be warning, alarm or state"), "stage 1: a siren kind keeps its message");
}
// The API form must list its profiles: a body without them is not a stage 1 config to convert (review F6, W19).
// M43: the place a sensor has in the list of its first profile, as submitted: the page's row, which an error names.
static void sensor_rows() {
    static AlarmConfig c; char err[160];
    // Slots follow first sight: S1 0, S2 1, S3 2. Profile b lists S2 first, then the S1 that a has, then S3.
    CHECK(parse(R"({"profiles":[{"id":"a","name":"A","class":"fire","sensors":[{"ieee":"0x1"}]},
                  {"id":"b","name":"B","class":"fire","sensors":[{"ieee":"0x2"},{"ieee":"0x1"},{"ieee":"0x3"}]}]})", &c, err, sizeof(err)) &&
          c.n_sensors == 3 && c.sensors[0].ieee == 1 && c.sensors[1].ieee == 2 && c.sensors[2].ieee == 3, "three sensors in the slots of first sight");
    CHECK(c.sensors[0].row == 0 && c.sensors[1].row == 0 && c.sensors[2].row == 2,
          "row: the place in the first profile's list as submitted (S2 is b's first, not its second slot; S1's is a's)");
}
static void config_needs_profiles() {
    static AlarmConfig c, before; char err[160];
    CHECK(parse(R"({"profiles":[{"id":"flood","name":"Flood","class":"water","sensors":[{"ieee":"0x3"}]}],"codes":["1234"]})",
                &c, err, sizeof(err)), "a config to keep");
    std::memcpy(static_cast<void*>(&before), &c, sizeof(c));
    for (const char* j : {"{}", R"({"sensors":[{"ieee":"0x1","modes":["away"]}]})", R"({"profiles":null})", R"({"profiles":{}})"}) {
        std::string what = std::string(j) + ": refused, the config unchanged";
        const bool ok = parse(j, &c, err, sizeof(err), &before);
        CHECK(!ok && std::strcmp(err, "profiles must be a list") == 0 && same(c, before), what.c_str());
    }
}
static void stage1_config() {
    static AlarmConfig c; char err[160];
    const char* old = R"({"exit_delay":45,"entry_delay":20,"siren_time":120,"notify_arm":true,
      "sensors":[{"ieee":"0x1","modes":["away","home","night"],"entry":true},{"ieee":"0x2","modes":["away"],"entry":false}],
      "sirens":[{"ieee":"0x5","kind":"warning","mode":"burglar","level":"very_high","strobe":false},{"ieee":"0x6","kind":"state"}],
      "keypads":[{"ieee":"0x10","trusted":false}],"codes":["4321"]})";
    CHECK(parse1(old, &c, err, sizeof(err)), "a stage 1 config loads");
    CHECK(c.n_profiles == 3 && std::strcmp(c.profiles[0].id, "away") == 0 && std::strcmp(c.profiles[1].id, "home") == 0 &&
          std::strcmp(c.profiles[2].id, "night") == 0 && std::strcmp(c.profiles[2].name, "Night") == 0, "as away, home, night");
    CHECK(c.profiles[0].arm_delay == 45 && c.profiles[1].arm_delay == 0 && c.profiles[2].arm_delay == 0, "the exit delay is away's");
    CHECK(c.profiles[1].trigger_delay == 20 && c.profiles[2].siren_time == 120 && c.profiles[0].notify_arm && c.profiles[2].notify_arm &&
          c.profiles[1].role == AlarmRole::HOME && c.profiles[0].cls == AlarmClass::INTRUSION, "the rest goes to all three");
    CHECK(c.sensors[0].profiles == 0x07 && c.sensors[0].entry == 0x07 && c.sensors[1].profiles == 0x01 && c.sensors[1].entry == 0, "sensors keep their modes");
    CHECK(c.n_outputs == 6 && c.outputs[0].profile == 0 && c.outputs[1].profile == 0 && c.outputs[2].profile == 1 && c.outputs[5].profile == 2 &&
          std::strcmp(c.outputs[4].wd_level, "very_high") == 0 && !c.outputs[4].strobe &&
          c.outputs[1].kind == AlarmOutKind::STATE && c.outputs[1].value && !c.outputs[1].keep, "each siren sounds in all three");
    CHECK(c.n_keypads == 1 && c.n_codes == 1 && std::strcmp(c.codes[0], "4321") == 0, "keypads and codes carry over");
    CHECK(parse1(largest_stage1().c_str(), &c, err, sizeof(err)) && c.n_sensors == 48 && c.n_outputs == 24 &&
          c.sensors[47].profiles == 0x07, "the largest stage 1 config loads whole");
    CHECK(parse1("{}", &c, err, sizeof(err)) && c.n_profiles == 3 && c.profiles[0].arm_delay == 30, "an empty stage 1 config is the defaults");
}

// ── What goes to flash ──

// The largest config the limits allow, in the widest form of each field: 8 profiles with 13-character ids and 24-byte
// names, 48 sensors delayed in all 8, 32 outputs of kind alarm (8 per profile in the first four), 8 keypads, 8 codes.
static void largest_config(AlarmConfig* c) {
    *c = AlarmConfig();
    for (unsigned i = 0; i < 8; i++) {
        AlarmProfile& p = c->profiles[c->n_profiles++];
        std::snprintf(p.id, sizeof(p.id), "profile_%05u", i);
        for (int k = 0; k < 12; k++) std::strcat(p.name, "\xD0\x96");   // 12 two-byte letters: 24 bytes
        p.cls = static_cast<AlarmClass>(i % 5); p.role = i ? AlarmRole::NONE : AlarmRole::AWAY;
        p.arm_delay = p.trigger_delay = 300; p.siren_time = 1800; p.notify_arm = true;
    }
    for (unsigned i = 0; i < 48; i++) c->sensors[c->n_sensors++] = {0x00158D0000000100ULL + i, 0xFF, 0xFF};
    for (unsigned i = 0; i < 32; i++) {
        AlarmOutput& o = c->outputs[c->n_outputs++];
        o.ieee = 0x00158D0000000200ULL + i; o.profile = static_cast<uint8_t>(i / 8); o.kind = AlarmOutKind::ALARM;
        std::snprintf(o.melody, sizeof(o.melody), "melody_%016u", i); std::snprintf(o.volume, sizeof(o.volume), "volume_%016u", i);
    }
    for (unsigned i = 0; i < 8; i++) {
        c->keypads[c->n_keypads++] = {0x00158D0000000010ULL + i, (i & 1) != 0};
        std::snprintf(c->codes[c->n_codes++], kAlarmCodeMax, "%08u", 12345670 + i);
    }
}

static void stored_config() {
    static AlarmConfig c, k; char err[160]; static char buf[kAlarmStoredMax];
    parse(R"({"profiles":[{"id":"away","name":"Away","class":"intrusion","role":"away","arm_delay":30,"trigger_delay":30,
              "sensors":[{"ieee":"0x00158D0001020304","entry":true}],
              "outputs":[{"ieee":"0x5","kind":"warning"},{"ieee":"0x6","kind":"state","value":"off","keep":true}]}],
             "keypads":[{"ieee":"0x10","trusted":true}],"codes":["1234"]})", &c, err, sizeof(err));
    const size_t n = alarm_cfg_pack(c, buf, sizeof(buf));
    CHECK(n && std::strcmp(buf, R"({"v":2,"p":[["away","Away",0,1,30,30,180,0]],"s":[["00158D0001020304",1,1]],)"
          R"("o":[[0,"0000000000000005",0,"burglar","high",1,0],[0,"0000000000000006",2,"","",0,1]],)"
          R"("k":[["0000000000000010",1]],"c":["1234"]})") == 0, "the stored form, byte for byte");
    CHECK(alarm_cfg_unpack(buf, n, &k) && same(c, k), "stored and read back: the same config");
    CHECK(!alarm_cfg_unpack("{\"v\":2,\"p\":7}", 13, &k) && !alarm_cfg_unpack("nonsense", 8, &k), "a damaged record is refused");
    largest_config(&c);
    const size_t m = alarm_cfg_pack(c, buf, sizeof(buf));
    CHECK(m && m <= kAlarmStoredMax && alarm_cfg_unpack(buf, m, &k) && same(c, k) && k.n_sensors == 48 && k.n_outputs == 32 &&
          k.sensors[47].entry == 0xFF, "the largest config fits 8 KB and reads back");
    char small[64];
    CHECK(alarm_cfg_pack(c, small, sizeof(small)) == 0, "a buffer too small: 0, nothing cut");
}
// What a later firmware may add is read past; what would put a number or a text outside its array is refused.
static void stored_config_shape() {
    static AlarmConfig k;
    auto reads = [&](const char* j) { return alarm_cfg_unpack(j, std::strlen(j), &k); };
    CHECK(reads(R"({"v":2,"p":[["a","A",0,0,0,0,180,0,"later"]],"s":[["0000000000000001",1,0,9]],"zz":[1]})") &&
          k.n_profiles == 1 && k.n_sensors == 1, "unknown keys and extra items are ignored");
    CHECK(reads(R"({"v":2,"p":[]})") && k.n_profiles == 0, "a config with no profile reads");
    CHECK(!reads(R"({"v":3,"p":[]})") && !reads(R"({"p":[]})"), "another version, or none, is refused");
    CHECK(!reads(R"({"v":2,"p":[["a","A",5,0,0,0,180,0]]})") && !reads(R"({"v":2,"p":[["a","A",0,4,0,0,180,0]]})"), "a class or role outside its table");
    CHECK(!reads(R"({"v":2,"p":[["a","A",0,0,0,0,180,0]],"o":[[1,"0000000000000005",0,"burglar","high",1,0]]})"), "an output of a profile that is not there");
    CHECK(!reads(R"({"v":2,"p":[["a","A",0,0,0,0,180,0]],"o":[[0,"0000000000000005",3,"","",0,0]]})"), "an output kind outside its table");
    CHECK(!reads(R"({"v":2,"p":[["a","A",0,0,0,0,180,0]],"o":[[0,"0000000000000005",1,"0123456789012345678901234","",0,0]]})"), "a melody of 25 characters");
    CHECK(!reads(R"({"v":2,"p":[["a","A",0,0,0,0,180,0]],"s":[["5",1,0]]})"), "an ieee that is not 16 digits");
    CHECK(!reads(R"({"v":2,"p":[["a","A",0,0,0,0,180,0]],"c":["12"]})"), "a code of 2 digits");
    std::string many = R"({"v":2,"p":[["a","A",0,0,0,0,180,0]],"s":[)";
    for (int i = 0; i < 49; i++) many += std::string(i ? "," : "") + "[\"00000000000000" + char('0' + i / 10) + char('0' + i % 10) + "\",1,0]";
    CHECK(!reads((many + "]}").c_str()), "49 sensors");
}
// A name, a melody with a quote and a backslash, UTF-8: stored and read back as they were.
static void stored_escapes() {
    static AlarmConfig c, k; static char buf[kAlarmStoredMax];
    c = config();
    std::snprintf(c.profiles[kHome].name, sizeof(c.profiles[kHome].name), "Q\"B\\S \xD0\x9D\xD1\x96\xD1\x87");
    AlarmOutput o{}; o.ieee = 5; o.profile = kFlood; o.kind = AlarmOutKind::ALARM; std::snprintf(o.melody, sizeof(o.melody), "a\"b\\c\td");
    c.outputs[c.n_outputs++] = o;
    const size_t n = alarm_cfg_pack(c, buf, sizeof(buf));
    CHECK(n && alarm_cfg_unpack(buf, n, &k) && same(c, k) && std::strcmp(k.profiles[kHome].name, c.profiles[kHome].name) == 0 &&
          std::strcmp(k.outputs[0].melody, o.melody) == 0, "quotes, backslashes, a tab and UTF-8 survive");
    largest_config(&c);   // and the worst a valid config can cost: every character of every text a two-byte escape
    for (uint8_t i = 0; i < c.n_profiles; i++) std::memset(c.profiles[i].name, '"', 24);
    for (uint8_t i = 0; i < c.n_outputs; i++) { std::memset(c.outputs[i].melody, '\\', 23); std::memset(c.outputs[i].volume, '"', 23); }
    const size_t m = alarm_cfg_pack(c, buf, sizeof(buf));
    CHECK(m && m <= kAlarmStoredMax && alarm_cfg_unpack(buf, m, &k) && same(c, k), "the largest config with every text character escaped still fits 8 KB");
}
static void stored_states() {
    AlarmConfig c = config(); AlarmPanel p(&c); uint64_t open = 0; char buf[256]; uint8_t ia = 0, so = 0;
    p.arm(kNight, false, 0, &open); p.arm(kFlood, false, 0, &open); p.sensor(sLeak, AlarmKey::WATER_LEAK, true, 5);
    p.sensor(sDoor, AlarmKey::CONTACT, true, 6);
    const size_t n = alarm_states_pack(c, p, buf, sizeof(buf));
    CHECK(n && std::strcmp(buf, R"({"v":1,"p":[["away",0,0],["home",0,0],["night",3,1],["flood",4,1],["fire",0,0]]})") == 0, "the stored states");
    AlarmPanel q(&c);
    CHECK(alarm_states_unpack(buf, n, c, &q, &ia, &so) && q.at(kNight).state == AlarmState::ARMED && q.at(kFlood).state == AlarmState::ARMED &&
          q.at(kFire).state == AlarmState::DISARMED && ia == ((1u << kNight) | (1u << kFlood)) && so == (1u << kFlood),
          "a restart comes back to the resting states and knows what was in alarm");
    AlarmConfig d = config(); std::snprintf(d.profiles[kFlood].id, sizeof(d.profiles[kFlood].id), "leak"); AlarmPanel r(&d);
    CHECK(alarm_states_unpack(buf, n, d, &r, &ia, &so) && r.at(kFlood).state == AlarmState::DISARMED && r.at(kNight).state == AlarmState::ARMED,
          "a profile the record does not name starts disarmed");
    CHECK(!alarm_states_unpack("{}", 2, c, &r, &ia, &so), "a damaged record is refused");
    AlarmPanel t(&c);
    const char* half = R"({"v":1,"p":[["night",2,1],["flood",9,1]]})";
    CHECK(!alarm_states_unpack(half, std::strlen(half), c, &t, &ia, &so) && t.at(kNight).state == AlarmState::DISARMED && !ia && !so,
          "a record damaged half way restores nothing");
    static AlarmConfig big; static char bigbuf[512];
    largest_config(&big); AlarmPanel w(&big);
    const size_t m = alarm_states_pack(big, w, bigbuf, sizeof(bigbuf));
    CHECK(m && m < 256 && alarm_states_pack(big, w, bigbuf, 20) == 0, "the states of 8 profiles are under 256 bytes; too small: 0");
}
static void stage1_state() {
    AlarmConfig c = config(); uint64_t open = 0; uint8_t rec[2], ia = 0, so = 0;
    { AlarmPanel p(&c); alarm_legacy_record(c, p, rec); CHECK(rec[0] == 0 && rec[1] == 0, "disarmed"); }
    { AlarmPanel p(&c); p.arm(kAway, false, 0, &open); alarm_legacy_record(c, p, rec); CHECK(rec[0] == 1 && rec[1] == 2, "arming away rests armed away"); }
    { AlarmPanel p(&c); p.arm(kNight, false, 0, &open); p.sensor(sWindow, AlarmKey::CONTACT, true, 5); alarm_legacy_record(c, p, rec);
      CHECK(rec[0] == 6 && rec[1] == 4, "night triggered rests armed night"); }
    { AlarmPanel p(&c); p.panic(kPad, 0); alarm_legacy_record(c, p, rec); CHECK(rec[0] == 6 && rec[1] == 0, "a panic from disarmed rests disarmed"); }
    { AlarmPanel p(&c); p.arm(kFlood, false, 0, &open); p.sensor(sLeak, AlarmKey::WATER_LEAK, true, 5); alarm_legacy_record(c, p, rec);
      CHECK(rec[0] == 0 && rec[1] == 0, "a water alarm is not in the stage 1 record"); }
    { AlarmConfig d = config(); std::snprintf(d.profiles[kHome].id, sizeof(d.profiles[kHome].id), "guests"); AlarmPanel p(&d);
      p.arm(kHome, false, 0, &open); alarm_legacy_record(d, p, rec); CHECK(rec[0] == 3 && rec[1] == 3, "another intrusion profile goes by its role");
      d.profiles[kHome].role = AlarmRole::NONE; alarm_legacy_record(d, p, rec); CHECK(rec[0] == 2 && rec[1] == 2, "with no role, as away"); }
    const uint8_t armed_home[2] = {3, 3}, pending_night[2] = {5, 4}, triggered_away[2] = {6, 2}, junk[2] = {9, 9};
    { AlarmPanel p(&c); CHECK(alarm_legacy_restore(armed_home, c, &p, &ia, &so) && p.at(kHome).state == AlarmState::ARMED && !ia && !so, "armed home on stage 1: home armed"); }
    { AlarmPanel p(&c); CHECK(alarm_legacy_restore(pending_night, c, &p, &ia, &so) && p.at(kNight).state == AlarmState::ARMED && ia == (1u << kNight) && !so,
                              "pending on stage 1: armed, and it was in alarm"); }
    { AlarmPanel p(&c); CHECK(alarm_legacy_restore(triggered_away, c, &p, &ia, &so) && p.at(kAway).state == AlarmState::ARMED && so == (1u << kAway), "triggered: it sounded"); }
    { AlarmPanel p(&c); CHECK(!alarm_legacy_restore(junk, c, &p, &ia, &so) && p.at(kAway).state == AlarmState::DISARMED, "a damaged stage 1 record is refused"); }
    const uint8_t panic[2] = {6, 0};
    { AlarmPanel p(&c); CHECK(alarm_legacy_restore(panic, c, &p, &ia, &so) && p.at(kAway).state == AlarmState::DISARMED && ia == (1u << kAway) && so == (1u << kAway),
                              "a panic from disarmed: nothing armed, the first intrusion profile was in alarm"); }
    { AlarmConfig d = config(); std::snprintf(d.profiles[kHome].id, sizeof(d.profiles[kHome].id), "guests"); AlarmPanel p(&d);
      CHECK(alarm_legacy_restore(armed_home, d, &p, &ia, &so) && p.at(kHome).state == AlarmState::ARMED, "no profile named home: the one with the role"); }
}
// The boot rule over both state records. "st2" carries the "st" bytes it was written beside; a "st" that differs at
// boot was written since by the stage 1 firmware (a fall back), and it decides the intrusion profiles.
static void states_of_two_firmwares() {
    AlarmConfig c = config(); uint64_t open = 0; char st2[256]; uint8_t ia = 9, so = 9;
    const uint8_t disarmed[2] = {0, 0}, armed_away[2] = {2, 2}, armed_home[2] = {3, 3}, armed_night[2] = {4, 4};
    const uint8_t alarm_away[2] = {6, 2}, junk[2] = {9, 9};
    auto restore = [&](size_t n, const uint8_t* st, AlarmPanel* p) { return alarm_states_restore(st2, n, st, c, p, &ia, &so); };

    {   // the format: "l" before the profiles, left out when there is no stage 1 record; the old reader skips it
        AlarmPanel p(&c); p.arm(kNight, false, 0, &open);
        const size_t n = alarm_states_pack(c, p, st2, sizeof(st2), armed_night);
        CHECK(n && std::strcmp(st2, R"({"v":1,"l":[4,4],"p":[["away",0,0],["home",0,0],["night",2,1],["flood",0,0],["fire",0,0]]})") == 0,
              "st2 carries the st bytes it was written beside");
        AlarmPanel q(&c);
        CHECK(alarm_states_unpack(st2, n, c, &q, &ia, &so) && q.at(kNight).state == AlarmState::ARMED, "the plain reader ignores them");
    }
    {   // 1. equal bytes: st2 wins, also where st cannot say it (an intrusion profile with no role is "away" there)
        AlarmConfig d = config(); std::snprintf(d.profiles[kHome].id, sizeof(d.profiles[kHome].id), "guests"); d.profiles[kHome].role = AlarmRole::NONE;
        AlarmPanel p(&d); p.arm(kHome, false, 0, &open); p.arm(kFlood, false, 0, &open);
        uint8_t rec[2]; alarm_legacy_record(d, p, rec);
        const size_t n = alarm_states_pack(d, p, st2, sizeof(st2), rec);
        AlarmPanel q(&d);
        CHECK(rec[0] == 2 && alarm_states_restore(st2, n, rec, d, &q, &ia, &so) == AlarmRestored::NEW && q.at(kHome).state == AlarmState::ARMED &&
              q.at(kAway).state == AlarmState::DISARMED && q.at(kFlood).state == AlarmState::ARMED && !ia && !so,
              "equal bytes: every profile from st2");
    }
    {   // 2. a fall back that armed on stage 1
        AlarmPanel p(&c); p.arm(kFlood, false, 0, &open);
        const size_t n = alarm_states_pack(c, p, st2, sizeof(st2), disarmed);
        AlarmPanel q(&c);
        CHECK(restore(n, armed_home, &q) == AlarmRestored::MIXED && q.at(kHome).state == AlarmState::ARMED && q.at(kAway).state == AlarmState::DISARMED &&
              q.at(kFlood).state == AlarmState::ARMED && !ia && !so, "armed on stage 1 since: the hub comes up armed; flood from st2");
        AlarmPanel r(&c);
        CHECK(restore(n, alarm_away, &r) == AlarmRestored::MIXED && r.at(kAway).state == AlarmState::ARMED && ia == (1u << kAway) && so == (1u << kAway),
              "armed and in alarm on stage 1 when it restarted: the restart notice still comes");
    }
    {   // 3. a fall back that disarmed on stage 1, while flood was (and stays) in alarm
        AlarmPanel p(&c); p.arm(kAway, false, 0, &open); p.tick(30); p.sensor(sWindow, AlarmKey::CONTACT, true, 31);
        p.arm(kFlood, false, 40, &open); p.sensor(sLeak, AlarmKey::WATER_LEAK, true, 41);
        const size_t n = alarm_states_pack(c, p, st2, sizeof(st2), alarm_away);
        AlarmPanel q(&c);
        CHECK(p.at(kAway).state == AlarmState::TRIGGERED && restore(n, disarmed, &q) == AlarmRestored::MIXED && q.at(kAway).state == AlarmState::DISARMED &&
              q.at(kFlood).state == AlarmState::ARMED && ia == (1u << kFlood) && so == (1u << kFlood),
              "disarmed on stage 1 since: disarmed, and its alarm is not this boot's; flood from st2 with its alarm");
    }
    {   // 4. a power cut between the two writes: st is of after, st2 of before
        AlarmPanel before(&c); before.arm(kNight, false, 0, &open);
        const size_t n = alarm_states_pack(c, before, st2, sizeof(st2), armed_night);
        AlarmPanel q(&c);   // then night was disarmed and flood armed; only st got written
        CHECK(restore(n, disarmed, &q) == AlarmRestored::MIXED && q.at(kNight).state == AlarmState::DISARMED && q.at(kFlood).state == AlarmState::DISARMED,
              "a power cut between the writes: the intrusion profiles of after, the others of before");
    }
    {   // 5. st2 without the bytes: st2 wins
        AlarmPanel p(&c); p.arm(kNight, false, 0, &open);
        const size_t n = alarm_states_pack(c, p, st2, sizeof(st2));
        AlarmPanel q(&c);
        CHECK(restore(n, disarmed, &q) == AlarmRestored::NEW && q.at(kNight).state == AlarmState::ARMED, "st2 that carries no st bytes wins");
        AlarmPanel r(&c);
        CHECK(restore(n, nullptr, &r) == AlarmRestored::NEW && r.at(kNight).state == AlarmState::ARMED, "no st at all: st2");
    }
    {   // the edges: a stage 1 record that does not read changes nothing; a damaged or missing st2 leaves st, as before
        AlarmPanel p(&c); p.arm(kNight, false, 0, &open);
        const size_t n = alarm_states_pack(c, p, st2, sizeof(st2), armed_night);
        AlarmPanel q(&c);
        CHECK(restore(n, junk, &q) == AlarmRestored::NEW && q.at(kNight).state == AlarmState::ARMED, "a damaged st beside a good st2: st2");
        std::snprintf(st2, sizeof(st2), "{}");
        AlarmPanel r(&c);
        CHECK(restore(2, armed_away, &r) == AlarmRestored::OLD && r.at(kAway).state == AlarmState::ARMED, "a damaged st2: st alone");
        AlarmPanel s(&c);
        CHECK(restore(0, armed_home, &s) == AlarmRestored::OLD && s.at(kHome).state == AlarmState::ARMED, "no st2 (a hub updated from stage 1): st alone");
        AlarmPanel t(&c);
        CHECK(restore(0, nullptr, &t) == AlarmRestored::NOTHING && restore(2, junk, &t) == AlarmRestored::NOTHING && t.at(kAway).state == AlarmState::DISARMED && !ia && !so,
              "no record that reads: nothing restored");
    }
}
static void adopt_states() {
    AlarmConfig c = config(); AlarmPanel p(&c); uint64_t open = 0;
    p.sensor(sWindow, AlarmKey::CONTACT, true, 1); p.arm(kNight, true, 2, &open); p.arm(kFlood, false, 3, &open);
    AlarmConfig d{};   // night kept (window still in it), flood renamed, a new profile first, the leak sensor kept
    d.profiles[d.n_profiles++] = prof("garage", AlarmClass::INTRUSION, AlarmRole::NONE, 0, 0);
    d.profiles[d.n_profiles++] = prof("night",  AlarmClass::INTRUSION, AlarmRole::NIGHT, 0, 30);
    d.profiles[d.n_profiles++] = prof("leak",   AlarmClass::WATER, AlarmRole::NONE, 0, 0);
    d.sensors[d.n_sensors++] = {ieee_of(sLeak), 0x04, 0};
    d.sensors[d.n_sensors++] = {ieee_of(sWindow), 0x02, 0};
    AlarmPanel q(&d); q.adopt(p, c);
    CHECK(q.at(1).state == AlarmState::ARMED && q.at(1).bypass == (1ull << 1), "a kept profile keeps its state and, by device, its bypass");
    CHECK(q.at(0).state == AlarmState::DISARMED && q.at(2).state == AlarmState::DISARMED, "a new id starts disarmed");
    q.disarm(1);
    CHECK(q.arm(1, false, 20, &open) == AlarmArmResult::OPEN && open == (1ull << 1), "the window is still known as open");
    // The hazard check a profile still owes (an arm that has not reached armed yet) survives the change.
    AlarmConfig e = config(); e.sensors[sSmoke].profiles |= 1u << kAway;
    AlarmPanel r(&e);
    r.seed(sSmoke, AlarmKey::SMOKE, true);
    r.arm(kAway, false, 0, &open); r.panic(kPad, 10); r.silence(kAway);
    AlarmConfig f{};
    f.profiles[f.n_profiles++] = prof("away", AlarmClass::INTRUSION, AlarmRole::AWAY, 30, 30);
    f.sensors[f.n_sensors++] = {ieee_of(sSmoke), 0x01, 0};
    AlarmPanel s(&f); s.adopt(r, e);
    CHECK(s.at(0).state == AlarmState::ARMED && s.at(0).alarms == 1, "a silenced panic's profile is adopted armed, with its count");
    s.tick(11);
    CHECK(s.at(0).state == AlarmState::TRIGGERED && s.at(0).cause == ieee_of(sSmoke) && s.at(0).alarms == 2,
          "the hazard check still owed is run at the next tick");
}
// M9: a bypass belongs to the profile the sensor is in. A save that takes the sensor out of an armed profile ends it there,
// and a later save that puts it back does not bring it back.
static void adopt_bypass_leaves_profile() {
    AlarmConfig c = config(); AlarmPanel p(&c); uint64_t open = 0;
    p.sensor(sWindow, AlarmKey::CONTACT, true, 1);
    p.arm(kNight, true, 2, &open);                               // forced: the open window is bypassed in night
    CHECK(p.at(kNight).bypass == (1ull << sWindow), "the window is bypassed in night");
    AlarmConfig d = config(); d.sensors[sWindow].profiles = 0x01;   // a save: the window is in away only (still a sensor of the config)
    AlarmPanel q(&d); q.adopt(p, c);
    CHECK(q.at(kNight).state == AlarmState::ARMED && q.at(kNight).bypass == 0, "a save that takes the sensor out of night leaves no bypass there");
    AlarmPanel r(&c); r.adopt(q, d);                             // a later save puts it back; night is still armed
    CHECK(r.at(kNight).state == AlarmState::ARMED && r.at(kNight).bypass == 0, "put back: not bypassed");
    r.sensor(sWindow, AlarmKey::CONTACT, false, 10); r.sensor(sWindow, AlarmKey::CONTACT, true, 11);
    CHECK(r.at(kNight).state == AlarmState::TRIGGERED && r.at(kNight).cause == ieee_of(sWindow), "and it guards night again");
    AlarmConfig e = config(); AlarmPanel s(&e);                  // a save that keeps it in the profile keeps the bypass
    s.adopt(p, c);
    CHECK(s.at(kNight).bypass == (1ull << sWindow), "a sensor that stays in the profile stays bypassed");
}
static void summary_and_keys() {
    static AlarmConfig c = config(); AlarmPanel p(&c); uint64_t open = 0; char s[24]; AlarmKey k;
    alarm_summary_name(c, p, s, sizeof(s)); CHECK(std::strcmp(s, "disarmed") == 0, "summary: disarmed");
    p.arm(kFlood, false, 0, &open); p.sensor(sLeak, AlarmKey::WATER_LEAK, true, 1);
    alarm_summary_name(c, p, s, sizeof(s)); CHECK(std::strcmp(s, "disarmed") == 0, "a water alarm does not touch the summary");
    p.arm(kAway, false, 10, &open); alarm_summary_name(c, p, s, sizeof(s)); CHECK(std::strcmp(s, "arming") == 0, "summary: arming");
    p.tick(40); alarm_summary_name(c, p, s, sizeof(s)); CHECK(std::strcmp(s, "armed_away") == 0, "summary: armed_away, as in stage 1");
    p.arm(kNight, false, 50, &open); alarm_summary_name(c, p, s, sizeof(s)); CHECK(std::strcmp(s, "armed_night") == 0, "a switch goes straight to the new value");
    p.sensor(sDoor, AlarmKey::CONTACT, true, 60); alarm_summary_name(c, p, s, sizeof(s)); CHECK(std::strcmp(s, "pending") == 0, "summary: pending");
    p.tick(90); alarm_summary_name(c, p, s, sizeof(s)); CHECK(std::strcmp(s, "triggered") == 0, "summary: triggered");
    CHECK(alarm_key_parse("contact", &k) && k == AlarmKey::CONTACT && alarm_key_parse("vibration", &k) && k == AlarmKey::MOTION &&
          alarm_key_parse("presence", &k) && k == AlarmKey::MOTION && alarm_key_parse("carbon_monoxide", &k) && k == AlarmKey::CO &&
          alarm_key_parse("water_leak", &k) && k == AlarmKey::WATER_LEAK && !alarm_key_parse("tamper", &k) && !alarm_key_parse("alarm", &k), "sensor keys");
}
static void changes_under_load() {
    static AlarmConfig c = config(); AlarmPanel p(&c); uint64_t open = 0; AlarmSeen seen[kAlarmMaxProfiles]; AlarmChange ch[kAlarmMaxProfiles];
    alarm_seen(p, seen);
    CHECK(alarm_changes(p, seen, ch) == 0, "nothing happened: no change");
    // Everything below happens before the task looks again.
    p.arm(kNight, false, 0, &open); p.arm(kFlood, false, 0, &open);
    p.sensor(sDoor, AlarmKey::CONTACT, true, 10);                           // night: pending until 40
    p.sensor(sLeak, AlarmKey::WATER_LEAK, true, 20); p.silence(kFlood);     // flood: an alarm that came and went
    p.tick(40);                                                             // night: triggered
    uint8_t n = alarm_changes(p, seen, ch);
    CHECK(n == 2 && ch[0].profile == kNight && ch[0].from == AlarmState::DISARMED && ch[0].to == AlarmState::TRIGGERED && ch[0].alarm &&
          ch[0].cause == ieee_of(sDoor), "skipped steps collapse into one change with the final state");
    CHECK(ch[1].profile == kFlood && ch[1].from == AlarmState::DISARMED && ch[1].to == AlarmState::ARMED && ch[1].alarm &&
          ch[1].cause == ieee_of(sLeak), "an alarm that is over again is still reported, with its cause");
    alarm_seen(p, seen);
    p.silence(kNight); p.sensor(sWindow, AlarmKey::CONTACT, true, 50);     // night: triggered, armed, triggered again
    n = alarm_changes(p, seen, ch);
    CHECK(n == 1 && ch[0].from == AlarmState::TRIGGERED && ch[0].to == AlarmState::TRIGGERED && ch[0].alarm && ch[0].cause == ieee_of(sWindow),
          "a second alarm behind the same state is a change");
    alarm_seen(p, seen);
    p.disarm_all();
    n = alarm_changes(p, seen, ch);
    CHECK(n == 1 && ch[0].profile == kNight && ch[0].to == AlarmState::DISARMED && ch[0].by_command && !ch[0].alarm, "a disarm by command");
    alarm_seen(p, seen);
    p.sensor(sWindow, AlarmKey::CONTACT, false, 55); p.sensor(sDoor, AlarmKey::CONTACT, false, 56);
    p.arm(kHome, false, 60, &open); alarm_seen(p, seen); p.arm(kNight, false, 70, &open);
    n = alarm_changes(p, seen, ch);
    CHECK(n == 2 && ch[0].profile == kHome && ch[0].to == AlarmState::DISARMED && !ch[0].by_command && ch[1].to == AlarmState::ARMED,
          "being replaced is not a disarm by command");
    alarm_seen(p, seen);
    p.disarm(kNight); p.panic(kPad, 80); p.tick(260);
    n = alarm_changes(p, seen, ch);
    CHECK(n == 2 && ch[0].profile == kAway && ch[0].from == AlarmState::DISARMED && ch[0].to == AlarmState::DISARMED && ch[0].alarm && !ch[0].by_command,
          "a panic alarm that ended by itself: an alarm, not a disarm");
}
static void event_names() {
    static AlarmConfig c = config(); char ev[20][kAlarmEventMax];
    const AlarmChange a[] = {{kNight, AlarmState::DISARMED, AlarmState::ARMED, 0, false, false}};
    uint8_t n = alarm_events(c, a, 1, "disarmed", "armed_night", ev, 20);
    CHECK(n == 1 && std::strcmp(ev[0], "alarm_armed_night") == 0, "armed: one event, the stage 1 name");
    const AlarmChange b[] = {{kFlood, AlarmState::ARMED, AlarmState::TRIGGERED, 5, true, false}};
    n = alarm_events(c, b, 1, "armed_night", "armed_night", ev, 20);
    CHECK(n == 1 && std::strcmp(ev[0], "alarm_triggered_flood") == 0, "a water alarm: its own event, no stage 1 event");
    const AlarmChange d[] = {{kNight, AlarmState::ARMED, AlarmState::TRIGGERED, 5, true, false}};
    n = alarm_events(c, d, 1, "armed_night", "triggered", ev, 20);
    CHECK(n == 2 && std::strcmp(ev[0], "alarm_triggered_night") == 0 && std::strcmp(ev[1], "alarm_triggered") == 0, "an intrusion alarm: both names, once each");
    const AlarmChange e[] = {{kHome, AlarmState::ARMED, AlarmState::DISARMED, 0, false, false}, {kNight, AlarmState::DISARMED, AlarmState::ARMED, 0, false, false}};
    n = alarm_events(c, e, 2, "armed_home", "armed_night", ev, 20);
    CHECK(n == 2 && std::strcmp(ev[0], "alarm_disarmed_home") == 0 && std::strcmp(ev[1], "alarm_armed_night") == 0, "a switch: no stage 1 alarm_disarmed");
    const AlarmChange f[] = {{kFlood, AlarmState::ARMED, AlarmState::ARMED, 5, true, false}};
    n = alarm_events(c, f, 1, "disarmed", "disarmed", ev, 20);
    CHECK(n == 1 && std::strcmp(ev[0], "alarm_triggered_flood") == 0, "an alarm that came and went still fires its event");
    const AlarmChange g[] = {{kAway, AlarmState::ARMED, AlarmState::DISARMED, 0, false, true}};
    n = alarm_events(c, g, 1, "armed_away", "disarmed", ev, 20);
    CHECK(n == 2 && std::strcmp(ev[0], "alarm_disarmed_away") == 0 && std::strcmp(ev[1], "alarm_disarmed") == 0, "a disarm: its own and the stage 1 event");
    n = alarm_events(c, e, 2, "armed_home", "armed_night", ev, 1);
    CHECK(n == 1 && std::strcmp(ev[0], "alarm_disarmed_home") == 0, "a full list takes no more names");
}
static void texts() {
    char t[64];
    alarm_text(AlarmText::TRIGGERED, "Away", "Front door", 0, t, sizeof(t)); CHECK(std::strcmp(t, "Front door (Away)") == 0, "triggered");
    alarm_text(AlarmText::PANIC, "Away", "Hall keypad", 0, t, sizeof(t)); CHECK(std::strcmp(t, "Panic button on Hall keypad") == 0, "panic");
    alarm_text(AlarmText::REFUSED, "Away", "Bathroom window", 0, t, sizeof(t)); CHECK(std::strcmp(t, "Not armed (Away): Bathroom window is open") == 0, "refused, one");
    alarm_text(AlarmText::REFUSED, "Away", "Bathroom window", 2, t, sizeof(t)); CHECK(std::strcmp(t, "Not armed (Away): Bathroom window and 2 more are open") == 0, "refused, more");
    alarm_text(AlarmText::ARMED, "Night", "", 0, t, sizeof(t)); CHECK(std::strcmp(t, "Armed (Night)") == 0, "armed");
    alarm_text(AlarmText::DISARMED, "Fire", "", 0, t, sizeof(t)); CHECK(std::strcmp(t, "Disarmed (Fire)") == 0, "disarmed");
    alarm_text(AlarmText::RESTARTED, "", "", 0, t, sizeof(t)); CHECK(std::strcmp(t, "The hub restarted during an alarm") == 0, "restarted");
    char cut[15];   // "Датчик " is 13 bytes; the 14 that fit would end inside the next Cyrillic letter
    const size_t n = alarm_text(AlarmText::TRIGGERED, "Ніч", "Датчик вікна", 0, cut, sizeof(cut));
    CHECK(n == std::strlen(cut) && n < sizeof(cut) && utf8_valid(cut, n), "a text that does not fit is cut between characters");
    char edge[13];   // the 12 bytes that fit are "Датчик": whole characters, so nothing is dropped
    CHECK(alarm_text(AlarmText::TRIGGERED, "Ніч", "Датчик вікна", 0, edge, sizeof(edge)) == 12 && std::strcmp(edge, "Датчик") == 0, "a cut on a character edge keeps everything that fits");
    char three[4];   // "a" and the first two bytes of the euro sign
    CHECK(alarm_text(AlarmText::TRIGGERED, "P", "a\xE2\x82\xAC" "b", 0, three, sizeof(three)) == 1 && std::strcmp(three, "a") == 0, "a three-byte character cut after two bytes is dropped");
}
static void outputs_plan() {
    static AlarmConfig c = config(); uint64_t open = 0; AlarmDrive d[kAlarmMaxOutputs];
    auto out = [&](uint8_t profile, uint64_t ieee, AlarmOutKind kind, bool value = true, bool keep = false) {
        AlarmOutput o{}; o.profile = profile; o.ieee = ieee; o.kind = kind; o.value = value; o.keep = keep; c.outputs[c.n_outputs++] = o; };
    out(kNight, 0xA1, AlarmOutKind::WARNING);                       // 0: the hall siren, intrusion
    out(kFlood, 0xB1, AlarmOutKind::STATE, false, true);            // 1: the valve
    out(kFlood, 0xA1, AlarmOutKind::WARNING);                       // 2: the hall siren, water
    out(kFire,  0xA1, AlarmOutKind::WARNING);                       // 3: the hall siren, fire
    AlarmPanel p(&c);
    uint8_t n = alarm_drive_plan(c, p, 0, d);
    CHECK(n == 2 && d[0].ieee == 0xA1 && d[0].output == -1 && d[1].ieee == 0xB1 && d[1].output == -1, "one entry per device; nothing sounds");
    p.arm(kNight, false, 0, &open); p.arm(kFlood, false, 0, &open); p.arm(kFire, false, 0, &open);
    p.sensor(sWindow, AlarmKey::CONTACT, true, 100);               // night until 280
    alarm_drive_plan(c, p, 110, d);
    CHECK(d[0].output == 0 && d[0].secs == 170 && d[1].output == -1, "the siren follows the intrusion alarm, for the time left");
    p.sensor(sLeak, AlarmKey::WATER_LEAK, true, 120);              // flood until 300
    alarm_drive_plan(c, p, 120, d);
    CHECK(d[0].output == 2 && d[0].secs == 180 && d[1].output == 1, "water outranks intrusion; the valve acts");
    p.sensor(sSmoke, AlarmKey::SMOKE, true, 130);                  // fire until 310
    alarm_drive_plan(c, p, 130, d);
    CHECK(d[0].output == 3 && d[0].secs == 180, "fire outranks water");
    p.silence(kFire);
    alarm_drive_plan(c, p, 140, d);
    CHECK(d[0].output == 2 && d[0].secs == 160, "fire ends: water takes the siren over for its time left");
    p.tick(300);
    alarm_drive_plan(c, p, 300, d);
    CHECK(d[0].output == -1 && d[1].output == -1, "all alarms over: nothing sounds");
}
// M14: one siren that all five classes list: the class decides, whatever the order of the profiles.
static void outputs_plan_classes() {
    // fire > gas > water > other > intrusion; the profiles are listed from the lowest rank, so a mix-up of the index and the rank shows
    static const AlarmClass kOrder[] = {AlarmClass::INTRUSION, AlarmClass::OTHER, AlarmClass::WATER, AlarmClass::GAS, AlarmClass::FIRE};
    static const char* const kWin[] = {"intrusion", "other", "water", "gas", "fire"};
    AlarmConfig c{}; uint64_t open = 0; AlarmDrive d[kAlarmMaxOutputs];
    for (uint8_t i = 0; i < 5; i++) {
        char id[8]; std::snprintf(id, sizeof(id), "p%u", i);
        c.profiles[c.n_profiles++] = prof(id, kOrder[i], AlarmRole::NONE, 0, 0);
        AlarmOutput o{}; o.profile = i; o.ieee = 0xA1; o.kind = AlarmOutKind::WARNING; c.outputs[c.n_outputs++] = o;
    }
    c.sensors[c.n_sensors++] = {0x5E, 0x1F, 0};        // one contact in every profile, not delayed
    AlarmPanel p(&c);
    for (uint8_t i = 0; i < 5; i++) p.arm(i, false, 0, &open);
    p.sensor(0, AlarmKey::CONTACT, true, 10);
    bool ok = true;
    for (int top = 4; top >= 0; top--) {   // fire wins; silence it and gas does; and so down to intrusion
        const uint8_t n = alarm_drive_plan(c, p, 20, d);
        ok = ok && n == 1 && d[0].output == top && c.profiles[d[0].output].cls == kOrder[top];
        if (!ok) std::printf("  step %s: n=%u output=%d\n", kWin[top], n, n ? d[0].output : -9);
        p.silence(static_cast<uint8_t>(top));
    }
    CHECK(ok, "five profiles on one siren: fire, gas, water, other, intrusion take it over in that order");
    alarm_drive_plan(c, p, 20, d);
    CHECK(d[0].output == -1, "and when all five are silenced nothing sounds");
}
static void outputs_keep() {
    static AlarmConfig c = config();
    auto out = [&](uint8_t profile, uint64_t ieee, AlarmOutKind kind, bool value, bool keep) {
        AlarmOutput o{}; o.profile = profile; o.ieee = ieee; o.kind = kind; o.value = value; o.keep = keep; c.outputs[c.n_outputs++] = o; };
    out(kNight, 0xC1, AlarmOutKind::STATE, true, false);    // 0: a lamp, switched back off
    out(kNight, 0xA1, AlarmOutKind::WARNING, true, false);  // 1: the siren, intrusion
    out(kFlood, 0xB1, AlarmOutKind::STATE, false, true);    // 2: the valve, left closed
    out(kFire,  0xB1, AlarmOutKind::STATE, false, true);    // 3: the same valve in another profile
    out(kFire,  0xA1, AlarmOutKind::WARNING, true, false);  // 4: the siren, fire
    CHECK(alarm_output_step(c, -1, 2) == AlarmOutAct::START, "the valve closes when the alarm starts");
    CHECK(alarm_output_step(c, 2, -1) == AlarmOutAct::NONE, "and is left closed when it ends");
    CHECK(alarm_output_step(c, 2, 3) == AlarmOutAct::NONE && alarm_output_step(c, 3, -1) == AlarmOutAct::NONE,
          "another profile taking the valve over, and ending, sends nothing");
    CHECK(alarm_output_step(c, -1, 0) == AlarmOutAct::START && alarm_output_step(c, 0, -1) == AlarmOutAct::STOP, "a lamp is switched back");
    CHECK(alarm_output_step(c, 1, 4) == AlarmOutAct::START && alarm_output_step(c, 4, 4) == AlarmOutAct::NONE &&
          alarm_output_step(c, 4, -1) == AlarmOutAct::STOP && alarm_output_step(c, -1, -1) == AlarmOutAct::NONE, "a siren: new signal, same signal, stop, nothing");
}
int main() {
    transitions();
    arming_rules();
    edges();
    hazards();
    kept_changes();
    group();
    silence_and_disarm_all();
    panic_and_restore();
    panic_before_armed();
    keypad_codes();
    names();
    siren_fit();
    config_json();
    class_words();
    config_errors();
    config_needs_profiles();
    sensor_rows();
    stage1_config();
    stored_config();
    stored_config_shape();
    stored_escapes();
    stored_states();
    stage1_state();
    states_of_two_firmwares();
    adopt_states();
    adopt_bypass_leaves_profile();
    summary_and_keys();
    changes_under_load();
    event_names();
    texts();
    outputs_plan();
    outputs_plan_classes();
    outputs_keep();
    std::printf("%s (%d failed)\n", g_fail ? "FAILED" : "ALL PASSED", g_fail);
    return g_fail ? 1 : 0;
}
