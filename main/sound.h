// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The hub speaker (spec extra/docs/superpowers/specs/2026-11-25-hub-sounds-design.md): the ES8311 codec
// found at boot, its I2S stream, the amplifier pin, and the task that plays the one-voice player of
// sound_synth.h. Rules, Lua, REST and the Hub Sound device all come through here; every call is safe from
// any task, one on a PSRAM stack included (one mutex around the player; no call writes flash). Built
// without CONFIG_ZHAC_AUDIO, each call is a stub: not present, plays ABSENT, volume and maximum 0.
#pragma once

#include <cstddef>
#include <cstdint>

#include "sound_synth.h"

// The hub's own Hub Sound device, a siren output for the alarm: "\x02ZHACSN", in the hub-local range.
constexpr uint64_t kHubSoundIeee = 0x025A484143534E00ULL;

// Boot, in this order (app_main): the stored volume and mute, the codec probe (a match claims the pins, so
// before user_io_early_init; it also drives the amplifier pin low at once) and the rule hooks (before
// simple_rules_init), with or without a codec ...
void sound_early_init();
// ... then I2S, the codec, the amplifier pin (low again) and the render task. A failure sets sound_error().
void sound_start();

bool        sound_present();           // the codec's chip ID matched
const char* sound_error();             // "" or why a present codec is off
bool        sound_owns_pin(int gpio);  // the 8 audio pins, once the chip ID matched
// The Hub Sound device in the pool (app_main, after the pool restore): added and kept current when the chip ID
// matched, else a stored one (a board swap) is removed, silently. With CONFIG_ZHAC_AUDIO off: nothing.
void        sound_attach_device();

// Plays as USER; volume -1 = the default. ABSENT without a codec or when it did not start; the rest is the
// player's admission (sound_synth.h). A sound asked for before sound_start() plays once the task runs.
SoundResult sound_play(const char* name, int volume, bool loop);
SoundResult sound_tone(int hz, int ms, int volume);
SoundResult sound_melody(const SoundNote* notes, size_t n, int volume);
bool        sound_stop(const char* name);   // nullptr or "" = whatever plays; else only that sound
const char* sound_playing();                // the name, "tone", "melody" or nullptr

// The setters change the live value at once and never write flash; the sound task saves volume and mute to
// NVS once no sound plays. Without that task (no codec, or its start failed) only the live value changes.
int  sound_volume();             // the default volume, 0..sound_max_volume()
int  sound_set_volume(int v);    // held to 0..max; returns the value now in use
bool sound_muted();
void sound_set_muted(bool m);
int  sound_max_volume();         // CONFIG_ZHAC_AUDIO_MAX_VOLUME
