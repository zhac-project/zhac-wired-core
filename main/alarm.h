// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Alarm panel ("Safety", spec extra/docs/superpowers/specs/2026-11-24-safety-profiles-design.md):
// the glue around alarm_core -- sensors and keypads in, outputs, notifications and the
// "Alarm" device out, the config and the states in NVS.
#pragma once

#include <cstddef>
#include <cstdint>

#include <ArduinoJson.h>

#include "alarm_core.h"

// The Alarm device: locally administered IEEE ("ZHACAL"), protocol PROTO_LOCAL.
constexpr uint64_t kAlarmIeee = 0x025A484143414C00ULL;

// Boot, in this order: load the config and the states, register the device hooks
// (beside user_io_early_init) ...
void alarm_early_init();
// ... add the Alarm pool entry (after the pool restore) ...
void alarm_attach_device();
// ... take the sensors' last values, watch sensors and keypads, start the task (before the event pump).
void alarm_start();

// The hub page (WS alarm.*). The Alarm device's attributes do the same, without force.
enum class AlarmReply : uint8_t {
    OK,
    OPEN,         // arm: contact sensors of the profile are open and force was not given
    BUSY,         // arm: an alarm is on (this profile, or for an intrusion profile another one)
    NO_PROFILE,   // no profile has that id
};
// OPEN: the open contact sensors in `open` (room for kAlarmMaxSensors), their count in `n_open`.
AlarmReply alarm_arm(const char* id, bool force, uint64_t* open, uint8_t* n_open);
// nullptr or "": the intrusion profiles are disarmed, the others silenced.
AlarmReply alarm_disarm(const char* id);
// Pending or triggered of that profile ends now; it goes where it would when the alarm time ends.
AlarmReply alarm_silence(const char* id);
// A cloud write to the Alarm device that the hub took (review 2026-11-26 HL-02): `key` the attribute (or the
// command's name), `value` its text or nullptr. The owner's alert "Alarm <value> from the cloud" (kind
// alarm_changed_by_cloud) and Telegram, as for the alarm's own notifications. Takes no lock.
void alarm_cloud_changed(const char* key, const char* value);
// Replace the config ("codes" left out keeps the stored ones). False with the first
// problem in `err` when nothing was changed; refused while an alarm is running.
bool alarm_config_set(const char* json, size_t len, char* err, size_t cap);
// {state, sensor?, profiles:[{id, name, class, state, since, left?, cause?, sensor?, bypassed[]}]}:
// alarm.get and alarm.changed. The top state and sensor are the Alarm device's alarm_state and alarm_sensor.
void alarm_status_json(JsonObject o);
// The config without the codes ("codes_count").
void alarm_config_json(JsonObject o);
