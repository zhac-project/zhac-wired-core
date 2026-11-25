// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Hub sounds (spec extra/docs/superpowers/specs/2026-11-25-hub-sounds-design.md): the built-in
// sound table and the synthesizer -- lengths, clicks, levels, loops and the cap, the loop gap,
// the tone and melody voices, the sweep -- and the one-voice player: the urgent policy, volumes,
// stopping, rendering and the Hub Sound `warning` text.
#include "alarm_core.h"
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

// ---- the player ---------------------------------------------------------------------------------
static SoundPlayer mk(int max_volume = 70) {
    SoundPlayer p;
    sound_player_init(&p, max_volume);
    return p;
}
static std::vector<int16_t> render_player(SoundPlayer& p) {   // 256-sample blocks until one comes back short
    std::vector<int16_t> out;
    int16_t blk[256];
    for (;;) {
        const size_t r = sound_player_render(&p, blk, 256);
        out.insert(out.end(), blk, blk + r);
        if (r < 256) break;
    }
    return out;
}
static bool is_playing(const SoundPlayer& p, const char* name) {
    const char* x = sound_player_playing(&p);
    return name ? x && !std::strcmp(x, name) : !x;
}
static SoundResult play(SoundPlayer& p, size_t i, int vol = -1, bool loop = false, SoundOwner o = SoundOwner::USER) {
    return sound_player_play(&p, (int)i, vol, loop, 0, o);
}
static int played_peak(SoundPlayer& p, size_t i, int vol) {   // play it through and take the peak
    if (play(p, i, vol) != SoundResult::OK) return -1;
    return peak(render_player(p));
}
static const SoundNote kMel[] = {{440, 100}, {0, 50}, {880, 100}};

static void result_texts() {
    CHECK(!std::strcmp(sound_result_text(SoundResult::OK), "ok") && !std::strcmp(sound_result_text(SoundResult::MUTED), "muted") &&
          !std::strcmp(sound_result_text(SoundResult::BUSY), "an urgent sound is playing") &&
          !std::strcmp(sound_result_text(SoundResult::UNKNOWN), "unknown sound") &&
          !std::strcmp(sound_result_text(SoundResult::NO_LOOP), "this sound cannot loop") &&
          !std::strcmp(sound_result_text(SoundResult::BAD_ARG), "bad value (volume 0-100, hz 100-8000, ms 1-10000, 1-32 notes)") &&
          !std::strcmp(sound_result_text(SoundResult::ABSENT), "no speaker on this hub"),
          "texts: every result has its text");
}

