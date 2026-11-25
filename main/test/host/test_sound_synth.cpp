// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Hub sounds (spec extra/docs/superpowers/specs/2026-11-25-hub-sounds-design.md): the built-in
// sound table and the synthesizer -- lengths, clicks, levels, loops and the cap, the loop gap,
// the tone and melody voices, the sweep.
#include "sound_synth.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <type_traits>
#include <vector>

static int g_fail = 0;
#define CHECK(c, m) do { if (c) std::printf("PASS: %s\n", m); else { std::printf("FAIL: %s\n", m); g_fail++; } } while (0)

static_assert(std::is_trivially_copyable_v<SoundVoice>, "a voice is a plain copyable struct");

constexpr size_t kBuiltIns = 16;
static const char* const kNames[kBuiltIns] = {
    "beep", "double_beep", "triple_beep", "chime", "doorbell", "westminster", "notify", "success",
    "error", "warning", "armed", "disarmed", "alarm", "siren", "fire", "co"};
enum : size_t { iBeep = 0, iDoubleBeep = 1, iDoorbell = 4, iWarning = 9, iAlarm = 12, iSiren = 13, iFire = 14, iCo = 15 };

// Render a whole voice in 256-sample blocks, as the player will.
static std::vector<int16_t> render_all(SoundVoice& v) {
    std::vector<int16_t> out;
    int16_t blk[256];
    for (;;) {
        const size_t r = sound_voice_render(&v, blk, 256);
        out.insert(out.end(), blk, blk + r);
        if (r < 256) break;
    }
    return out;
}
static std::vector<int16_t> render_builtin(size_t i, int volume, bool loop, uint32_t max_ms) {
    SoundVoice v;
    if (!sound_voice_builtin(&v, (int)i, volume, loop, max_ms)) return {};
    return render_all(v);
}
static int peak(const std::vector<int16_t>& s) {
    int p = 0;
    for (int16_t x : s) p = std::abs((int)x) > p ? std::abs((int)x) : p;
    return p;
}
// A crossing = two consecutive NON-ZERO samples of opposite sign; exact zeros are skipped.
static int crossings(const std::vector<int16_t>& s, size_t from, size_t to) {
    int n = 0, prev = 0;
    for (size_t i = from; i < to && i < s.size(); i++) {
        if (s[i] == 0) continue;
        const int sign = s[i] > 0 ? 1 : -1;
        if (prev && sign != prev) n++;
        prev = sign;
    }
    return n;
}
static bool all_zero(const std::vector<int16_t>& s, size_t from, size_t to) {
    if (to > s.size()) return false;
    for (size_t i = from; i < to; i++) if (s[i] != 0) return false;
    return true;
}

static void table() {
    CHECK(sound_count() == kBuiltIns, "table: 16 sounds");
    bool order = true;
    for (size_t i = 0; i < kBuiltIns; i++) order = order && sound_name(i) && !std::strcmp(sound_name(i), kNames[i]);
    CHECK(order, "table: names in the table's order");
    CHECK(sound_name(16) == nullptr, "table: sound_name past the end is nullptr");
    bool urgent = true, loopable = true;
    for (size_t i = 0; i < kBuiltIns; i++) {
        urgent = urgent && sound_urgent(i) == (i == iAlarm || i == iSiren || i == iFire || i == iCo);
        loopable = loopable && sound_loopable(i) == (i == iBeep || i == iDoubleBeep || i == 2 || i == iWarning ||
                                                     i == iAlarm || i == iSiren || i == iFire || i == iCo);
    }
    CHECK(urgent, "table: urgent is exactly alarm, siren, fire, co");
    CHECK(loopable, "table: loopable is exactly beep, double_beep, triple_beep, warning, alarm, siren, fire, co");
    CHECK(!sound_urgent(16) && !sound_loopable(16) && sound_ms(16) == 0, "table: flags and length past the end are empty");
    CHECK(sound_find("doorbell") == (int)iDoorbell, "table: find doorbell");
    CHECK(sound_find("Doorbell") == -1, "table: find is exact (case)");
    CHECK(sound_find("") == -1 && sound_find(nullptr) == -1, "table: find of empty and nullptr is -1");
    CHECK(sound_find("door") == -1 && sound_find("doorbells") == -1, "table: find is exact (length)");
    CHECK(sound_ms(0) == 120 && sound_ms(1) == 300 && sound_ms(15) == 700, "table: one-pass lengths of beep, double_beep, co");
}

