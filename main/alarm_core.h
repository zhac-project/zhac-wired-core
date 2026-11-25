// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Alarm panel ("Safety"): the profiles, one state machine per profile and the config.
// Pure (no IDF), host-tested in test/host/test_alarm_core.cpp. The glue -- sensors,
// outputs, keypads, notifications, NVS and the Alarm device -- is alarm.cpp.
// Spec: extra/docs/superpowers/specs/2026-11-24-safety-profiles-design.md.
#pragma once
#include <cstddef>
#include <cstdint>

#include <ArduinoJson.h>

enum class AlarmState   : uint8_t { DISARMED = 0, ARMING, ARMED, PENDING, TRIGGERED };   // of one profile
enum class AlarmClass   : uint8_t { INTRUSION = 0, FIRE, WATER, GAS, OTHER };
enum class AlarmRole    : uint8_t { NONE = 0, AWAY, HOME, NIGHT };                     // the keypad button; intrusion only
// The key an output is driven by: IAS WD `warning`, a Tuya DP siren's `alarm`, a plug or relay's `state`.
enum class AlarmOutKind : uint8_t { WARNING = 0, ALARM, STATE };
enum class AlarmKey     : uint8_t { CONTACT = 0, MOTION, WATER_LEAK, SMOKE, GAS, CO };  // MOTION: occupancy, presence, vibration
enum class AlarmArmResult : uint8_t {
    OK,
    OPEN,   // contact sensors of the profile are open and force was not given
    BUSY,   // pending or triggered (this profile, or for an intrusion profile another one): disarm first
};

constexpr uint8_t kAlarmMaxProfiles = 8, kAlarmMaxSensors = 48, kAlarmMaxProfileOutputs = 8, kAlarmMaxOutputs = 32;
constexpr uint8_t kAlarmMaxKeypads = 8, kAlarmMaxCodes = 8;
constexpr size_t  kAlarmIdMax = 14, kAlarmNameMax = 25;   // with the NUL
constexpr size_t  kAlarmCodeMax    = 9;    // 4-8 digits + NUL
constexpr size_t  kAlarmEnumMax    = 24;   // a melody / volume value as the siren's expose names it
constexpr uint16_t kAlarmDelayMax  = 300;  // arm / trigger delay, s
constexpr uint16_t kAlarmSirenMin  = 10;   // siren time, s
constexpr uint16_t kAlarmSirenMax  = 1800;

struct AlarmProfile {
    char       id[kAlarmIdMax];
    char       name[kAlarmNameMax];
    AlarmClass cls;
    AlarmRole  role;
    uint16_t   arm_delay, trigger_delay, siren_time;   // s
    bool       notify_arm;                             // also notify on arm and disarm
};

struct AlarmSensor {
    uint64_t ieee;
    uint8_t  profiles;   // bit i: in profile i
    uint8_t  entry;      // bit i: delayed in profile i (starts pending instead of the alarm)
    // Where the API form listed it in its first profile (the page's row, which an error names). Set by
    // alarm_cfg_from_json only: the stored form and the stage 1 form do not keep it, and nothing but a message reads it.
    uint8_t  row = 0;
};

struct AlarmOutput {
    uint64_t     ieee;
    uint8_t      profile;
    AlarmOutKind kind;
    // WARNING: z2m's warning object (the duration is the siren time).
    char         wd_mode[16];    // burglar fire emergency police_panic fire_panic emergency_panic
    char         wd_level[12];   // low medium high very_high
    bool         strobe;
    // ALARM: written before alarm=true when not empty.
    char         melody[kAlarmEnumMax];
    char         volume[kAlarmEnumMax];
    // STATE: the value set at the alarm; keep: left so when the alarm ends.
    bool         value;
    bool         keep;
};

struct AlarmKeypad {
    uint64_t ieee;
    bool     trusted;   // may disarm without a code (a key fob)
};

struct AlarmConfig {
    uint8_t      n_profiles = 0, n_sensors = 0, n_outputs = 0, n_keypads = 0, n_codes = 0;
    AlarmProfile profiles[kAlarmMaxProfiles] = {};
    AlarmSensor  sensors[kAlarmMaxSensors] = {};
    AlarmOutput  outputs[kAlarmMaxOutputs] = {};   // grouped by profile, each group in its listed order
    AlarmKeypad  keypads[kAlarmMaxKeypads] = {};
    char         codes[kAlarmMaxCodes][kAlarmCodeMax] = {};
};

