// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// /api/sound, the pure half (main/api_sound_json.h): the PUT and play bodies -- what is refused and with which
// text -- and the GET body (it parses, lists the 16 sounds, adds "error" only when there is one).
#include "api_sound_json.h"

#include <cstdio>
#include <cstring>
#include <string>

static int g_fail = 0;
#define CHECK(c, m) do { if (c) std::printf("PASS: %s\n", m); else { std::printf("FAIL: %s\n", m); g_fail++; } } while (0)

static const char* put(const char* body, SoundPut* p) { return sound_put_parse(body, std::strlen(body), p); }

static void put_bodies() {
    SoundPut p;
    CHECK(!put("{}", &p) && p.volume == -1 && p.muted == -1, "PUT {}: nothing to change");
    CHECK(!put("{\"volume\":0}", &p) && p.volume == 0 && p.muted == -1, "PUT volume 0");
    CHECK(!put("{\"volume\":100,\"muted\":true}", &p) && p.volume == 100 && p.muted == 1, "PUT volume 100 + muted");
    CHECK(!put("{\"muted\":false}", &p) && p.volume == -1 && p.muted == 0, "PUT muted false");
    CHECK(!put("{\"volume\":40,\"x\":1}", &p) && p.volume == 40, "PUT: an unknown key is ignored");
    for (const char* b : {"{\"volume\":-1}", "{\"volume\":101}", "{\"volume\":40.5}", "{\"volume\":40.0}", "{\"volume\":\"40\"}",
                          "{\"volume\":true}", "{\"volume\":null}", "{\"volume\":[40]}", "{\"volume\":99999999999}"})
        CHECK(put(b, &p) && !std::strcmp(put(b, &p), "volume must be 0-100"), b);
    for (const char* b : {"{\"muted\":1}", "{\"muted\":\"true\"}", "{\"muted\":null}", "{\"muted\":0.0}"})
        CHECK(put(b, &p) && !std::strcmp(put(b, &p), "muted must be true or false"), b);
    // Volume is checked first: a body wrong in both says volume.
    CHECK(!std::strcmp(put("{\"volume\":300,\"muted\":3}", &p), "volume must be 0-100"), "PUT: both wrong -> volume text");
    for (const char* b : {"", "{", "[]", "null", "40", "\"x\"", "{\"volume\":40"})
        CHECK(put(b, &p) && !std::strcmp(put(b, &p), "body must be a JSON object under 256 bytes"), b[0] ? b : "(empty body)");
}

static void play_bodies() {
    char name[40];
    int vol = 7;
    auto play = [&](const char* b) { return sound_play_parse(b, std::strlen(b), name, sizeof(name), &vol); };
    CHECK(!play("{\"name\":\"doorbell\"}") && !std::strcmp(name, "doorbell") && vol == -1, "play: name, default volume");
    CHECK(!play("{\"name\":\"beep\",\"volume\":0}") && !std::strcmp(name, "beep") && vol == 0, "play: volume 0");
    CHECK(!play("{\"name\":\"beep\",\"volume\":100}") && vol == 100, "play: volume 100");
    CHECK(play("{\"name\":\"beep\",\"volume\":101}") && !std::strcmp(play("{\"name\":\"beep\",\"volume\":101}"), "volume must be 0-100"),
          "play: volume 101 refused");
    CHECK(play("{\"name\":\"beep\",\"volume\":-1}") && play("{\"name\":\"beep\",\"volume\":1.5}") &&
          play("{\"name\":\"beep\",\"volume\":\"5\"}"), "play: -1, a float and a string refused like PUT's");
    CHECK(!play("{\"volume\":5}") && !name[0] && vol == 5, "play: no name -> empty (the player says unknown sound)");
    CHECK(!play("{\"name\":7}") && !name[0], "play: a number as name -> empty");
    CHECK(!play("{\"name\":null}") && !name[0], "play: a null name -> empty");
    const std::string longname = std::string("{\"name\":\"") + std::string(39, 'a') + "\"}";
    CHECK(!play(longname.c_str()) && std::strlen(name) == 39, "play: a 39-character name fits the 40-byte buffer");
    const std::string toolong = std::string("{\"name\":\"") + std::string(40, 'a') + "\"}";
    CHECK(!play(toolong.c_str()) && !name[0], "play: a 40-character name -> empty, no overflow");
    CHECK(play("") && !std::strcmp(play(""), "body must be a JSON object under 256 bytes") && !name[0], "play: empty body");
    CHECK(play("[\"beep\"]") != nullptr, "play: an array is not an object");
}

