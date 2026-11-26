// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// lua_io.cpp's zhac.sound_* under the vendored Lua 5.5 (hub-sounds review M4, L195): a number a script gets
// wrong is `nil, err`, never an error that ends the script, and a refusal says why the speaker is off.
// Behind the bindings: the real player from sound_synth.cpp, with sound.cpp's admission (no codec, or one
// that did not start: ABSENT) but not its lock or I2S. The pin calls are refused stubs.
#include "lua_engine.h"
#include "sound.h"
#include "user_io.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#include <cstdio>
#include <string>

static int g_fail = 0;
#define CHECK(c, m) do { if (c) std::printf("PASS: %s\n", m); else { std::printf("FAIL: %s\n", m); g_fail++; } } while (0)

// ---- fakes -----------------------------------------------------------------------------------
static const luaL_Reg* g_fns   = nullptr;
static SoundPlayer     g_p;
static bool            g_codec = true;   // sound_present()
static const char*     g_off   = "";     // sound_error(): set = the codec answered but did not start

void lua_engine_set_zhac_extension(const luaL_Reg* fns) { g_fns = fns; }

template <typename F>
static SoundResult admit(F&& f) { return !g_codec || *g_off ? SoundResult::ABSENT : f(); }

SoundResult sound_play(const char* name, int volume, bool loop) {
    return admit([&] { return sound_player_play(&g_p, sound_find(name), volume, loop, 0, SoundOwner::USER); });
}
SoundResult sound_tone(int hz, int ms, int volume) { return admit([&] { return sound_player_tone(&g_p, hz, ms, volume); }); }
SoundResult sound_melody(const SoundNote* n, size_t count, int volume) {
    return admit([&] { return sound_player_melody(&g_p, n, count, volume); });
}
bool        sound_stop(const char* name) { return sound_player_stop(&g_p, name); }
const char* sound_playing() { return sound_player_playing(&g_p); }
int         sound_volume() { return g_p.volume; }
int         sound_set_volume(int v) { return sound_player_set_volume(&g_p, v); }
const char* sound_error() { return g_off; }

static bool no_pins(char* err, size_t cap) {
    std::snprintf(err, cap, "no pins in this test");
    return false;
}
bool user_io_write(const char*, UserIoMode, double, char* err, size_t cap) { return no_pins(err, cap); }
int  user_io_toggle(const char*, char* err, size_t cap) { return no_pins(err, cap) ? 0 : -1; }
bool user_io_read(const char*, int32_t*, int32_t*, char* err, size_t cap) { return no_pins(err, cap); }
bool user_io_strip(const char*, const uint32_t*, size_t, char* err, size_t cap) { return no_pins(err, cap); }

// ---- the runner ------------------------------------------------------------------------------
static lua_State* L = nullptr;

// A chunk's results as text joined by ",", or "error: <message>" when it raised one.
static std::string run(const char* chunk) {
    const int   top = lua_gettop(L);
    std::string out;
    if (luaL_loadstring(L, chunk) != LUA_OK || lua_pcall(L, 0, LUA_MULTRET, 0) != LUA_OK) {
        out = std::string("error: ") + lua_tostring(L, -1);
    } else {
        for (int i = top + 1; i <= lua_gettop(L); i++) {
            if (i > top + 1) out += ",";
            out += luaL_tolstring(L, i, nullptr);
            lua_pop(L, 1);
        }
    }
    lua_settop(L, top);
    return out;
}

static void numbers() {
    const std::string bad = std::string("nil,") + sound_result_text(SoundResult::BAD_ARG);
    CHECK(run("return zhac.sound_tone(440, 100)") == "true", "tone: whole numbers play");
    CHECK(run("return zhac.sound_tone(440.5, 100)") == bad, "tone: a fractional hz is nil, bad value");
    CHECK(run("return zhac.sound_tone(440, 'long')") == bad, "tone: a word for ms is nil, bad value");
    CHECK(run("return zhac.sound_tone(440)") == bad, "tone: no ms is nil, bad value");
    CHECK(run("return zhac.sound_tone(440, 100, 40.5)") == bad, "tone: a fractional volume is nil, bad value");
    CHECK(run("return zhac.sound_play('chime', 40.5)") == bad, "play: a fractional volume is nil, bad value");
    CHECK(run("return zhac.sound_play('chime', 'loud')") == bad, "play: a word for the volume is nil, bad value");
    CHECK(run("return zhac.sound_play('chime', 40.0)") == "true", "play: 40.0 is the whole number 40");
    CHECK(run("return zhac.sound_play('chime', '40')") == "true", "play: a numeric string counts, as Lua converts it");
    CHECK(run("return zhac.sound_play('chime', 1e9)") == bad, "play: a huge volume is nil, bad value");
    CHECK(run("return zhac.sound_melody({{440, 100}}, 2.5)") == bad, "melody: a fractional volume is nil, bad value");
    CHECK(run("local ok, err = zhac.sound_tone(0.5, 1) return 'went on', ok, err") == "went on," + bad,
          "tone: the script goes on after a bad number");
}

static void speaker_off() {
    g_off = "I2S setup failed: ESP_ERR_INVALID_STATE";
    const std::string why = "nil,speaker off: I2S setup failed: ESP_ERR_INVALID_STATE";
    CHECK(run("return zhac.sound_play('chime')") == why, "play: a speaker that did not start says why");
    CHECK(run("return zhac.sound_tone(440, 100)") == why, "tone: ... the same");
    CHECK(run("return zhac.sound_melody({{440, 100}})") == why, "melody: ... the same");
    g_off   = "";
    g_codec = false;
    CHECK(run("return zhac.sound_play('chime')") == "nil,no speaker on this hub", "play: no codec is no speaker on this hub");
    g_codec = true;
}

int main() {
    sound_player_init(&g_p, 70);
    L = luaL_newstate();
    luaL_openlibs(L);
    lua_io_register();
    if (!g_fns) {
        std::printf("FAIL: lua_io_register gave no functions\n");
        return 1;
    }
    lua_newtable(L);
    luaL_setfuncs(L, g_fns, 0);
    lua_setglobal(L, "zhac");
    numbers();
    speaker_off();
    lua_close(L);
    std::printf("%s (%d failed)\n", g_fail ? "FAILED" : "ALL PASSED", g_fail);
    return g_fail ? 1 : 0;
}