const char* alarm_state_name(AlarmState s);                      // disarmed arming armed pending triggered
bool        alarm_state_parse(const char* s, AlarmState* out);
int  alarm_cfg_profile(const AlarmConfig& c, const char* id);   // index, -1
int  alarm_cfg_role(const AlarmConfig& c, AlarmRole r);         // the intrusion profile with that role, -1
int  alarm_cfg_sensor(const AlarmConfig& c, uint64_t ieee);     // slot, -1
const AlarmKeypad* alarm_cfg_keypad(const AlarmConfig& c, uint64_t ieee);

// The API form (alarm.config.set, alarm.get). "profiles" must be a list: without one, the body is refused before
// `out` is touched. "codes" left out keeps `keep`'s codes; "codes":[] clears them. False with the first problem in
// `err`. `out` is built in place (an AlarmConfig is ~4.4 KB: never a local on a firmware stack) and is partly filled
// on any later failure. `pool`: where the parse takes its memory; null is the default allocator. The firmware gives
// PSRAM: the largest config parses to tens of KB.
bool alarm_cfg_from_json(const char* json, size_t len, const AlarmConfig* keep, AlarmConfig* out, char* err, size_t cap,
                         ArduinoJson::Allocator* pool = nullptr);
// The stage 1 record ("cfg"), read at boot only: an object without "profiles" is the stage 1 form and is converted
// (one with them reads as the API form). Otherwise as above, with no codes to keep.
bool alarm_cfg_from_stage1(const char* json, size_t len, AlarmConfig* out, char* err, size_t cap,
                           ArduinoJson::Allocator* pool = nullptr);
void alarm_cfg_write(const AlarmConfig& c, bool with_codes, JsonObject o);   // with_codes false: "codes_count"
void alarm_cfg_defaults(AlarmConfig* out);                                   // a hub with no record: away, home, night

// May this keypad disarm with this code? A trusted keypad always may; otherwise the
// code must be one of the configured ones, and with none configured nobody may.
bool alarm_keypad_may_disarm(const AlarmConfig& c, uint64_t keypad, const char* code);

// Why an output of `kind` cannot act on a device with these exposes (the JSON array the adapter
// builds), or nullptr when it can: warning needs a settable `warning`, alarm a settable `alarm`
// (a smoke sensor's alarm is only a reading), state a settable `state`.
const char* alarm_output_misfit(AlarmOutKind kind, const char* exposes, size_t len);

struct AlarmProfileStatus {
    AlarmState state = AlarmState::DISARMED;
    bool       rests_armed = false;   // where pending and triggered end: armed, or disarmed after a panic from disarmed
    uint64_t   cause = 0;             // the sensor or keypad behind pending / triggered, else 0
    uint64_t   alarm_cause = 0;       // of the latest triggered; kept after it ends
    uint32_t   deadline = 0;          // end of arming / pending / triggered
    uint32_t   alarms = 0;            // entries to triggered so far
    uint32_t   disarms = 0;           // disarms by a command so far; being replaced by another profile is not one
    uint64_t   bypass = 0;            // bit s: sensor slot s is ignored until disarm
};