static void policy() {
    SoundPlayer p = mk();
    CHECK(is_playing(p, nullptr), "policy: a new player is idle");
    CHECK(play(p, iBeep) == SoundResult::OK && is_playing(p, "beep"), "policy: idle, play beep is OK and plays beep");
    // an urgent sound blocks everything that is not urgent
    p = mk();
    CHECK(play(p, iAlarm, -1, true) == SoundResult::OK && is_playing(p, "alarm"), "policy: alarm plays");
    CHECK(play(p, iDoorbell) == SoundResult::BUSY, "policy: alarm playing, doorbell is BUSY");
    CHECK(sound_player_tone(&p, 440, 100, -1) == SoundResult::BUSY, "policy: alarm playing, tone is BUSY");
    CHECK(sound_player_melody(&p, kMel, 3, -1) == SoundResult::BUSY, "policy: alarm playing, melody is BUSY");
    CHECK(is_playing(p, "alarm"), "policy: a refused call leaves the alarm playing");
    CHECK(play(p, iSiren, -1, true) == SoundResult::OK && is_playing(p, "siren"), "policy: alarm playing, siren is OK and replaces it");
    // arguments are checked before state
    CHECK(play(p, iBeep, 101) == SoundResult::BAD_ARG && play(p, 99) == SoundResult::UNKNOWN &&
          play(p, 3, -1, true) == SoundResult::NO_LOOP && is_playing(p, "siren"),
          "policy: BAD_ARG, UNKNOWN and NO_LOOP come before BUSY and leave the sound alone");
    CHECK(sound_player_tone(&p, 99, 100, -1) == SoundResult::BAD_ARG && sound_player_tone(&p, 440, 100, 101) == SoundResult::BAD_ARG &&
          sound_player_melody(&p, kMel, 0, -1) == SoundResult::BAD_ARG && is_playing(p, "siren"),
          "policy: bad tone and melody arguments are BAD_ARG, not BUSY");
    // a non-urgent sound is replaced by another
    p = mk();
    play(p, iDoorbell);
    CHECK(play(p, 3) == SoundResult::OK && is_playing(p, "chime"), "policy: doorbell playing, chime is OK and replaces it");
    CHECK(sound_player_tone(&p, 440, 100, -1) == SoundResult::OK && is_playing(p, "tone"), "policy: a tone replaces it too");
    CHECK(sound_player_melody(&p, kMel, 3, -1) == SoundResult::OK && is_playing(p, "melody"), "policy: and so does a melody");
    // muted
    p = mk();
    sound_player_set_muted(&p, true);
    CHECK(play(p, iBeep) == SoundResult::MUTED && sound_player_tone(&p, 440, 100, -1) == SoundResult::MUTED &&
          sound_player_melody(&p, kMel, 3, -1) == SoundResult::MUTED && is_playing(p, nullptr),
          "policy: muted, beep, tone and melody return MUTED");
    CHECK(sound_player_tone(&p, 99, 100, -1) == SoundResult::BAD_ARG && play(p, iBeep, 101) == SoundResult::BAD_ARG &&
          play(p, 99) == SoundResult::UNKNOWN && play(p, 3, -1, true) == SoundResult::NO_LOOP,
          "policy: muted, bad arguments are still reported as such");
    CHECK(play(p, iFire, -1, true) == SoundResult::OK && is_playing(p, "fire"), "policy: muted, fire is OK");
    sound_player_set_muted(&p, false);
    CHECK(play(p, iBeep) == SoundResult::BUSY, "policy: unmuted, a beep over fire is BUSY");
    // muting a playing sound
    p = mk();
    play(p, iBeep, -1, true);
    sound_player_set_muted(&p, true);
    CHECK(is_playing(p, nullptr) && render_player(p).empty(), "policy: muting stops a looping beep");
    p = mk();
    play(p, iFire, -1, true);
    sound_player_set_muted(&p, true);
    CHECK(is_playing(p, "fire") && !render_player(p).empty(), "policy: muting does not stop fire");
    // the rest
    p = mk();
    CHECK(play(p, 99) == SoundResult::UNKNOWN && play(p, -1) == SoundResult::UNKNOWN, "policy: an unknown index is UNKNOWN");
    CHECK(play(p, 3, -1, true) == SoundResult::NO_LOOP, "policy: chime with loop is NO_LOOP");
    CHECK(play(p, iBeep, 101) == SoundResult::BAD_ARG && play(p, iBeep, -2) == SoundResult::BAD_ARG, "policy: volume 101 and -2 are BAD_ARG");
    CHECK(is_playing(p, nullptr), "policy: the refused calls started nothing");
    CHECK(play(p, iBeep, 100) == SoundResult::OK && play(p, iBeep, 0) == SoundResult::OK, "policy: volume 0 and 100 are accepted");
}

static void volume() {
    SoundPlayer p = mk(70);
    CHECK(p.volume == 40, "volume: after init(70) the default is 40");
    p = mk(30);
    CHECK(p.volume == 30, "volume: after init(30) the default is held to 30");
    p = mk(70);
    CHECK(sound_player_set_volume(&p, 100) == 70 && p.volume == 70, "volume: set_volume(100) is held to 70");
    CHECK(sound_player_set_volume(&p, -5) == 0 && p.volume == 0, "volume: set_volume(-5) is 0");
    p = mk(70);
    const int p100 = played_peak(p, iBeep, 100), p70 = played_peak(p, iBeep, 70);
    CHECK(p100 > 0 && p100 == p70, "volume: play at 100 peaks like play at 70 when the max is 70");
    CHECK(played_peak(p, iBeep, 40) < p70, "volume: play at 40 is quieter");
    sound_player_set_volume(&p, 55);
    const int d = played_peak(p, iBeep, -1), e = played_peak(p, iBeep, 55);
    CHECK(d > 0 && d == e, "volume: -1 is the default volume");
    sound_player_tone(&p, 1000, 20, -1);
    const int t1 = peak(render_player(p));
    sound_player_tone(&p, 1000, 20, 55);
    CHECK(t1 > 0 && t1 == peak(render_player(p)), "volume: a tone's -1 is the default volume too");
    sound_player_melody(&p, kMel, 3, -1);
    const int m1 = peak(render_player(p));
    sound_player_melody(&p, kMel, 3, 55);
    CHECK(m1 > 0 && m1 == peak(render_player(p)), "volume: and a melody's");
    sound_player_tone(&p, 1000, 20, 100);
    const int t100 = peak(render_player(p));
    sound_player_tone(&p, 1000, 20, 70);
    CHECK(t100 == peak(render_player(p)), "volume: a tone at 100 is held to the max too");
    p = mk(70);
    sound_player_set_volume(&p, 0);
    CHECK(played_peak(p, iBeep, -1) == 0, "volume: a default of 0 is silent");
}

