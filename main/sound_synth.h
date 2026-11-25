// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Hub sounds: the table of built-in sounds, the synthesizer that renders them to 16 kHz mono int16,
// and the one-voice player (the urgent policy) with the Hub Sound `warning` text. Pure (no IDF, no
// heap), host-tested in test/host/test_sound_synth.cpp. The glue -- the lock around the player, the
// I2S output, the REST/WS/Lua API and the Hub Sound device -- lives elsewhere.
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

// ---- the player: one voice, the urgent policy -------------------------------------------------
// No locking: whoever shares a player between tasks wraps every call in one mutex.
enum class SoundOwner  : uint8_t { USER, DEVICE };   // DEVICE = the Hub Sound device
enum class SoundResult : uint8_t { OK, MUTED, BUSY, UNKNOWN, NO_LOOP, BAD_ARG, ABSENT };
const char* sound_result_text(SoundResult r);

struct SoundPlayer {
    SoundVoice  voice;
    bool        active, urgent, muted;
    SoundOwner  owner;
    const char* name;          // while active: the built-in's name, "tone" or "melody"
    int         volume;        // the default, 0..max_volume
    int         max_volume;    // 0..100
};

void sound_player_init(SoundPlayer* p, int max_volume);   // default volume 40 (held to the max), unmuted, idle
int  sound_player_set_volume(SoundPlayer* p, int v);      // the default: held to 0..max; returns it
void sound_player_set_muted(SoundPlayer* p, bool m);      // muting stops a playing sound that is not urgent

// Admission, in this order (arguments before state; a refused call changes nothing):
//   volume outside -1..100 (and, for a tone or melody, a bad hz/ms/note list): BAD_ARG
//   an unknown index: UNKNOWN;  loop on a sound that cannot loop: NO_LOOP
//   a non-urgent sound (tone and melody too) while muted: MUTED, or while an urgent one plays: BUSY
//   otherwise it replaces the current sound.
// Volume -1 = the default volume; whatever it is, it is held to max_volume. max_ms: see sound_voice_builtin.
SoundResult sound_player_play(SoundPlayer* p, int idx, int volume, bool loop, uint32_t max_ms, SoundOwner o);
SoundResult sound_player_tone(SoundPlayer* p, int hz, int ms, int volume);
SoundResult sound_player_melody(SoundPlayer* p, const SoundNote* n, size_t count, int volume);
bool        sound_player_stop(SoundPlayer* p, const char* name);    // nullptr or "" = any; else only if that name plays
bool        sound_player_stop_owner(SoundPlayer* p, SoundOwner o);  // only if o started the current sound
const char* sound_player_playing(const SoundPlayer* p);             // built-in name, "tone", "melody" or nullptr
// Renders up to n samples; 0 when idle. A result below n (0 included) means the sound ended with this
// call and the player is idle after it.
size_t      sound_player_render(SoundPlayer* p, int16_t* out, size_t n);

// ---- the Hub Sound device's `warning` -----------------------------------------------------------
// The alarm sends IAS WD's "mode,level,strobe,seconds"; fewer fields are fine: "mode[,level[,strobe[,seconds]]]".
// Exact words, case matters, no spaces, at most 4 fields, none empty. Defaults: emergency, medium, 10 s.
//   mode:    stop OFF | burglar -> alarm | fire fire_panic -> fire | emergency alarm ON police_panic emergency_panic -> siren
//   level:   low medium high very_high -> volume 25 40 55 70
//   strobe:  1 true 0 false (accepted, ignored)      seconds: decimal digits, held to 600; 0 = stop
struct SoundWarning { bool stop; int idx; int volume; uint32_t secs; };
bool         sound_warning_text(const char* s, SoundWarning* w);   // false (w untouched) when s is not one
SoundWarning sound_warning_secs(uint32_t secs);                    // Bool true (10) or an integer: a siren for secs; 0 = stop
// Loops w.idx at w.volume for w.secs as a DEVICE sound. w.stop (or 0 s) stops the DEVICE's sound, if it is the
// one playing, and is OK either way.
SoundResult  sound_player_warn(SoundPlayer* p, const SoundWarning& w);
inline constexpr char kHubSoundExposes[] = R"([{"name":"warning","type":"binary","access":2}])";