// All profiles of one config. Time is monotonic seconds, passed in. No locking, no I/O.
// The glue compares at() before and after each call and acts on the change.
class AlarmPanel {
public:
    explicit AlarmPanel(const AlarmConfig* cfg) : cfg_(cfg) {}
    void seed(uint8_t slot, AlarmKey key, bool active);                   // a last known value: never an event
    // A report; CONTACT: active = open. True when it changed the value kept for a contact or a hazard (motion keeps
    // none): what a restart seeds the panel with, so the glue has the shadow write it to flash at once.
    bool sensor(uint8_t slot, AlarmKey key, bool active, uint32_t now);
    // `open`: the profile's sensor slots whose contact is open now (set for OPEN and for a forced arm).
    // Arming an intrusion profile disarms the other intrusion profiles (not counted as disarms).
    AlarmArmResult arm(uint8_t profile, bool force, uint32_t now, uint64_t* open);
    void disarm(uint8_t profile);
    void disarm_all();                               // intrusion profiles disarmed, the others silenced
    void silence(uint8_t profile);                   // pending / triggered end now
    bool panic(uint64_t source, uint32_t now);       // false: there is no intrusion profile
    void tick(uint32_t now);
    void restore(uint8_t profile, bool armed);       // at boot
    // After a config change, on a panel made for the new config: each profile whose id the old config had takes its
    // state (the caller makes sure nothing is running), counters and, by ieee, bypass; each sensor both have keeps
    // its last values. The rest starts as a new panel does.
    void adopt(const AlarmPanel& old, const AlarmConfig& old_cfg);
    bool rests_armed(uint8_t profile) const;         // what a restart comes back to
    const AlarmProfileStatus& at(uint8_t profile) const;
    bool running() const;                            // any profile arming, pending or triggered
    int  summary_profile() const;                    // the intrusion profile that is not disarmed, -1

private:
    enum class Contact : uint8_t { UNKNOWN = 0, OPEN, CLOSED };
    struct Last { Contact contact = Contact::UNKNOWN; uint8_t hazards = 0; };   // hazards: bit (key - WATER_LEAK)

    bool intrusion(uint8_t profile) const;
    bool remember(uint8_t slot, AlarmKey key, bool active, bool* changed);   // true: the report counts
    void go_off(uint8_t profile, uint8_t slot, uint32_t now);
    void hazard_check(uint8_t profile, uint32_t now);
    void trigger(uint8_t profile, uint64_t cause, uint32_t now);
    void end_alarm(uint8_t profile);
    void stop(uint8_t profile);

    const AlarmConfig* cfg_;
    AlarmProfileStatus st_[kAlarmMaxProfiles] = {};
    // This arm has not reached armed yet: the hazard check runs when it does. A panic during
    // the arm delay keeps it due, so the check runs when that alarm ends in armed.
    bool               check_due_[kAlarmMaxProfiles] = {};
    Last               last_[kAlarmMaxSensors] = {};
};

// What goes to flash (NVS namespace "alarm"). The stage 1 records "cfg" and "st" are not these: "cfg" is never rewritten,
// "st" is kept current by alarm_legacy_record so that a fall back to the stage 1 firmware finds the state of now.
constexpr size_t kAlarmStoredMax = 8192;   // the largest config the limits allow, in its stored form, fits
// The stored config ("cfg2"): arrays without key names, each sensor once with the set of its profiles. Returns the length,
// or 0 when it does not fit `cap` (with its NUL): nothing is cut. Unknown keys and extra array items are ignored when read.
size_t alarm_cfg_pack(const AlarmConfig& c, char* buf, size_t cap);
// Built in place, partly filled on failure, and with `pool`, like alarm_cfg_from_json.
bool   alarm_cfg_unpack(const char* json, size_t len, AlarmConfig* out, ArduinoJson::Allocator* pool = nullptr);
// The stored states ("st2"): {"v":1,"l":[state, resting],"p":[[id, state, rests_armed], ...]} for every profile.
// "l" is the stage 1 record ("st") as flash holds it when this is written; left out when `st` is null.
size_t alarm_states_pack(const AlarmConfig& c, const AlarmPanel& p, char* buf, size_t cap, const uint8_t* st = nullptr);
// The same from a copy (the alarm task's look): profile i is `prof[i].id`, in `state[i]`, resting armed when `rests[i]`.
size_t alarm_states_pack(const AlarmProfile* prof, const AlarmState* state, const bool* rests, uint8_t n, char* buf, size_t cap,
                         const uint8_t* st = nullptr);