static void stop() {
    SoundPlayer p = mk();
    CHECK(!sound_player_stop(&p, nullptr) && !sound_player_stop(&p, "beep") && !sound_player_stop_owner(&p, SoundOwner::USER), "stop: idle, nothing stops");
    play(p, iAlarm, -1, true);
    CHECK(!sound_player_stop(&p, "beep") && is_playing(p, "alarm"), "stop: stop(beep) while alarm plays is false and alarm plays on");
    CHECK(!sound_player_stop(&p, "alar") && !sound_player_stop(&p, "alarms") && is_playing(p, "alarm"), "stop: the name is exact");
    CHECK(sound_player_stop(&p, nullptr) && is_playing(p, nullptr), "stop: stop(nullptr) stops it");
    play(p, iAlarm, -1, true);
    CHECK(sound_player_stop(&p, "") && is_playing(p, nullptr), "stop: stop(\"\") is any");
    play(p, iAlarm, -1, true);
    CHECK(sound_player_stop(&p, "alarm") && is_playing(p, nullptr) && render_player(p).empty(), "stop: stop(alarm) stops alarm and the renderer is idle");
    sound_player_tone(&p, 440, 100, -1);
    CHECK(!sound_player_stop(&p, "melody") && sound_player_stop(&p, "tone"), "stop: a tone stops by the name tone");
    // by owner
    play(p, iDoorbell);
    CHECK(!sound_player_stop_owner(&p, SoundOwner::DEVICE) && is_playing(p, "doorbell"), "stop: a USER doorbell is not stopped by DEVICE");
    CHECK(sound_player_stop_owner(&p, SoundOwner::USER) && is_playing(p, nullptr), "stop: but by USER it is");
    SoundWarning w;
    CHECK(sound_warning_text("burglar,high,1,180", &w) && sound_player_warn(&p, w) == SoundResult::OK, "stop: warn burglar starts");
    CHECK(sound_player_stop_owner(&p, SoundOwner::DEVICE) && is_playing(p, nullptr), "stop: stop_owner(DEVICE) stops the warning");
    sound_player_warn(&p, w);
    CHECK(play(p, iSiren, -1, true, SoundOwner::USER) == SoundResult::OK && !sound_player_stop_owner(&p, SoundOwner::DEVICE) &&
          is_playing(p, "siren"), "stop: a USER siren that replaced the warning is not stopped by DEVICE");
}

static void render() {
    SoundPlayer p = mk();
    int16_t blk[256];
    CHECK(sound_player_render(&p, blk, 256) == 0, "render: idle renders 0");
    play(p, iBeep);
    CHECK(is_playing(p, "beep") && render_player(p).size() == 1920, "render: beep renders 1920 samples in total");
    CHECK(is_playing(p, nullptr), "render: the block that came back short left the player idle");
    CHECK(sound_player_render(&p, blk, 256) == 0 && is_playing(p, nullptr), "render: then 0, and idle");
    // 256 samples = 16 ms: a voice of an exact multiple ends with a call that returns 0
    sound_player_tone(&p, 1000, 16, -1);
    CHECK(sound_player_render(&p, blk, 256) == 256 && is_playing(p, "tone"), "render: a full last block keeps the voice until the next call");
    CHECK(sound_player_render(&p, blk, 256) == 0 && is_playing(p, nullptr), "render: the next call returns 0 and the player goes idle");
    play(p, iBeep);
    CHECK(sound_player_render(&p, blk, 0) == 0 && is_playing(p, "beep"), "render: n = 0 changes nothing");
    CHECK(sound_player_render(&p, nullptr, 256) == 0 && is_playing(p, "beep"), "render: a null buffer changes nothing");
    // after the end, a new sound plays from the start
    render_player(p);
    CHECK(play(p, iBeep) == SoundResult::OK && render_player(p).size() == 1920, "render: a sound after one that ended renders in full");
}

