// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Hub sounds: the built-in table, the synthesizer, the one-voice player and the `warning` text. See sound_synth.h.
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

// ---- the player ------------------------------------------------------------------------------
namespace {

constexpr int      kDefaultVolume = 40;
constexpr uint32_t kWarnMaxSecs   = kSoundLoopCapMs / 1000;
constexpr int      kMedium        = 40;   // the `warning` level a text without one means
constexpr uint32_t kWarnSecs      = 10;   // ... and its duration

bool bad_volume(int v) { return v < -1 || v > 100; }

int effective_volume(const SoundPlayer* p, int v) {   // -1 = the default, either way no louder than the max
    const int e = v < 0 ? p->volume : v;
    return e > p->max_volume ? p->max_volume : e;
}

void go_idle(SoundPlayer* p) { p->active = p->urgent = false; p->name = nullptr; }

// Whether a sound with this urgency may start now. Urgent sounds always may.
SoundResult admit(const SoundPlayer* p, bool urgent) {
    if (urgent) return SoundResult::OK;
    if (p->muted) return SoundResult::MUTED;
    return p->active && p->urgent ? SoundResult::BUSY : SoundResult::OK;
}

// The admitted voice replaces the current one.
SoundResult start(SoundPlayer* p, const SoundVoice& v, const char* name, bool urgent, SoundOwner o) {
    p->voice = v;
    p->active = true;
    p->urgent = urgent;
    p->owner = o;
    p->name = name;
    return SoundResult::OK;
}

}  // namespace

const char* sound_result_text(SoundResult r) {
    switch (r) {
    case SoundResult::OK:      return "ok";
    case SoundResult::MUTED:   return "muted";
    case SoundResult::BUSY:    return "an urgent sound is playing";
    case SoundResult::UNKNOWN: return "unknown sound";
    case SoundResult::NO_LOOP: return "this sound cannot loop";
    case SoundResult::BAD_ARG: return "bad value (volume 0-100, hz 100-8000, ms 1-10000, 1-32 notes)";
    case SoundResult::ABSENT:  return "no speaker on this hub";
    }
    return "";
}

void sound_player_init(SoundPlayer* p, int max_volume) {
    *p = SoundPlayer{};
    p->max_volume = max_volume < 0 ? 0 : max_volume > 100 ? 100 : max_volume;
    p->volume = kDefaultVolume > p->max_volume ? p->max_volume : kDefaultVolume;
}

int sound_player_set_volume(SoundPlayer* p, int v) {
    return p->volume = v < 0 ? 0 : v > p->max_volume ? p->max_volume : v;
}

void sound_player_set_muted(SoundPlayer* p, bool m) {
    p->muted = m;
    if (m && p->active && !p->urgent) go_idle(p);
}

SoundResult sound_player_play(SoundPlayer* p, int idx, int volume, bool loop, uint32_t max_ms, SoundOwner o) {
    if (bad_volume(volume)) return SoundResult::BAD_ARG;
    if (idx < 0 || (size_t)idx >= sound_count()) return SoundResult::UNKNOWN;
    if (loop && !sound_loopable((size_t)idx)) return SoundResult::NO_LOOP;
    const bool urgent = sound_urgent((size_t)idx);
    if (const SoundResult r = admit(p, urgent); r != SoundResult::OK) return r;
    SoundVoice v;
    if (!sound_voice_builtin(&v, idx, effective_volume(p, volume), loop, max_ms)) return SoundResult::BAD_ARG;   // not reachable: checked above
    return start(p, v, sound_name((size_t)idx), urgent, o);
}

SoundResult sound_player_tone(SoundPlayer* p, int hz, int ms, int volume) {
    SoundVoice v;
    if (bad_volume(volume) || !sound_voice_tone(&v, hz, ms, effective_volume(p, volume))) return SoundResult::BAD_ARG;
    if (const SoundResult r = admit(p, false); r != SoundResult::OK) return r;
    return start(p, v, "tone", false, SoundOwner::USER);
}

SoundResult sound_player_melody(SoundPlayer* p, const SoundNote* n, size_t count, int volume) {
    SoundVoice v;
    if (bad_volume(volume) || !sound_voice_melody(&v, n, count, effective_volume(p, volume))) return SoundResult::BAD_ARG;
    if (const SoundResult r = admit(p, false); r != SoundResult::OK) return r;
    return start(p, v, "melody", false, SoundOwner::USER);
}

