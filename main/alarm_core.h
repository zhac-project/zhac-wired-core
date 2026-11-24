// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Alarm panel ("security mode"): the state machine, its config and the config's
// JSON. Pure (no IDF), host-tested in test/host/test_alarm_core.cpp. The glue --
// sensors, sirens, keypads, notifications, NVS and the Alarm device -- is alarm.cpp.
// Spec: extra/docs/superpowers/specs/2026-11-24-security-mode-design.md.
#pragma once
#include <cstddef>
#include <cstdint>

#include <ArduinoJson.h>

// Home Assistant's alarm_control_panel states (HomeTiles' alarm tile shows the same).
enum class AlarmState : uint8_t { DISARMED = 0, ARMING, ARMED_AWAY, ARMED_HOME, ARMED_NIGHT, PENDING, TRIGGERED };
enum class AlarmMode : uint8_t { AWAY = 0, HOME, NIGHT };   // bit (1 << mode) in AlarmSensor::modes
// The key a siren is driven by: IAS WD `warning`, a Tuya DP siren's `alarm`, a plug or relay's `state`.
enum class AlarmSirenKind : uint8_t { WARNING = 0, ALARM, SWITCH };

constexpr uint8_t kAlarmMaxSensors = 48;
constexpr uint8_t kAlarmMaxSirens  = 8;
constexpr uint8_t kAlarmMaxKeypads = 8;
constexpr uint8_t kAlarmMaxCodes   = 8;
constexpr size_t  kAlarmCodeMax    = 9;    // 4-8 digits + NUL
constexpr size_t  kAlarmEnumMax    = 24;   // a melody / volume value as the siren's expose names it
constexpr uint16_t kAlarmDelayMax  = 300;  // exit / entry delay, s
constexpr uint16_t kAlarmSirenMin  = 10;   // siren time, s
constexpr uint16_t kAlarmSirenMax  = 1800;

struct AlarmSensor {
    uint64_t ieee;
    uint8_t  modes;   // bit (1 << AlarmMode)
    bool     entry;   // starts the entry delay instead of the alarm
};

struct AlarmSiren {
    uint64_t       ieee;
    AlarmSirenKind kind;
    // WARNING: z2m's warning object (the duration is the siren time).
    char           wd_mode[16];    // burglar fire emergency police_panic fire_panic emergency_panic
    char           wd_level[12];   // low medium high very_high
    bool           strobe;
    // ALARM: written before alarm=true when not empty.
    char           melody[kAlarmEnumMax];
    char           volume[kAlarmEnumMax];
};

struct AlarmKeypad {
    uint64_t ieee;
    bool     trusted;   // may disarm without a code (a key fob)
};

struct AlarmConfig {
    uint16_t    exit_delay  = 30;    // s, armed_away only
    uint16_t    entry_delay = 30;    // s, entry sensors in every mode
    uint16_t    siren_time  = 180;   // s
    bool        notify_arm  = false; // also notify on arm and disarm
    uint8_t     n_sensors = 0, n_sirens = 0, n_keypads = 0, n_codes = 0;
    AlarmSensor sensors[kAlarmMaxSensors] = {};
    AlarmSiren  sirens[kAlarmMaxSirens] = {};
    AlarmKeypad keypads[kAlarmMaxKeypads] = {};
    char        codes[kAlarmMaxCodes][kAlarmCodeMax] = {};
};

const char* alarm_state_name(AlarmState s);
bool        alarm_state_parse(const char* s, AlarmState* out);
const char* alarm_mode_name(AlarmMode m);
bool        alarm_mode_parse(const char* s, AlarmMode* out);
AlarmState  alarm_armed_state(AlarmMode m);

// Parses and validates a whole config. Fields left out take the defaults, unknown
// fields are ignored. "codes" left out keeps `keep`'s codes (the stored ones; null =
// none; never `out` itself); "codes":[] clears them. False with the first problem
// in `err` ("sensors[2]: 0x... is listed twice"); `out` is then unspecified.
bool alarm_cfg_from_json(const char* json, size_t len, const AlarmConfig* keep,
                         AlarmConfig* out, char* err, size_t cap);