static JsonDocument parse(const char* s) {
    JsonDocument d;
    CHECK(!deserializeJson(d, s), "the GET body is valid JSON");
    return d;
}

static void state_body() {
    char out[2048];
    SoundView v{true, 40, 70, false, nullptr, ""};
    size_t n = sound_state_json(out, sizeof(out), v);
    CHECK(n > 0 && n == std::strlen(out), "GET: written, length matches");
    JsonDocument d = parse(out);
    CHECK(d["present"] == true && d["volume"] == 40 && d["max_volume"] == 70 && d["muted"] == false, "GET: scalars");
    CHECK(d["playing"].isNull() && d["playing"].is<JsonVariantConst>(), "GET: playing is an explicit null when idle");
    CHECK(d["sounds"].size() == sound_count() && sound_count() == 16, "GET: the 16 sounds");
    bool shape = true;
    for (size_t i = 0; i < sound_count(); i++) {
        JsonObjectConst s = d["sounds"][i];
        shape = shape && !std::strcmp(s["name"] | "", sound_name(i)) && s["ms"].as<uint32_t>() == sound_ms(i) &&
                s["loop"].is<bool>() && s["loop"].as<bool>() == sound_loopable(i) &&
                s["urgent"].is<bool>() && s["urgent"].as<bool>() == sound_urgent(i);
    }
    CHECK(shape, "GET: each sound has name, ms, loop and urgent from the table");
    CHECK(!d["error"].is<JsonVariantConst>(), "GET: no error key when the text is empty");
    CHECK(n < 1500, "GET: about 1 KB, well inside the 2 KB buffer");

    v = SoundView{true, 70, 70, true, "tone", nullptr};
    n = sound_state_json(out, sizeof(out), v);
    d = parse(out);
    CHECK(n > 0 && !std::strcmp(d["playing"] | "", "tone") && d["muted"] == true && !d["error"].is<JsonVariantConst>(),
          "GET: a playing name, muted, a null error text");

    v = SoundView{true, 0, 0, false, nullptr, "i2s_new_channel failed: \"ESP_ERR_NO_MEM\"\\\n"};
    n = sound_state_json(out, sizeof(out), v);
    d = parse(out);
    CHECK(n > 0 && !std::strcmp(d["error"] | "", "i2s_new_channel failed: \"ESP_ERR_NO_MEM\"\\"),
          "GET: the error text is escaped (quote, backslash) and loses a control character");

    v = SoundView{false, 0, 0, false, nullptr, ""};
    n = sound_state_json(out, sizeof(out), v);
    d = parse(out);
    CHECK(n > 0 && d["present"] == false && d["sounds"].size() == 16, "GET: no speaker still lists the sounds");

    char small[200];
    CHECK(sound_state_json(small, sizeof(small), v) == 0, "GET: too small a buffer gives 0");
    for (size_t cap = 1; cap < 20; cap++) {
        char tiny[20];
        if (sound_state_json(tiny, cap, v) != 0) { CHECK(false, "GET: a tiny buffer gives 0"); break; }
    }
}

int main() {
    put_bodies();
    play_bodies();
    state_body();
    std::printf("%s (%d failed)\n", g_fail ? "FAILED" : "ALL PASSED", g_fail);
    return g_fail ? 1 : 0;
}
