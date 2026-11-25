// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Hub sounds: the table of built-in sounds and the synthesizer that renders them to 16 kHz mono
// int16. Pure (no IDF, no heap), host-tested in test/host/test_sound_synth.cpp. The glue -- the
// player, the I2S output, the REST/WS/Lua API and the Hub Sound device -- lives elsewhere.
// Spec: extra/docs/superpowers/specs/2026-11-25-hub-sounds-design.md.
#pragma once
#include <cstddef>
#include <cstdint>

constexpr uint32_t kSoundRate      = 16000;    // Hz, mono, int16
constexpr uint32_t kSoundLoopCapMs = 600000;   // 10 min: the longest any built-in plays
constexpr size_t   kSoundMaxNotes  = 32;

struct SoundNote { uint16_t hz; uint16_t ms; };   // hz 0 = rest

// ---- the built-in table ----------------------------------------------------------------------
size_t      sound_count();                 // 16
const char* sound_name(size_t i);          // nullptr past the end
bool        sound_urgent(size_t i);
bool        sound_loopable(size_t i);
uint32_t    sound_ms(size_t i);            // one pass, the loop gap excluded
int         sound_find(const char* name);  // exact match; -1 for none, "" or nullptr

// ---- one voice -------------------------------------------------------------------------------
enum : uint8_t { kStepDecay = 1, kStepSquare = 2 };   // SoundStep::flags: fade over the note / softened square
struct SoundStep { uint16_t hz0, hz1, ms; uint8_t flags; };   // hz0 0 = rest; hz1 != hz0 = a sweep hz0 -> hz1

// A plain copyable struct: it owns its steps (a built-in's are copied from the table, a melody's from
// the caller), so nothing points into caller memory. Fill it with the sound_voice_* builders only.
struct SoundVoice {
    SoundStep steps[kSoundMaxNotes];
    uint8_t   n_steps;
    bool      loop;
    float     gain;        // peak amplitude, int16 units
    uint32_t  gap;         // samples between two passes of a loop
    uint32_t  cap;         // samples; 0 = no cap
    // render position
    uint32_t  total;       // samples rendered so far
    uint32_t  pos;         // sample within the current step, or within the gap
    float     phase;       // cycles, 0..1
    uint8_t   step;
    bool      in_gap;
    bool      done;
};

// volume is 0-100 (a value outside is held to it); the clamp to the board's maximum is the player's.
// max_ms caps the sound whether it loops or not; 0 (or a value past it) = kSoundLoopCapMs. The cut fades
// out over at most 10 ms and ends at the cap. loop on a sound that cannot loop is refused.
bool   sound_voice_builtin(SoundVoice* v, int idx, int volume, bool loop, uint32_t max_ms);
bool   sound_voice_tone(SoundVoice* v, int hz, int ms, int volume);                       // hz 100-8000, ms 1-10000
bool   sound_voice_melody(SoundVoice* v, const SoundNote* n, size_t count, int volume);   // 1-32 notes, hz 0 or 100-8000, ms 1-10000
// Renders up to n samples; fewer than n means the voice ended with this call, and every later call returns 0.
size_t sound_voice_render(SoundVoice* v, int16_t* out, size_t n);
