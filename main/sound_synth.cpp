// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Hub sounds: the built-in table and the synthesizer. See sound_synth.h.
#include "sound_synth.h"

#include <cmath>
#include <cstring>
#include <iterator>

namespace {

// ---- the table -------------------------------------------------------------------------------
constexpr uint8_t kLoop = 1, kUrgent = 2;

constexpr SoundStep note(uint16_t hz, uint16_t ms, uint8_t flags = 0) { return {hz, hz, ms, flags}; }
constexpr SoundStep rest(uint16_t ms) { return {0, 0, ms, 0}; }
constexpr SoundStep slide(uint16_t from, uint16_t to, uint16_t ms) { return {from, to, ms, 0}; }

constexpr SoundStep kBeep[]       = {note(2000, 120)};
constexpr SoundStep kDoubleBeep[] = {note(2000, 100), rest(100), note(2000, 100)};
constexpr SoundStep kTripleBeep[] = {note(2000, 80), rest(80), note(2000, 80), rest(80), note(2000, 80)};
constexpr SoundStep kChime[]      = {note(1319, 400, kStepDecay), note(1047, 600, kStepDecay)};
constexpr SoundStep kDoorbell[]   = {note(659, 600, kStepDecay), note(523, 900, kStepDecay)};
constexpr SoundStep kWestminster[] = {
    note(659, 450, kStepDecay), note(523, 450, kStepDecay), note(587, 450, kStepDecay), note(392, 450, kStepDecay),
    rest(200),
    note(392, 450, kStepDecay), note(587, 450, kStepDecay), note(659, 450, kStepDecay), note(523, 900, kStepDecay)};
constexpr SoundStep kNotify[]     = {note(880, 120), note(1320, 160)};
constexpr SoundStep kSuccess[]    = {note(1047, 100), note(1319, 100), note(1568, 200)};
constexpr SoundStep kError[]      = {note(440, 200), note(330, 300)};
constexpr SoundStep kWarning[]    = {note(1500, 150), rest(150), note(1500, 150), rest(150), note(1500, 150), rest(150), note(1500, 150)};
constexpr SoundStep kArmed[]      = {slide(800, 1600, 100), rest(80), slide(800, 1600, 100)};
constexpr SoundStep kDisarmed[]   = {slide(1600, 800, 200)};
constexpr SoundStep kAlarm[]      = {note(1400, 250, kStepSquare), note(950, 250, kStepSquare)};
constexpr SoundStep kSiren[]      = {slide(600, 1500, 1500), slide(1500, 600, 1500)};
constexpr SoundStep kFire[]       = {note(3000, 500), rest(500), note(3000, 500), rest(500), note(3000, 500)};   // temporal-3
constexpr SoundStep kCo[]         = {note(3000, 100), rest(100), note(3000, 100), rest(100), note(3000, 100), rest(100), note(3000, 100)};   // temporal-4

struct SoundDef { const char* name; const SoundStep* steps; uint8_t n; uint16_t gap_ms; uint8_t flags; };
template <size_t N>
constexpr SoundDef def(const char* name, const SoundStep (&steps)[N], uint16_t gap_ms, uint8_t flags) {
    return {name, steps, (uint8_t)N, gap_ms, flags};
}
constexpr SoundDef kSounds[] = {
    def("beep", kBeep, 880, kLoop),
    def("double_beep", kDoubleBeep, 700, kLoop),
    def("triple_beep", kTripleBeep, 600, kLoop),
    def("chime", kChime, 0, 0),
    def("doorbell", kDoorbell, 0, 0),
    def("westminster", kWestminster, 0, 0),
    def("notify", kNotify, 0, 0),
    def("success", kSuccess, 0, 0),
    def("error", kError, 0, 0),
    def("warning", kWarning, 1000, kLoop),
    def("armed", kArmed, 0, 0),
    def("disarmed", kDisarmed, 0, 0),
    def("alarm", kAlarm, 0, kLoop | kUrgent),
    def("siren", kSiren, 0, kLoop | kUrgent),
    def("fire", kFire, 1500, kLoop | kUrgent),
    def("co", kCo, 5000, kLoop | kUrgent),
};
constexpr size_t kCount = std::size(kSounds);

// ---- the synthesis choices, one place each -----------------------------------------------------
constexpr uint32_t kSamplesPerMs = kSoundRate / 1000;
constexpr uint32_t kAttack  = 2 * kSamplesPerMs;    // every tone ramps in over >= 2 ms ...
constexpr uint32_t kRelease = 5 * kSamplesPerMs;    // ... and out over >= 5 ms, so notes never click (a step under 7 ms gets shorter ramps)
constexpr uint32_t kFade    = 10 * kSamplesPerMs;   // a cut at the cap fades out over at most 10 ms
constexpr float    kPeak    = 28000.f;              // amplitude at volume 100, under the 30000 limit
constexpr float    kTwoPi   = 6.2831853f;

float gain_of(int volume) {   // squared: loudness feels closer to linear than the amplitude does
    const float g = (float)(volume < 0 ? 0 : volume > 100 ? 100 : volume) / 100.f;
    return g * g * kPeak;
}
float soft_square(float s) { return std::tanh(3.f * s) / 0.99505475f; }   // a sine pushed into a rounded square, unit peak (tanh 3)
float hz_at(const SoundStep& st, float t) { return (float)st.hz0 + ((float)st.hz1 - (float)st.hz0) * t; }   // sweeps are linear in Hz
float decay_at(float t) { const float d = 1.f - t; return d * d; }   // decay: a quadratic fade to silence over the note

// One sample of the current step, -1..1 before gain; advances the phase.
float step_sample(SoundVoice* v, const SoundStep& st, uint32_t len) {
    if (!st.hz0) return 0.f;   // a rest
    const uint32_t pos = v->pos;
    const float t = (float)pos / (float)len;
    float w = std::sin(kTwoPi * v->phase);
    if (st.flags & kStepSquare) w = soft_square(w);
    v->phase += hz_at(st, t) / (float)kSoundRate;
    if (v->phase >= 1.f) v->phase -= 1.f;
    const uint32_t att = len / 2 < kAttack ? len / 2 : kAttack;
    const uint32_t rel = len - att < kRelease ? len - att : kRelease;
    float env = 1.f;
    if (pos < att) env = (float)pos / (float)att;                              // first sample 0
    else if (pos + rel >= len) env = (float)(len - 1 - pos) / (float)rel;      // last sample 0
    if (st.flags & kStepDecay) env *= decay_at(t);
    return w * env;
}

void next_step(SoundVoice* v) {
    v->pos = 0;
    v->phase = 0.f;
    if (++v->step < v->n_steps) return;
    v->step = 0;
    if (!v->loop) v->done = true;
    else if (v->gap) v->in_gap = true;
}

void voice_init(SoundVoice* v, int volume, bool loop) {
    *v = SoundVoice{};
    v->gain = gain_of(volume);
    v->loop = loop;
}

}  // namespace