static void lengths() {
    bool ok = true;
    for (size_t i = 0; i < kBuiltIns; i++) {
        const size_t n = render_builtin(i, 70, false, 0).size();
        if (n != (size_t)sound_ms(i) * 16) { std::printf("  %s: %zu samples, want %u\n", kNames[i], n, sound_ms(i) * 16); ok = false; }
    }
    CHECK(ok, "lengths: every built-in renders sound_ms * 16 samples, no loop");
    SoundVoice v;
    sound_voice_builtin(&v, iBeep, 70, false, 0);
    render_all(v);
    int16_t blk[256];
    CHECK(sound_voice_render(&v, blk, 256) == 0 && sound_voice_render(&v, blk, 256) == 0, "lengths: an ended voice renders 0 from then on");
    CHECK(sound_voice_render(nullptr, blk, 256) == 0, "lengths: a null voice renders 0");
}

static void no_clicks() {
    bool ok = true;
    for (size_t i = 0; i < kBuiltIns; i++) {
        const auto s = render_builtin(i, 70, false, 0);
        if (s.empty() || std::abs(s.front()) > 64 || std::abs(s.back()) > 64) { std::printf("  %s clicks\n", kNames[i]); ok = false; }
    }
    CHECK(ok, "no_clicks: first and last samples of every built-in are |s| <= 64");
    // Inside a sound, between two notes with no rest (chime: 1319 then 1047), the join is quiet too.
    const auto chime = render_builtin(3, 100, false, 0);
    const size_t join = 400 * 16;
    CHECK(chime.size() > join && std::abs(chime[join - 1]) <= 64 && std::abs(chime[join]) <= 64, "no_clicks: the join of two notes is quiet");
}

static void levels() {
    bool ok = true;
    for (size_t i = 0; i < kBuiltIns; i++) {
        const int p = peak(render_builtin(i, 100, false, 0));
        if (p > 30000 || p < 10000) { std::printf("  %s peaks at %d\n", kNames[i], p); ok = false; }
    }
    CHECK(ok, "levels: every built-in peaks between 10000 and 30000 at volume 100");
    ok = true;
    for (size_t i = 0; i < kBuiltIns; i++) {
        const auto s = render_builtin(i, 0, false, 0);
        ok = ok && !s.empty() && all_zero(s, 0, s.size());
    }
    CHECK(ok, "levels: volume 0 renders silence");
    int prev = -1;
    bool mono = true;
    for (int vol = 0; vol <= 100; vol += 10) {
        const int p = peak(render_builtin(iBeep, vol, false, 0));
        mono = mono && p >= prev;
        prev = p;
    }
    CHECK(mono, "levels: beep's peak never falls as the volume rises");
    CHECK(peak(render_builtin(iBeep, 40, false, 0)) < peak(render_builtin(iBeep, 70, false, 0)), "levels: beep peaks lower at 40 than at 70");
    CHECK(peak(render_builtin(iBeep, 250, false, 0)) <= 30000, "levels: a volume past 100 is held at 100");
}

static void loop_cap() {
    const auto a = render_builtin(iAlarm, 70, true, 3000);
    CHECK(a.size() >= 47840 && a.size() <= 48160, "loop_cap: alarm, loop, 3000 ms renders 3 s");
    CHECK(!a.empty() && std::abs(a.back()) <= 64, "loop_cap: the cut fades out");
    CHECK(a.size() > 3000 && peak(std::vector<int16_t>(a.begin() + 47000, a.begin() + 47600)) > 5000, "loop_cap: the alarm is still loud before the fade");
    const auto b = render_builtin(iBeep, 70, true, 0);
    CHECK(b.size() >= 599u * 16000 && b.size() <= 600u * 16000 + 160, "loop_cap: beep, loop, max_ms 0 stops at the 10 min cap");
    CHECK(b.size() > 0 && std::abs(b.back()) <= 64, "loop_cap: the 10 min cut fades out");
    const auto c = render_builtin(iBeep, 70, true, 1000000000u);
    CHECK(c.size() >= 599u * 16000 && c.size() <= 600u * 16000 + 160, "loop_cap: a max_ms past the cap is held at the cap");
    const auto d = render_builtin(3, 70, false, 100);      // chime, 1000 ms, cut at 100 ms
    CHECK(d.size() >= 1440 && d.size() <= 1760 && std::abs(d.back()) <= 64, "loop_cap: a sound that is not looped is capped too");
    const auto e = render_builtin(iBeep, 70, false, 5000); // beep, 120 ms, cap 5 s: ends naturally
    CHECK(e.size() == 120u * 16, "loop_cap: a sound shorter than the cap ends by itself");
}

