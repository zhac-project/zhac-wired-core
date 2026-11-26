// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// /api/sound: the hub page's Settings > Sound card (spec 2026-11-25 hub sounds). Local only, like /api/io.
// The checks of the bodies and the GET body are in api_sound_json.h (host-tested); this is the httpd glue.
#include "api_sound.h"

#include "sdkconfig.h"

#if CONFIG_ZHAC_AUDIO

#include <cstdio>
#include <cstdlib>

#include "api_sound_json.h"
#include "auth.h"
#include "http_body.h"
#include "sound.h"

namespace {

constexpr size_t kBodyMax = 256;
constexpr size_t kOutMax  = 2048;   // 16 sounds are about 1 KB

esp_err_t reply_state(httpd_req_t* req) {
    const SoundView v{sound_present(), sound_volume(), sound_max_volume(), sound_muted(), sound_playing(), sound_error()};
    char* out = static_cast<char*>(std::malloc(kOutMax));
    const size_t n = out ? sound_state_json(out, kOutMax, v) : 0;
    if (!n) {
        std::free(out);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    const esp_err_t e = httpd_resp_send(req, out, static_cast<ssize_t>(n));
    std::free(out);
    return e;
}

esp_err_t reply_error(httpd_req_t* req, const char* err) {
    // Fixed firmware texts (api_sound_json.h's, sound_result_text's, and sound_error()'s step name and
    // esp_err_to_name): no quotes or backslashes.
    char out[160];
    std::snprintf(out, sizeof(out), "{\"error\":\"%s\"}", err);
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out);
    return ESP_FAIL;
}

esp_err_t reply_ok(httpd_req_t* req) {
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

esp_err_t handle_get(httpd_req_t* req) { return reply_state(req); }

esp_err_t handle_put(httpd_req_t* req) {
    char body[kBodyMax];
    const int n = http_read_body(req, body, sizeof(body));
    SoundPut p;
    const char* err = n > 0 ? sound_put_parse(body, static_cast<size_t>(n), &p) : kSoundBadBody;
    if (err) return reply_error(req, err);
    // Both are checked before either is applied. The setters hold the volume to the maximum and never write
    // flash (the sound task saves it), so this is safe on the httpd task.
    if (p.volume >= 0) sound_set_volume(p.volume);
    if (p.muted >= 0) sound_set_muted(p.muted != 0);
    return reply_state(req);
}

esp_err_t handle_play(httpd_req_t* req) {
    char body[kBodyMax], name[40];
    const int n = http_read_body(req, body, sizeof(body));
    int volume = -1;
    const char* err = n > 0 ? sound_play_parse(body, static_cast<size_t>(n), name, sizeof(name), &volume) : kSoundBadBody;
    if (err) return reply_error(req, err);
    const SoundResult r = sound_play(name, volume, false);
    if (r == SoundResult::OK) return reply_ok(req);
    char why[96];   // "speaker off: " and sound_error()'s reason, or a result's text
    sound_refusal(r, sound_error(), why, sizeof(why));
    return reply_error(req, why);
}

esp_err_t handle_stop(httpd_req_t* req) {
    sound_stop(nullptr);
    return reply_ok(req);
}

}  // namespace

bool api_sound_register(httpd_handle_t hd) {
    if (!hd) return false;
    httpd_uri_t u{};
    u.uri = "/api/sound"; u.method = HTTP_GET; u.handler = handle_get;
    bool ok = auth_register_uri(hd, &u) == ESP_OK;
    u.method = HTTP_PUT; u.handler = handle_put;
    ok = auth_register_uri(hd, &u) == ESP_OK && ok;
    u.uri = "/api/sound/play"; u.method = HTTP_POST; u.handler = handle_play;
    ok = auth_register_uri(hd, &u) == ESP_OK && ok;
    u.uri = "/api/sound/stop"; u.handler = handle_stop;
    ok = auth_register_uri(hd, &u) == ESP_OK && ok;
    return ok;
}

#else

bool api_sound_register(httpd_handle_t) { return false; }

#endif