size_t sound_count() { return kCount; }
const char* sound_name(size_t i) { return i < kCount ? kSounds[i].name : nullptr; }
bool sound_urgent(size_t i) { return i < kCount && (kSounds[i].flags & kUrgent); }
bool sound_loopable(size_t i) { return i < kCount && (kSounds[i].flags & kLoop); }

uint32_t sound_ms(size_t i) {
    if (i >= kCount) return 0;
    uint32_t ms = 0;
    for (uint8_t k = 0; k < kSounds[i].n; k++) ms += kSounds[i].steps[k].ms;
    return ms;
}

int sound_find(const char* name) {
    if (!name || !*name) return -1;
    for (size_t i = 0; i < kCount; i++)
        if (!std::strcmp(kSounds[i].name, name)) return (int)i;
    return -1;
}

bool sound_voice_builtin(SoundVoice* v, int idx, int volume, bool loop, uint32_t max_ms) {
    if (!v || idx < 0 || (size_t)idx >= kCount) return false;
    const SoundDef& d = kSounds[idx];
    if (loop && !(d.flags & kLoop)) return false;
    voice_init(v, volume, loop);
    for (uint8_t k = 0; k < d.n; k++) v->steps[k] = d.steps[k];
    v->n_steps = d.n;
    v->gap = (uint32_t)d.gap_ms * kSamplesPerMs;
    v->cap = (max_ms == 0 || max_ms > kSoundLoopCapMs ? kSoundLoopCapMs : max_ms) * kSamplesPerMs;
    return true;
}

bool sound_voice_tone(SoundVoice* v, int hz, int ms, int volume) {
    if (!v || hz < 100 || hz > 8000 || ms < 1 || ms > 10000) return false;
    voice_init(v, volume, false);
    v->steps[0] = note((uint16_t)hz, (uint16_t)ms);
    v->n_steps = 1;
    return true;
}

bool sound_voice_melody(SoundVoice* v, const SoundNote* n, size_t count, int volume) {
    if (!v || !n || count < 1 || count > kSoundMaxNotes) return false;
    for (size_t k = 0; k < count; k++)
        if ((n[k].hz && (n[k].hz < 100 || n[k].hz > 8000)) || n[k].ms < 1 || n[k].ms > 10000) return false;
    voice_init(v, volume, false);
    for (size_t k = 0; k < count; k++) v->steps[k] = n[k].hz ? note(n[k].hz, n[k].ms) : rest(n[k].ms);
    v->n_steps = (uint8_t)count;
    return true;
}

size_t sound_voice_render(SoundVoice* v, int16_t* out, size_t n) {
    if (!v || !out) return 0;
    size_t i = 0;
    while (i < n && !v->done) {
        float s = 0.f;
        if (v->in_gap) {
            if (++v->pos >= v->gap) { v->in_gap = false; v->pos = 0; }
        } else {
            const SoundStep& st = v->steps[v->step];
            const uint32_t len = (uint32_t)st.ms * kSamplesPerMs;
            s = step_sample(v, st, len);
            if (++v->pos >= len) next_step(v);
        }
        if (v->cap) {   // the cut: fade out over the last kFade samples, the last one is 0
            const uint32_t fade = v->cap < kFade ? v->cap : kFade;
            if (v->total + fade >= v->cap) s *= (float)(v->cap - 1 - v->total) / (float)fade;
            if (v->total + 1 >= v->cap) v->done = true;
        }
        v->total++;
        out[i++] = (int16_t)std::lround(s * v->gain);
    }
    return i;
}