static void loop_gap() {
    const auto s = render_builtin(iDoubleBeep, 70, true, 2000);
    CHECK(s.size() >= 31840 && s.size() <= 32160, "loop_gap: double_beep, loop, 2000 ms renders 2 s");
    CHECK(all_zero(s, 4800, 16000), "loop_gap: the 700 ms gap after the pass is silent");
    CHECK(peak(std::vector<int16_t>(s.begin() + 16000, s.begin() + 16000 + 1600)) > 1000, "loop_gap: the second pass starts at sample 16000");
    CHECK(peak(std::vector<int16_t>(s.begin() + 15000, s.begin() + 16000)) == 0, "loop_gap: nothing before it");
    CHECK(all_zero(s, 4800 + 11200 + 4800, 32000 - 160), "loop_gap: the gap after the second pass is silent too");
    // alarm has no gap: the second pass follows the first at sample 8000
    const auto a = render_builtin(iAlarm, 70, true, 2000);
    CHECK(a.size() > 8100 && peak(std::vector<int16_t>(a.begin() + 8000, a.begin() + 8100)) > 0 && a[8000] == 0, "loop_gap: a zero gap starts the next pass at once");
}

static void tone() {
    SoundVoice v;
    CHECK(!sound_voice_tone(&v, 99, 100, 70), "tone: 99 Hz is refused");
    CHECK(!sound_voice_tone(&v, 8001, 100, 70), "tone: 8001 Hz is refused");
    CHECK(!sound_voice_tone(&v, 440, 0, 70), "tone: 0 ms is refused");
    CHECK(!sound_voice_tone(&v, 440, 10001, 70), "tone: 10001 ms is refused");
    CHECK(!sound_voice_tone(nullptr, 440, 100, 70), "tone: a null voice is refused");
    CHECK(sound_voice_tone(&v, 100, 1, 70) && render_all(v).size() == 16, "tone: 100 Hz, 1 ms is accepted (16 samples)");
    CHECK(sound_voice_tone(&v, 8000, 10000, 70) && render_all(v).size() == 160000, "tone: 8000 Hz, 10000 ms is accepted");
    CHECK(sound_voice_tone(&v, 1000, 1000, 70), "tone: 1000 Hz, 1000 ms is accepted");
    const auto s = render_all(v);
    CHECK(s.size() == 16000, "tone: renders 16000 samples");
    const int z = crossings(s, 0, s.size());
    CHECK(z >= 1980 && z <= 2020, "tone: 1000 Hz for 1 s has about 2000 zero crossings");
    CHECK(std::abs(s.front()) <= 64 && std::abs(s.back()) <= 64, "tone: no click at either end");
    CHECK(peak(s) > 10000 && peak(s) <= 30000, "tone: level at volume 70");
}