// The config as JSON. with_codes=false (every API read) writes "codes_count" in
// place of the codes. Bytes written, 0 if it did not fit.
size_t alarm_cfg_to_json(const AlarmConfig& c, bool with_codes, char* buf, size_t cap);
// The same into a document's object (a WS reply).
void alarm_cfg_write(const AlarmConfig& c, bool with_codes, JsonObject o);
const AlarmSensor* alarm_cfg_sensor(const AlarmConfig& c, uint64_t ieee);
const AlarmKeypad* alarm_cfg_keypad(const AlarmConfig& c, uint64_t ieee);

// May this keypad disarm with this code? A trusted keypad always may; otherwise the
// code must be one of the configured ones, and with none configured nobody may.
bool alarm_keypad_may_disarm(const AlarmConfig& c, uint64_t keypad, const char* code);

// Why a siren of `kind` cannot sound on a device with these exposes (the JSON array the adapter
// builds), or nullptr when it can: warning needs a settable `warning`, alarm a settable `alarm`
// (a smoke sensor's alarm is only a reading), state a settable `state`.
const char* alarm_siren_misfit(AlarmSirenKind kind, const char* exposes, size_t len);

enum class AlarmArmResult : uint8_t {
    OK,
    OPEN,   // contact sensors of the mode are open and force was not given
    BUSY,   // pending or triggered: disarm first
};

// The panel. Time is monotonic seconds, passed in. The glue compares state()
// before and after each call and acts on the change (sirens, notifications, ...).
class AlarmCore {
public:
    explicit AlarmCore(const AlarmConfig* cfg) : cfg_(cfg) {}

    // Arm `m`. `open` lists the mode's contact sensors that are open now (the glue
    // reads them): without force any refuses the arm; with force they are bypassed
    // until disarm. Arming away with an exit delay goes through ARMING.
    AlarmArmResult arm(AlarmMode m, bool force, const uint64_t* open, uint8_t n_open, uint32_t now);
    void disarm();
    // A configured sensor went active (contact opened, motion, vibration).
    void sensor_active(uint64_t ieee, uint32_t now);
    // A keypad's emergency / panic / fire button: the alarm, whatever the state.
    void panic(uint64_t source, uint32_t now);
    // Ends the exit delay, the entry delay and the siren time.
    void tick(uint32_t now);
    // At boot, with what resting() returned before the restart.
    void restore(AlarmState saved);

    AlarmState state() const { return state_; }
    AlarmMode  mode() const { return mode_; }          // of arming / armed / pending
    uint64_t   cause() const { return cause_; }        // the sensor or keypad behind pending / triggered
    uint32_t   deadline() const { return deadline_; }  // end of arming / pending / triggered
    // What a restart comes back to: the armed mode for arming, pending and triggered
    // (a panic while disarmed: disarmed), else the state itself.
    AlarmState resting() const;
    bool       in_alarm() const { return state_ == AlarmState::PENDING || state_ == AlarmState::TRIGGERED; }
    bool       bypassed(uint64_t ieee) const;
    uint8_t    bypass_count() const { return n_bypass_; }
    uint64_t   bypass_at(uint8_t i) const { return i < n_bypass_ ? bypass_[i] : 0; }

private:
    void trigger(uint64_t cause, uint32_t now);

    const AlarmConfig* cfg_;
    AlarmState state_ = AlarmState::DISARMED;
    AlarmMode  mode_ = AlarmMode::AWAY;
    AlarmState return_to_ = AlarmState::DISARMED;   // where triggered goes back to
    uint64_t   cause_ = 0;
    uint32_t   deadline_ = 0;
    uint64_t   bypass_[kAlarmMaxSensors] = {};
    uint8_t    n_bypass_ = 0;
};