struct WCase { const char* in; bool ok, stop; const char* sound; int volume; uint32_t secs; };
static void warning() {
    const WCase cases[] = {
        {"burglar,high,1,180",          true, false, "alarm", 55, 180},
        {"fire",                        true, false, "fire",  40, 10},
        {"fire_panic,low",              true, false, "fire",  25, 10},
        {"police_panic,very_high,0,5",  true, false, "siren", 70, 5},
        {"emergency",                   true, false, "siren", 40, 10},
        {"alarm",                       true, false, "siren", 40, 10},
        {"ON",                          true, false, "siren", 40, 10},
        {"emergency_panic,medium,true,60", true, false, "siren", 40, 60},
        {"burglar,low,false,7",         true, false, "alarm", 25, 7},
        {"burglar,high,0000180",        false, false, nullptr, 0, 0},   // 3 fields: strobe "0000180" is not a strobe
        {"burglar,high,1,0000180",      true, false, "alarm", 55, 180},
        {"burglar,high,1,65535",        true, false, "alarm", 55, 600},
        {"burglar,high,1,99999999999999999999", true, false, "alarm", 55, 600},
        {"burglar,high,1,600",          true, false, "alarm", 55, 600},
        {"burglar,high,1,601",          true, false, "alarm", 55, 600},
        {"burglar,high,1,4294967296",   true, false, "alarm", 55, 600},   // 2^32: would wrap to 0
        {"burglar,high,1,4294967346",   true, false, "alarm", 55, 600},   // 2^32 + 50: would wrap to 50
        {"stop",                        true, true,  nullptr, 0, 0},
        {"OFF",                         true, true,  nullptr, 0, 0},
        {"burglar,high,1,0",            true, true,  nullptr, 0, 0},
        {"stop,low,1,30",               true, true,  nullptr, 0, 0},
        {"bogus",                       false, false, nullptr, 0, 0},
        {"burglar,loud",                false, false, nullptr, 0, 0},
        {"burglar,high,2",              false, false, nullptr, 0, 0},
        {"burglar,high,1,x",            false, false, nullptr, 0, 0},
        {"",                            false, false, nullptr, 0, 0},
        {"Burglar",                     false, false, nullptr, 0, 0},   // case matters
        {"on",                          false, false, nullptr, 0, 0},
        {"fir",                         false, false, nullptr, 0, 0},   // a word, not a prefix of one
        {"O",                           false, false, nullptr, 0, 0},
        {"burglar,hi",                  false, false, nullptr, 0, 0},
        {"burglar,high,tru",            false, false, nullptr, 0, 0},
        {"burglar, high",               false, false, nullptr, 0, 0},   // no trimming
        {"burglar ,high",               false, false, nullptr, 0, 0},
        {"burglar,,1",                  false, false, nullptr, 0, 0},   // empty field
        {",high",                       false, false, nullptr, 0, 0},
        {"fire,",                       false, false, nullptr, 0, 0},
        {"burglar,high,1,",             false, false, nullptr, 0, 0},
        {"burglar,high,1,10,extra",     false, false, nullptr, 0, 0},   // at most 4 fields
        {"burglar,high,1,10,",          false, false, nullptr, 0, 0},
        {"burglar,high,1,-5",           false, false, nullptr, 0, 0},
        {"burglar,high,1,5s",           false, false, nullptr, 0, 0},
        {"burglar,high,1,+5",           false, false, nullptr, 0, 0},
        {"burglar,high,yes,5",          false, false, nullptr, 0, 0},
        {"stop,loud",                   false, false, nullptr, 0, 0},   // every field is checked, stop or not
    };
    for (const WCase& c : cases) {
        SoundWarning w = {false, 12345, 54321, 99999};
        const bool ok = sound_warning_text(c.in, &w);
        bool good = ok == c.ok;
        if (good && ok) {
            if (c.stop) good = w.stop;
            else good = !w.stop && w.idx == sound_find(c.sound) && w.volume == c.volume && w.secs == c.secs;
        } else if (good) {
            good = w.idx == 12345 && w.volume == 54321 && w.secs == 99999 && !w.stop;   // a failed parse leaves the result alone
        }
        char msg[96];
        std::snprintf(msg, sizeof msg, "warning: \"%s\" %s", c.in, c.ok ? (c.stop ? "stops" : "parses") : "fails");
        CHECK(good, msg);
    }
    SoundWarning w;
    CHECK(!sound_warning_text(nullptr, &w) && !sound_warning_text("fire", nullptr), "warning: a null text or result fails");
    w = sound_warning_secs(10);
    CHECK(!w.stop && w.idx == (int)iSiren && w.volume == 40 && w.secs == 10, "warning: secs(10) is {siren, 40, 10}");
    CHECK(sound_warning_secs(0).stop, "warning: secs(0) stops");
    w = sound_warning_secs(65535);
    CHECK(!w.stop && w.secs == 600, "warning: secs(65535) is held to 600");
}

