// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Hub pin configuration: the data, its validation and its JSON. Pure (no IDF),
// host-tested in test/host/test_user_io_config.cpp. Hardware is user_io.cpp.
// Spec: extra/docs/superpowers/specs/2026-11-15-gpio-control-design.md §1/§6.
#pragma once

#include <cstddef>
#include <cstdint>

enum class UserIoMode : uint8_t { OUT = 0, PWM, IN, ADC, STRIP };
enum class UserIoPull : uint8_t { NONE = 0, UP, DOWN };
enum class UserIoBoot : uint8_t { OFF = 0, ON, LAST };

constexpr uint8_t kUserIoMaxPins = 16;
constexpr size_t  kUserIoNameMax = 16;   // 15 characters + NUL

struct UserIoPin {
    uint8_t    gpio;
    UserIoMode mode;
    char       name[kUserIoNameMax];   // [a-z][a-z0-9_]{0,14}: the Hub I/O attribute key
    bool       invert;                 // out/pwm: on = low; in: low reads as on
    UserIoPull pull;                   // in
    UserIoBoot boot;                   // out/pwm: level at boot
    uint32_t   freq;                   // pwm, Hz
    uint16_t   interval;               // adc publish period, s (0 = Lua only)
    uint16_t   delta;                  // adc publish threshold
    uint16_t   leds;                   // strip length
};

struct UserIoConfig {
    uint8_t   count;
    UserIoPin pins[kUserIoMaxPins];
};

// What this build offers: the pins a user may configure and which are ADC pins.
struct UserIoCaps {
    uint8_t  allowed[64];
    uint8_t  n_allowed;
    uint64_t adc_mask;   // bit n set = GPIO n has an ADC channel
};

// Parses and validates a whole config ({"pins":[...]}); fields left out take
// the defaults, unknown fields are ignored. False with the first problem in
// `err` ("row 2: gpio 21 is used twice"); `out` is then unspecified.
bool user_io_cfg_from_json(const char* json, size_t len, const UserIoCaps& caps,
                           UserIoConfig* out, char* err, size_t cap);

// One pin's config fields as a JSON object (no runtime state). Bytes written, 0 if it did not fit.
size_t user_io_cfg_pin_json(const UserIoPin& p, char* buf, size_t cap);

// Index of the pin `ref` names (its name, or its GPIO number in decimal), else -1.
int user_io_cfg_find(const UserIoConfig& c, const char* ref);

// A level as rules and the UI write it: on/off/true/false/high/low (any case)
// or a number (non-zero = on). False for anything else.
bool user_io_parse_level(const char* s, bool* on);

// "2, 3,4" -> {2,3,4}: the Kconfig pin list. Entries that are not 0-63 are skipped. Returns the count.
uint8_t user_io_parse_pins(const char* csv, uint8_t* out, uint8_t cap);

const char* user_io_mode_name(UserIoMode m);