// Restores each profile named in the record that the config still has; a damaged record restores nothing. `in_alarm` /
// `sounding`: bit i set when profile i was pending or triggered / triggered when the record was written.
bool   alarm_states_unpack(const char* json, size_t len, const AlarmConfig& c, AlarmPanel* p, uint8_t* in_alarm, uint8_t* sounding);
// The stage 1 state record ("st"): {state, resting} in the stage 1 numbers (0 disarmed, 1 arming, 2 armed_away,
// 3 armed_home, 4 armed_night, 5 pending, 6 triggered), describing summary_profile(); {0, 0} with none.
void   alarm_legacy_record(const AlarmConfig& c, const AlarmPanel& p, uint8_t rec[2]);
bool   alarm_legacy_restore(const uint8_t rec[2], const AlarmConfig& c, AlarmPanel* p, uint8_t* in_alarm, uint8_t* sounding);
// What a boot restores from both records (`st2` with len 0: there is none; `st` null: there is none).
enum class AlarmRestored : uint8_t {
    NOTHING,   // no record that reads: every profile starts disarmed
    NEW,       // every profile from "st2"
    MIXED,     // "st2", but the intrusion profiles from a "st" the stage 1 firmware wrote after it
    OLD,       // "st" alone: there is no "st2", or it does not read
};
// "st2" restores every profile. When the "st" beside it is not the one it was written with ("l"), the hub ran the
// stage 1 firmware since (a fall back) and was armed or disarmed there: the intrusion profiles, and their bits of
// `in_alarm` and `sounding`, then come from "st". A "st2" that carries no "l" wins as it is.
AlarmRestored alarm_states_restore(const char* st2, size_t len, const uint8_t* st, const AlarmConfig& c, AlarmPanel* p,
                                   uint8_t* in_alarm, uint8_t* sounding);

// ── The work lists of one pass of the alarm task: what changed, which rule events to fire, the texts, what the outputs play ──

bool alarm_key_parse(const char* key, AlarmKey* out);   // contact; occupancy presence vibration; water_leak smoke gas carbon_monoxide

// "disarmed" | "arming" | "armed_<id>" | "pending" | "triggered": the stage 1 alarm_state, from the intrusion profiles.
void alarm_summary_name(const AlarmConfig& c, const AlarmPanel& p, char* out, size_t cap);

// The task keeps no queue of changes (stage 1 lost the ones that did not fit): it remembers what it acted on last and asks
// what is different now. Steps in between may collapse; the final state and every alarm still come out.
struct AlarmSeen { AlarmState state = AlarmState::DISARMED; uint32_t alarms = 0, disarms = 0; };   // what was acted on last
struct AlarmChange {
    uint8_t    profile;
    AlarmState from, to;
    uint64_t   cause;        // alarm: of that alarm; else of pending, else 0
    bool       alarm;        // an alarm started since `seen`, even if it is over again
    bool       by_command;   // to == disarmed because of a disarm command
};
// What took the profiles from `seen` to now: at most one per profile, in profile order.
uint8_t alarm_changes(const AlarmPanel& p, const AlarmSeen* seen, AlarmChange* out);   // both [kAlarmMaxProfiles]
void    alarm_seen(const AlarmPanel& p, AlarmSeen* seen);                              // seen := now

constexpr size_t kAlarmEventMax = 40;
// Rule events of one pass, each name once, in this order: per change "alarm_triggered_<id>" when `alarm`, then
// "alarm_<to>_<id>" when the state moved and it is not that same name; then the stage 1 names: "alarm_triggered"
// when an intrusion profile had an alarm, and "alarm_<summary_now>" when the summary moved. Stops when `cap` is full.
uint8_t alarm_events(const AlarmConfig& c, const AlarmChange* ch, uint8_t n, const char* summary_was, const char* summary_now,
                     char out[][kAlarmEventMax], uint8_t cap);

enum class AlarmText : uint8_t { TRIGGERED, PANIC, REFUSED, ARMED, DISARMED, RESTARTED };
// The spec's notification texts without the time. Never ends inside a UTF-8 character. Returns the length.
size_t alarm_text(AlarmText t, const char* profile, const char* device, unsigned more, char* out, size_t cap);

struct AlarmDrive { uint64_t ieee; int8_t output; uint16_t secs; };   // output: index into cfg.outputs, -1 = nothing sounds
// One entry per different output device, in the order of cfg.outputs. A device follows the triggered profile that
// lists it; with several: fire, gas, water, other, intrusion, then the lower profile index. secs: that alarm's time left.
uint8_t alarm_drive_plan(const AlarmConfig& c, const AlarmPanel& p, uint32_t now, AlarmDrive* out);   // [kAlarmMaxOutputs]
enum class AlarmOutAct : uint8_t { NONE, START, STOP };
// A device followed output `was` (-1: none) and now follows `next` (-1: none).
AlarmOutAct alarm_output_step(const AlarmConfig& c, int8_t was, int8_t next);
