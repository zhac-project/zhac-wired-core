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
    const std::string p1 = R"({"id":"a","name":"A","class":"fire",)", w = R"(,"kind":"warning")";
    bad((R"({"profiles":[)" + p1 + R"("sensors":[)" + items(1, 48) + R"(]},{"id":"b","name":"B","class":"gas","sensors":[)" + items(49, 1) + "]}]}").c_str(),
        "profiles[1].sensors[0]: more than 48 different sensors", "a 49th different sensor");
    bad((R"({"profiles":[)" + p1 + R"("outputs":[)" + items(1, 9, w.c_str()) + "]}]}").c_str(), "profiles[0].outputs: at most 8", "nine outputs in a profile");
    std::string five = R"({"profiles":[)";
    for (int i = 0; i < 5; i++)
        five += std::string(i ? "," : "") + R"({"id":"p)" + char('a' + i) + R"(","name":"P","class":"other","outputs":[)" +
                items(1 + 8 * i, i < 4 ? 8 : 1, w.c_str()) + "]}";
    bad((five + "]}").c_str(), "profiles[4].outputs[0]: more than 32 outputs in all", "a 33rd output");
    static AlarmConfig big;
    CHECK(parse((R"({"profiles":[)" + p1 + R"("sensors":[)" + items(1, 48) + "]}]}").c_str(), &big, err, sizeof(err)) && big.n_sensors == 48, "48 sensors are allowed");
    bad(R"({"sensors":[{"ieee":"0x1","modes":["away"]},{"ieee":"0x1","modes":["home"]}]})", "sensors[1]: 0x0000000000000001 is listed twice", "stage 1: a sensor twice keeps its message");
    bad(R"({"sirens":[{"ieee":"0x1","kind":"horn"}]})", "sirens[0]: kind must be warning, alarm or state", "stage 1: a siren kind keeps its message");
}
static void stage1_config() {
    static AlarmConfig c; char err[160];
    const char* old = R"({"exit_delay":45,"entry_delay":20,"siren_time":120,"notify_arm":true,
      "sensors":[{"ieee":"0x1","modes":["away","home","night"],"entry":true},{"ieee":"0x2","modes":["away"],"entry":false}],
      "sirens":[{"ieee":"0x5","kind":"warning","mode":"burglar","level":"very_high","strobe":false},{"ieee":"0x6","kind":"state"}],
      "keypads":[{"ieee":"0x10","trusted":false}],"codes":["4321"]})";
    CHECK(parse(old, &c, err, sizeof(err)), "a stage 1 config loads");
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
    CHECK(parse(largest_stage1().c_str(), &c, err, sizeof(err)) && c.n_sensors == 48 && c.n_outputs == 24 &&
          c.sensors[47].profiles == 0x07, "the largest stage 1 config loads whole");
    CHECK(parse("{}", &c, err, sizeof(err)) && c.n_profiles == 3 && c.profiles[0].arm_delay == 30, "an empty stage 1 config is the defaults");
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
    config_json();
    config_errors();
    stage1_config();
    std::printf("%s (%d failed)\n", g_fail ? "FAILED" : "ALL PASSED", g_fail);
    return g_fail ? 1 : 0;
}