static void melody() {
    SoundVoice v;
    const SoundNote ok3[] = {{440, 100}, {0, 100}, {440, 100}};
    CHECK(!sound_voice_melody(&v, ok3, 0, 70), "melody: count 0 is refused");
    SoundNote many[33];
    for (auto& n : many) n = {440, 10};
    CHECK(!sound_voice_melody(&v, many, 33, 70), "melody: 33 notes are refused");
    CHECK(sound_voice_melody(&v, many, 32, 70) && render_all(v).size() == 32u * 160, "melody: 32 notes are accepted");
    CHECK(!sound_voice_melody(&v, nullptr, 3, 70), "melody: a null list is refused");
    const SoundNote low[] = {{440, 100}, {50, 100}};
    CHECK(!sound_voice_melody(&v, low, 2, 70), "melody: a note of 50 Hz is refused");
    const SoundNote high[] = {{8001, 100}};
    CHECK(!sound_voice_melody(&v, high, 1, 70), "melody: a note of 8001 Hz is refused");
    const SoundNote zero[] = {{440, 0}};
    CHECK(!sound_voice_melody(&v, zero, 1, 70), "melody: a note of 0 ms is refused");
    const SoundNote longer[] = {{440, 10001}};
    CHECK(!sound_voice_melody(&v, longer, 1, 70), "melody: a note of 10001 ms is refused");
    SoundNote own[] = {{440, 100}, {0, 100}, {440, 100}};
    CHECK(sound_voice_melody(&v, own, 3, 70), "melody: 440, rest, 440 is accepted");
    own[0] = {50, 1}; own[1] = {8000, 5}; own[2] = {0, 1};   // the caller's array can change or go away
    SoundVoice copy = v;                                      // a voice is a plain copy
    const auto s = render_all(copy);
    CHECK(s.size() == 4800, "melody: renders 4800 samples");
    CHECK(all_zero(s, 1600, 3200), "melody: the rest is silent");
    CHECK(peak(std::vector<int16_t>(s.begin(), s.begin() + 1600)) > 1000 && peak(std::vector<int16_t>(s.begin() + 3200, s.end())) > 1000, "melody: both notes sound");
    CHECK(crossings(s, 0, 1600) > 0 && std::abs(s[1599]) <= 64 && std::abs(s[3200]) <= 64, "melody: notes start and end quiet");
}

static void sweep() {
    const auto s = render_builtin(iSiren, 70, false, 0);
    CHECK(s.size() == 3000u * 16, "sweep: siren is 3 s");
    const int lo = crossings(s, 0, 1600), hi = crossings(s, 22400, 24000);
    std::printf("  siren crossings: %d at the start of the rise, %d at its top\n", lo, hi);
    CHECK(lo < hi, "sweep: the first siren pass rises (fewer crossings at the start than at the top)");
    const int top = crossings(s, 24000, 25600), end = crossings(s, 46400, 48000);
    CHECK(end < top, "sweep: the second half falls");
    const auto armed = render_builtin(10, 70, false, 0);   // 800->1600 x100, rest 80, 800->1600 x100
    CHECK(armed.size() == 280u * 16 && all_zero(armed, 1600, 1600 + 1280), "sweep: armed is two rises around an 80 ms rest");
}

static void flags_and_bounds() {
    SoundVoice v;
    CHECK(!sound_voice_builtin(&v, -1, 70, false, 0) && !sound_voice_builtin(&v, 16, 70, false, 0), "bounds: an unknown index is refused");
    CHECK(!sound_voice_builtin(nullptr, 0, 70, false, 0), "bounds: a null voice is refused");
    CHECK(!sound_voice_builtin(&v, 3, 70, true, 0), "bounds: loop on a sound that cannot loop is refused");
    CHECK(sound_voice_builtin(&v, 3, 70, false, 0) && sound_voice_builtin(&v, (int)iCo, 70, true, 0), "bounds: loop on a loopable sound is accepted");
    // rendering in other block sizes gives the same samples
    const auto ref = render_builtin(iDoorbell, 70, false, 0);
    SoundVoice w;
    sound_voice_builtin(&w, (int)iDoorbell, 70, false, 0);
    std::vector<int16_t> odd;
    int16_t blk[97];
    for (;;) { const size_t r = sound_voice_render(&w, blk, 97); odd.insert(odd.end(), blk, blk + r); if (r < 97) break; }
    CHECK(odd == ref, "bounds: the block size does not change the samples");
}

int main() {
    table();
    lengths();
    no_clicks();
    levels();
    loop_cap();
    loop_gap();
    tone();
    melody();
    sweep();
    flags_and_bounds();
    std::printf("%s (%d failed)\n", g_fail ? "FAILED" : "ALL PASSED", g_fail);
    return g_fail ? 1 : 0;
}