bool sound_player_stop(SoundPlayer* p, const char* name) {
    if (!p->active || (name && *name && std::strcmp(name, p->name))) return false;
    go_idle(p);
    return true;
}

bool sound_player_stop_owner(SoundPlayer* p, SoundOwner o) {
    if (!p->active || p->owner != o) return false;
    go_idle(p);
    return true;
}

const char* sound_player_playing(const SoundPlayer* p) { return p->active ? p->name : nullptr; }

size_t sound_player_render(SoundPlayer* p, int16_t* out, size_t n) {
    if (!p->active || !out || !n) return 0;
    const size_t r = sound_voice_render(&p->voice, out, n);
    if (r < n) go_idle(p);   // a voice that ends on a block boundary says so with a 0 on the next call
    return r;
}

// ---- the Hub Sound `warning` -------------------------------------------------------------------
namespace {

struct Field { const char* p; size_t n; };
bool is(const Field& f, const char* word) { return f.n == std::strlen(word) && !std::strncmp(f.p, word, f.n); }

struct Mode  { const char* word; const char* sound; };   // sound nullptr = stop
struct Level { const char* word; int volume; };
constexpr Mode kModes[] = {
    {"stop", nullptr}, {"OFF", nullptr},
    {"burglar", "alarm"},
    {"fire", "fire"}, {"fire_panic", "fire"},
    {"emergency", "siren"}, {"alarm", "siren"}, {"ON", "siren"}, {"police_panic", "siren"}, {"emergency_panic", "siren"},
};
constexpr Level kLevels[] = {{"low", 25}, {"medium", kMedium}, {"high", 55}, {"very_high", 70}};

constexpr SoundWarning kStop = {true, -1, 0, 0};

uint32_t held(uint32_t secs) { return secs > kWarnMaxSecs ? kWarnMaxSecs : secs; }

// Decimal digits only, held to 600 as they come (a long run of digits cannot overflow).
bool seconds(const Field& f, uint32_t* out) {
    uint32_t v = 0;
    for (size_t i = 0; i < f.n; i++) {
        if (f.p[i] < '0' || f.p[i] > '9') return false;
        if (v <= kWarnMaxSecs) v = v * 10 + (uint32_t)(f.p[i] - '0');
    }
    *out = held(v);
    return true;
}

}  // namespace

bool sound_warning_text(const char* s, SoundWarning* w) {
    if (!s || !w) return false;
    Field f[4];
    size_t k = 0;
    for (;;) {
        if (k == 4) return false;
        const char* comma = std::strchr(s, ',');
        const size_t n = comma ? (size_t)(comma - s) : std::strlen(s);
        if (!n) return false;
        f[k++] = {s, n};
        if (!comma) break;
        s = comma + 1;
    }
    const Mode* mode = nullptr;
    for (const Mode& m : kModes) if (is(f[0], m.word)) mode = &m;
    if (!mode) return false;
    int volume = kMedium;
    if (k > 1) {
        const Level* lv = nullptr;
        for (const Level& l : kLevels) if (is(f[1], l.word)) lv = &l;
        if (!lv) return false;
        volume = lv->volume;
    }
    if (k > 2 && !is(f[2], "1") && !is(f[2], "true") && !is(f[2], "0") && !is(f[2], "false")) return false;
    uint32_t secs = kWarnSecs;
    if (k > 3 && !seconds(f[3], &secs)) return false;
    *w = !mode->sound || secs == 0 ? kStop : SoundWarning{false, sound_find(mode->sound), volume, secs};
    return true;
}

SoundWarning sound_warning_secs(uint32_t secs) {
    return secs ? SoundWarning{false, sound_find("siren"), kMedium, held(secs)} : kStop;
}

SoundResult sound_player_warn(SoundPlayer* p, const SoundWarning& w) {
    if (w.stop || w.secs == 0) {   // a struct built by hand with 0 s must not become the 10 min cap
        sound_player_stop_owner(p, SoundOwner::DEVICE);
        return SoundResult::OK;
    }
    return sound_player_play(p, w.idx, w.volume, true, held(w.secs) * 1000, SoundOwner::DEVICE);
}
