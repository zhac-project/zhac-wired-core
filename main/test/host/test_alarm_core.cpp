// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Alarm panel (spec extra/docs/superpowers/specs/2026-11-24-safety-profiles-design.md):
// every state transition of a profile, the delays on a fake clock, the arming check and
// force, the bypass, edges and hazards, intrusion profiles replacing each other, silence,
// disarm all, panic, restore, the keypad code policy and the output fit.
#include "alarm_core.h"

#include <cstdio>
#include <cstring>

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

int main() {
    transitions();
    arming_rules();
    edges();
    hazards();
    group();
    silence_and_disarm_all();
    panic_and_restore();
    panic_before_armed();
    keypad_codes();
    names();
    siren_fit();
    std::printf("%s (%d failed)\n", g_fail ? "FAILED" : "ALL PASSED", g_fail);
    return g_fail ? 1 : 0;
}
