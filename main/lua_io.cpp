// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// zhac.gpio_* / pwm_write / adc_read / ws_write (spec §4): the hub pins for
// Lua, through user_io like rules and the UI. Each returns nil, err on refusal.
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

const luaL_Reg kIoFns[] = {
    {"gpio_write",  l_gpio_write},
    {"gpio_toggle", l_gpio_toggle},
    {"gpio_read",   l_gpio_read},
    {"pwm_write",   l_pwm_write},
    {"adc_read",    l_adc_read},
    {"ws_write",    l_ws_write},
    {nullptr, nullptr},
};

}  // namespace

void lua_io_register() { lua_engine_set_zhac_extension(kIoFns); }

#else

void lua_io_register() {}

#endif
