// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// zhac.gpio_* / pwm_write / adc_read / ws_write (spec §4): the hub pins for
// Lua, through user_io like rules and the UI. Each returns nil, err on refusal.
// zhac.sound_* (hub sounds): the hub speaker, through sound.h like rules and the UI.
#include "sound.h"
#include "user_io.h"

#include "sdkconfig.h"

#if CONFIG_LUA_ENGINE_ENABLED

#include <cstdio>
#include <cstdlib>

extern "C" {   // the Lua library is built as C
#include "lauxlib.h"
#include "lua.h"
}
#include "lua_engine.h"

namespace {

// Arg 1: a pin name, or a GPIO number (as Lua passes it: integer).
const char* pin_arg(lua_State* L, char* buf, size_t cap) {
    if (lua_isinteger(L, 1)) {
        std::snprintf(buf, cap, "%lld", static_cast<long long>(lua_tointeger(L, 1)));
        return buf;
    }
    return luaL_checkstring(L, 1);
}

int fail(lua_State* L, const char* err) {
    lua_pushnil(L);
    lua_pushstring(L, err);
    return 2;
}

int l_gpio_write(lua_State* L) {
    char b[24], err[96];
    const char* pin = pin_arg(L, b, sizeof(b));
    const double v = lua_isboolean(L, 2) ? (lua_toboolean(L, 2) ? 1.0 : 0.0) : luaL_checknumber(L, 2);
    if (!user_io_write(pin, UserIoMode::OUT, v, err, sizeof(err))) return fail(L, err);
    lua_pushboolean(L, 1);
    return 1;
}

int l_gpio_toggle(lua_State* L) {
    char b[24], err[96];
    const int lvl = user_io_toggle(pin_arg(L, b, sizeof(b)), err, sizeof(err));
    if (lvl < 0) return fail(L, err);
    lua_pushinteger(L, lvl);
    return 1;
}

int l_gpio_read(lua_State* L) {
    char b[24], err[96];
    int32_t v = 0, raw = 0;
    if (!user_io_read(pin_arg(L, b, sizeof(b)), &v, &raw, err, sizeof(err))) return fail(L, err);
    lua_pushinteger(L, v);
    return 1;
}

int l_pwm_write(lua_State* L) {
    char b[24], err[96];
    const char* pin = pin_arg(L, b, sizeof(b));
    if (!user_io_write(pin, UserIoMode::PWM, luaL_checknumber(L, 2), err, sizeof(err))) return fail(L, err);
    lua_pushboolean(L, 1);
    return 1;
}

int l_adc_read(lua_State* L) {
    char b[24], err[96];
    int32_t v = 0, raw = 0;
    if (!user_io_read(pin_arg(L, b, sizeof(b)), &v, &raw, err, sizeof(err))) return fail(L, err);
    lua_pushinteger(L, v);
    lua_pushinteger(L, raw);
    return 2;
}

int l_ws_write(lua_State* L) {
    char b[24], err[96];
    const char* pin = pin_arg(L, b, sizeof(b));
    luaL_checktype(L, 2, LUA_TTABLE);
    size_t n = lua_rawlen(L, 2);
    if (n > 1024) n = 1024;   // the longest strip a pin takes; extra colours are ignored anyway
    auto* rgb = static_cast<uint32_t*>(std::malloc((n ? n : 1) * sizeof(uint32_t)));
    if (!rgb) return fail(L, "out of memory");
    for (size_t i = 0; i < n; i++) {
        lua_rawgeti(L, 2, static_cast<lua_Integer>(i + 1));
        rgb[i] = static_cast<uint32_t>(lua_tointeger(L, -1)) & 0xFFFFFF;
        lua_pop(L, 1);
    }
    const bool ok = user_io_strip(pin, rgb, n, err, sizeof(err));
    std::free(rgb);
    if (!ok) return fail(L, err);
    lua_pushboolean(L, 1);
    return 1;
}

// A volume argument: absent or nil = -1 (the default); any other integer goes to the player, which refuses
// what is not -1..100 (BAD_ARG), so a huge one is only held to an int.
int volume_arg(lua_State* L, int idx) {
    if (lua_isnoneornil(L, idx)) return -1;
    const lua_Integer v = luaL_checkinteger(L, idx);
    return v < -1000 ? -1000 : (v > 1000 ? 1000 : static_cast<int>(v));
}

int sound_result(lua_State* L, SoundResult r) {
    if (r == SoundResult::OK) { lua_pushboolean(L, 1); return 1; }
    return fail(L, sound_result_text(r));
}

int l_sound_play(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    return sound_result(L, sound_play(name, volume_arg(L, 2), lua_toboolean(L, 3) != 0));
}

int l_sound_tone(lua_State* L) {
    const lua_Integer hz = luaL_checkinteger(L, 1), ms = luaL_checkinteger(L, 2);
    auto cl = [](lua_Integer v) { return static_cast<int>(v < -1 ? -1 : (v > 100000 ? 100000 : v)); };   // the player's range is far inside
    return sound_result(L, sound_tone(cl(hz), cl(ms), volume_arg(L, 3)));
}

// notes: {{hz, ms}, ...}, at most kSoundMaxNotes; a malformed list is BAD_ARG, like a value out of range.
int l_sound_melody(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    const size_t n = lua_rawlen(L, 1);
    if (n > kSoundMaxNotes) return fail(L, sound_result_text(SoundResult::BAD_ARG));
    SoundNote notes[kSoundMaxNotes];
    for (size_t i = 0; i < n; i++) {
        lua_rawgeti(L, 1, static_cast<lua_Integer>(i + 1));
        int ok1 = 0, ok2 = 0;
        lua_Integer hz = 0, ms = 0;
        if (lua_istable(L, -1)) {
            lua_rawgeti(L, -1, 1);
            lua_rawgeti(L, -2, 2);
            hz = lua_tointegerx(L, -2, &ok1);
            ms = lua_tointegerx(L, -1, &ok2);
            lua_pop(L, 2);
        }
        lua_pop(L, 1);
        if (!ok1 || !ok2 || hz < 0 || hz > 0xFFFF || ms < 0 || ms > 0xFFFF)   // the player checks the ranges proper
            return fail(L, sound_result_text(SoundResult::BAD_ARG));
        notes[i] = {static_cast<uint16_t>(hz), static_cast<uint16_t>(ms)};
    }
    return sound_result(L, sound_melody(notes, n, volume_arg(L, 2)));
}

int l_sound_stop(lua_State* L) {
    sound_stop(luaL_optstring(L, 1, nullptr));
    lua_pushboolean(L, 1);
    return 1;
}

// With a value: the new default (an integer 0-100; the board's maximum holds it lower), which the hub keeps.
// Returns the default in force.
int l_sound_volume(lua_State* L) {
    if (!lua_isnoneornil(L, 1)) {
        int ok = 0;
        const lua_Integer v = lua_tointegerx(L, 1, &ok);
        if (!ok || v < 0 || v > 100) return fail(L, "volume must be 0-100");
        sound_set_volume(static_cast<int>(v));
    }
    lua_pushinteger(L, sound_volume());
    return 1;
}

int l_sound_playing(lua_State* L) {
    const char* name = sound_playing();
    if (name) lua_pushstring(L, name); else lua_pushnil(L);
    return 1;
}

const luaL_Reg kIoFns[] = {
    {"gpio_write",  l_gpio_write},
    {"gpio_toggle", l_gpio_toggle},
    {"gpio_read",   l_gpio_read},
    {"pwm_write",   l_pwm_write},
    {"adc_read",    l_adc_read},
    {"ws_write",    l_ws_write},
    {"sound_play",    l_sound_play},
    {"sound_tone",    l_sound_tone},
    {"sound_melody",  l_sound_melody},
    {"sound_stop",    l_sound_stop},
    {"sound_volume",  l_sound_volume},
    {"sound_playing", l_sound_playing},
    {nullptr, nullptr},
};

}  // namespace

void lua_io_register() { lua_engine_set_zhac_extension(kIoFns); }

#else

void lua_io_register() {}

#endif