static void warn_timing() {
    SoundPlayer p = mk();
    SoundWarning w = {false, (int)iAlarm, 70, 2};
    CHECK(sound_player_warn(&p, w) == SoundResult::OK && is_playing(p, "alarm"), "warn_timing: warn alarm 2 s plays alarm");
    const auto s = render_player(p);
    CHECK(s.size() >= 32000 - 160 && s.size() <= 32000 + 160, "warn_timing: it renders 2 s");
    CHECK(is_playing(p, nullptr), "warn_timing: then the player is idle");
    w = {false, (int)iAlarm, 70, 1000};
    sound_player_warn(&p, w);
    const size_t n = render_player(p).size();
    CHECK(n >= 599u * 16000 && n <= 600u * 16000, "warn_timing: 1000 s renders no more than 600 s");
    w = {false, (int)iAlarm, 70, 0xFFFFFFFFu};
    sound_player_warn(&p, w);
    const size_t m = render_player(p).size();
    CHECK(m >= 599u * 16000 && m <= 600u * 16000, "warn_timing: 4294967295 s does not overflow");
    SoundWarning b;
    sound_warning_text("burglar", &b);
    sound_player_set_muted(&p, true);
    CHECK(sound_player_warn(&p, b) == SoundResult::OK && is_playing(p, "alarm"), "warn_timing: muted, warn burglar is OK and plays");
    sound_player_set_muted(&p, false);
    // stop
    SoundWarning stop;
    sound_warning_text("stop", &stop);
    CHECK(sound_player_warn(&p, stop) == SoundResult::OK && is_playing(p, nullptr), "warn_timing: warn stop stops the DEVICE sound");
    CHECK(sound_player_warn(&p, stop) == SoundResult::OK, "warn_timing: warn stop on an idle player is OK");
    play(p, iSiren, -1, true, SoundOwner::USER);
    CHECK(sound_player_warn(&p, stop) == SoundResult::OK && is_playing(p, "siren"), "warn_timing: warn stop leaves a USER sound alone");
    sound_player_stop(&p, nullptr);
    SoundWarning zero = {false, (int)iAlarm, 70, 0};
    CHECK(sound_player_warn(&p, zero) == SoundResult::OK && is_playing(p, nullptr), "warn_timing: 0 s is a stop, not a 10 min loop");
    // a warning replaces another and a user sound
    sound_player_warn(&p, b);
    SoundWarning f;
    sound_warning_text("fire,low,0,5", &f);
    CHECK(sound_player_warn(&p, f) == SoundResult::OK && is_playing(p, "fire"), "warn_timing: a new warning replaces the playing one");
    SoundWarning bad = {false, 99, 40, 5};
    CHECK(sound_player_warn(&p, bad) == SoundResult::UNKNOWN && is_playing(p, "fire"), "warn_timing: a bad warning is refused and leaves the sound alone");
}

static void exposes() {
    CHECK(alarm_output_misfit(AlarmOutKind::WARNING, kHubSoundExposes, std::strlen(kHubSoundExposes)) == nullptr,
          "exposes: the Hub Sound's exposes fit an alarm warning output");
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
    result_texts();
    policy();
    volume();
    stop();
    render();
    warning();
    warn_timing();
    exposes();
    std::printf("%s (%d failed)\n", g_fail ? "FAILED" : "ALL PASSED", g_fail);
    return g_fail ? 1 : 0;
}
