// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// /api/sound, the pure half (api_sound.cpp is the httpd glue): the request-body checks and the GET body.
// No IDF; host test: test/host/test_api_sound_json.cpp.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "ArduinoJson.h"
#include "sound_synth.h"

inline constexpr char kSoundBadBody[]   = "body must be a JSON object under 256 bytes";
inline constexpr char kSoundBadVolume[] = "volume must be 0-100";
inline constexpr char kSoundBadMuted[]  = "muted must be true or false";

// "volume": absent -> -1; else an integer 0-100 (a float, a string, a bool or a null is refused). nullptr or the 400 text.
inline const char* sound_body_volume(JsonObjectConst o, int* volume) {
    *volume = -1;
    JsonVariantConst x = o["volume"];
    if (!x.is<JsonVariantConst>()) return nullptr;            // not in the body
    if (!x.is<int>() || x.as<int>() < 0 || x.as<int>() > 100) return kSoundBadVolume;
    *volume = x.as<int>();
    return nullptr;
}

// PUT /api/sound: {"volume"?:int,"muted"?:bool}. -1 = not in the body (also what a fresh struct holds). A refusal sets neither.
struct SoundPut { int volume = -1; int muted = -1; };
inline const char* sound_put_parse(const char* body, size_t len, SoundPut* out) {
    JsonDocument doc;
    if (deserializeJson(doc, body, len) || !doc.is<JsonObject>()) return kSoundBadBody;
    const JsonObjectConst o = doc.as<JsonObjectConst>();
    if (const char* e = sound_body_volume(o, &out->volume)) return e;
    out->muted = -1;
    JsonVariantConst m = o["muted"];
    if (m.is<JsonVariantConst>()) {
        if (!m.is<bool>()) return kSoundBadMuted;
        out->muted = m.as<bool>() ? 1 : 0;
    }
    return nullptr;
}

// POST /api/sound/play: {"name":s,"volume"?:int}. A missing, non-string or too long name leaves `name` empty,
// which the player answers "unknown sound" (no extra error text); volume -1 = the default.
inline const char* sound_play_parse(const char* body, size_t len, char* name, size_t cap, int* volume) {
    name[0] = '\0';
    JsonDocument doc;
    if (deserializeJson(doc, body, len) || !doc.is<JsonObject>()) return kSoundBadBody;
    const JsonObjectConst o = doc.as<JsonObjectConst>();
    if (const char* e = sound_body_volume(o, volume)) return e;
    const char* n = o["name"].is<const char*>() ? o["name"].as<const char*>() : nullptr;
    if (n && std::strlen(n) < cap) std::strcpy(name, n);
    return nullptr;
}

// The GET /api/sound body, also PUT's answer. `error` (a short text of the firmware's own) is added when it is not empty.
struct SoundView { bool present; int volume, max_volume; bool muted; const char* playing; const char* error; };

// Bytes written, 0 if it did not fit.
inline size_t sound_state_json(char* out, size_t cap, const SoundView& v) {
    size_t n = 0;
    bool ok = true;
    auto put = [&](const char* fmt, auto... args) {
        if (!ok) return;
        const int m = std::snprintf(out + n, cap - n, fmt, args...);
        if (m < 0 || static_cast<size_t>(m) >= cap - n) { ok = false; return; }
        n += static_cast<size_t>(m);
    };
    auto lit = [&](const char* s) { put("%s", s); };
    put("{\"present\":%s,\"volume\":%d,\"max_volume\":%d,\"muted\":%s,\"playing\":", v.present ? "true" : "false",
        v.volume, v.max_volume, v.muted ? "true" : "false");
    if (v.playing) put("\"%s\"", v.playing);   // a built-in's name, "tone" or "melody": no escaping needed
    else lit("null");
    lit(",\"sounds\":[");
    for (size_t i = 0; i < sound_count(); i++)
        put("%s{\"name\":\"%s\",\"ms\":%lu,\"loop\":%s,\"urgent\":%s}", i ? "," : "", sound_name(i),
            static_cast<unsigned long>(sound_ms(i)), sound_loopable(i) ? "true" : "false",
            sound_urgent(i) ? "true" : "false");
    lit("]");
    if (v.error && *v.error) {
        lit(",\"error\":\"");
        for (const char* p = v.error; *p && ok; p++) {
            if (static_cast<unsigned char>(*p) < 0x20) continue;
            if (*p == '"' || *p == '\\') lit("\\");
            put("%c", *p);
        }
        lit("\"");
    }
    lit("}");
    return ok ? n : 0;
}
