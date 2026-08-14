// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// log_ring.cpp — capture last N log lines into a ring buffer for
// /api/logs. Installs as esp_log_set_vprintf() hook; original output
// (UART/USB-CDC) is preserved.
//
// Also fans captured lines to two optional live sinks:
//   - MQTT  (topic `<root>/log/<level>`, rate-limited)
//   - WS    (frame {type:"log", entry:{…}})

#include "log_ring.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "mqtt_gw.h"
#include "ws_server.h"
#include <atomic>
#include <ctime>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cinttypes>

namespace {

constexpr size_t LOG_RING_SIZE = 128;
constexpr size_t LOG_LINE_MAX  = 192;

struct LogEntry {
    uint64_t ts_us;
    char     line[LOG_LINE_MAX];
};

LogEntry* s_ring = nullptr;
size_t    s_head  = 0;     // next write slot
size_t    s_count = 0;     // up to LOG_RING_SIZE
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

vprintf_like_t s_prev = nullptr;

// ── Live sinks state ────────────────────────────────────────────────

std::atomic<bool> s_mqtt_enabled{false};
std::atomic<char> s_mqtt_level{'I'};
// Default OFF: the log sink allocates ~900 B from PSRAM per line and
// broadcasts to every WS client, which under handler-heavy paths
// (REST/WS dispatch) caused noticeable server-side latency plus an
// observed freeze-like crawl when the SPA bootstraps. User can turn
// it on in Settings → "log_ws_enabled" when they actively want the
// Logs page to auto-update; the SPA falls back to polling `logs.get`
// otherwise.
std::atomic<bool> s_ws_enabled{false};
std::atomic<char> s_ws_level{'I'};

std::atomic<uint32_t> s_mqtt_published{0};
std::atomic<uint32_t> s_mqtt_dropped_rate{0};
std::atomic<uint32_t> s_mqtt_dropped_disc{0};

// 50 publishes/sec hard cap → drop surplus within a rolling 1-second
// window. Window counter + epoch second reset.
std::atomic<uint8_t>  s_rate_count{0};
std::atomic<uint32_t> s_rate_epoch{0};
static constexpr uint8_t MQTT_RATE_CAP_PER_SEC = 50;

// Map a level char to an ordering — E(err)=0 most-severe, V(verbose)=4
// least-severe. Level threshold `min` lets lines with severity <= min.
int level_order(char c) {
    switch (c) {
        case 'E': return 0;
        case 'W': return 1;
        case 'I': return 2;
        case 'D': return 3;
        case 'V': return 4;
        default:  return 2;   // treat unknown as info
    }
}
bool level_passes(char line_level, char min_level) {
    return level_order(line_level) <= level_order(min_level);
}
const char* level_topic_suffix(char c) {
    switch (c) {
        case 'E': return "log/err";
        case 'W': return "log/warn";
        case 'I': return "log/info";
        case 'D': return "log/debug";
        case 'V': return "log/verbose";
        default:  return "log/info";
    }
}

// Parse `I (12345) tag: msg` — works for ANSI-stripped lines.
// Returns a reasonable default on any malformed input so callers never
// blow up publishing partial data.
void parse_line(const char* line, char* out_level,
                char* out_tag, size_t tag_cap,
                const char** out_msg) {
    *out_level = 'I';
    out_tag[0] = '\0';
    *out_msg   = line;
    if (!line || !line[0]) return;
    char lvl = line[0];
    if (lvl == 'E' || lvl == 'W' || lvl == 'I' || lvl == 'D' || lvl == 'V') {
        *out_level = lvl;
    } else {
        return;   // raw printf output; leave tag empty and msg = full line
    }
    const char* p = strchr(line, ')');
    if (!p) return;
    p++;
    while (*p == ' ') p++;
    const char* colon = strchr(p, ':');
    if (!colon) return;
    size_t tlen = (size_t)(colon - p);
    if (tlen >= tag_cap) tlen = tag_cap - 1;
    memcpy(out_tag, p, tlen);
    out_tag[tlen] = '\0';
    p = colon + 1;
    while (*p == ' ') p++;
    *out_msg = p;
}

// Build the JSON body shared by MQTT + WS. Returns bytes written.
// Shape: {"ts":12345,"src":"s3","tag":"...","msg":"..."}
// (level intentionally omitted — it's in the MQTT topic already, and
// WS consumers get it from the wrapping `level` field at dispatch.)
size_t build_body_json(char* out, size_t cap,
                        const char* src, const char* tag, const char* msg) {
    uint32_t ts = (uint32_t)time(nullptr);
    int n = snprintf(out, cap, "{\"ts\":%" PRIu32 ",\"src\":\"%s\",\"tag\":\"",
                     ts, src);
    if (n < 0 || (size_t)n >= cap) return 0;
    size_t pos = (size_t)n;
    auto emit_escaped = [&](const char* s) -> bool {
        for (; *s; s++) {
            unsigned char c = (unsigned char)*s;
            if (pos + 6 >= cap) return false;
            if (c == '"' || c == '\\') {
                out[pos++] = '\\';
                out[pos++] = (char)c;
            } else if (c < 0x20) {
                int w = snprintf(out + pos, cap - pos, "\\u%04x", c);
                if (w < 0) return false;
                pos += (size_t)w;
            } else {
                out[pos++] = (char)c;
            }
        }
        return true;
    };
    if (!emit_escaped(tag)) return 0;
    if (pos + 10 >= cap) return 0;
    memcpy(out + pos, "\",\"msg\":\"", 9); pos += 9;
    if (!emit_escaped(msg)) return 0;
    if (pos + 3 >= cap) return 0;
    out[pos++] = '"'; out[pos++] = '}'; out[pos] = '\0';
    return pos;
}

// Rate-limit gate — returns true if the publish should proceed, bumps
// the counter otherwise. Resets every wall-clock second.
bool mqtt_rate_allow() {
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    uint32_t epoch = s_rate_epoch.load(std::memory_order_relaxed);
    if (epoch != now) {
        s_rate_epoch.store(now, std::memory_order_relaxed);
        s_rate_count.store(1, std::memory_order_relaxed);
        return true;
    }
    uint8_t c = s_rate_count.fetch_add(1, std::memory_order_relaxed);
    return c < MQTT_RATE_CAP_PER_SEC;
}

void dispatch_to_sinks(const char* src, char level,
                       const char* tag, const char* msg) {
    const bool mqtt_on = s_mqtt_enabled.load(std::memory_order_acquire);
    const bool ws_on   = s_ws_enabled.load(std::memory_order_acquire);
    if (!mqtt_on && !ws_on) return;

    // Allocate scratch from PSRAM rather than the caller's stack
    // (TaskGPIORst / TaskAlertPrst have ~2 KB stacks, too tight for
    // the 900 B of locals this routine needs). PSRAM allocs take
    // single-digit microseconds and the log rate is bounded by the
    // MQTT rate-limit (~128/s), so the churn is negligible. Single
    // heap_caps_malloc + single free per line keeps the path fast.
    // Separate allocations — GCC's -Wrestrict rejects `snprintf` when
    // src and dst can be inferred to alias (they do if we carve all
    // three out of one block).
    constexpr size_t kBodyCap  = 384;
    constexpr size_t kFrameCap = 480;
    constexpr size_t kTopicCap = 64;
    char* body  = static_cast<char*>(heap_caps_malloc(kBodyCap,  MALLOC_CAP_SPIRAM));
    char* frame = static_cast<char*>(heap_caps_malloc(kFrameCap, MALLOC_CAP_SPIRAM));
    char* topic = static_cast<char*>(heap_caps_malloc(kTopicCap, MALLOC_CAP_SPIRAM));
    if (!body || !frame || !topic) {
        heap_caps_free(body); heap_caps_free(frame); heap_caps_free(topic);
        return;   // OOM: silently drop this log line
    }

    size_t body_len = build_body_json(body, kBodyCap, src, tag, msg);
    if (body_len == 0) {
        heap_caps_free(body); heap_caps_free(frame); heap_caps_free(topic);
        return;
    }

    if (mqtt_on && level_passes(level, s_mqtt_level.load())) {
        if (!mqtt_gw_is_connected()) {
            s_mqtt_dropped_disc.fetch_add(1, std::memory_order_relaxed);
        } else if (!mqtt_rate_allow()) {
            s_mqtt_dropped_rate.fetch_add(1, std::memory_order_relaxed);
        } else if (mqtt_gw_format_topic(topic, kTopicCap,
                                          level_topic_suffix(level)) > 0) {
            mqtt_gw_publish(topic, body, strlen(body), 0, false);
            s_mqtt_published.fetch_add(1, std::memory_order_relaxed);
        }
    }

    if (ws_on && level_passes(level, s_ws_level.load())) {
        // Only the legacy frame remains — the new SPA polls
        // `logs.get` every 5 s from pages/Logs.jsx instead of
        // subscribing to `log.entry` events. Per-log ws_event
        // fan-out created alloc + mutex pressure that stalled
        // TaskHTTP under bootstrap load.
        int w = snprintf(frame, kFrameCap,
                         "{\"type\":\"log\",\"level\":\"%c\",\"entry\":%s}",
                         level, body);
        if (w > 0 && (size_t)w < kFrameCap) {
            ws_server_broadcast(frame, (size_t)w);
        }
    }

    heap_caps_free(body);
    heap_caps_free(frame);
    heap_caps_free(topic);
}

int log_vprintf(const char* fmt, va_list args) {
    // Forward to original sink (serial). vprintf consumes args — take a
    // copy for our second formatting pass.
    va_list args2;
    va_copy(args2, args);
    const int fwd = s_prev ? s_prev(fmt, args) : vprintf(fmt, args);

    if (s_ring) {
        char buf[LOG_LINE_MAX];
        int w = vsnprintf(buf, sizeof(buf), fmt, args2);
        if (w > 0) {
            size_t n = (size_t)w < sizeof(buf) ? (size_t)w : sizeof(buf) - 1;
            while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' ||
                             buf[n - 1] == 0x1b)) {
                n--;
            }
            buf[n] = '\0';
            // Strip ANSI colour codes: ESC '[' <digits> 'm' (ESP-IDF tags
            // log lines with "\x1b[0;32m…\x1b[0m" in default log format).
            char clean[LOG_LINE_MAX];
            size_t ci = 0;
            for (size_t i = 0; i < n && ci < sizeof(clean) - 1; i++) {
                if (buf[i] == 0x1b && i + 1 < n && buf[i + 1] == '[') {
                    i += 2;
                    while (i < n && buf[i] != 'm') i++;
                    continue;
                }
                clean[ci++] = buf[i];
            }
            clean[ci] = '\0';

            portENTER_CRITICAL(&s_mux);
            LogEntry& e = s_ring[s_head];
            e.ts_us = esp_timer_get_time();
            memcpy(e.line, clean, ci + 1);
            s_head = (s_head + 1) % LOG_RING_SIZE;
            if (s_count < LOG_RING_SIZE) s_count++;
            portEXIT_CRITICAL(&s_mux);

            // Fan out to live sinks (MQTT / WS). Source is always "s3"
            // here — P4-relayed lines come in through
            // log_sinks_publish_p4() instead.
            char lvl = 'I';
            char tag[32];
            const char* msg = nullptr;
            parse_line(clean, &lvl, tag, sizeof(tag), &msg);
            if (msg) dispatch_to_sinks("s3", lvl, tag, msg);
        }
    }
    va_end(args2);
    return fwd;
}

}  // namespace

void log_ring_init() {
    if (s_ring) return;
    s_ring = static_cast<LogEntry*>(
        heap_caps_calloc(LOG_RING_SIZE, sizeof(LogEntry),
                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!s_ring) {
        ESP_LOGE("log_ring", "alloc failed");
        return;
    }
    s_prev = esp_log_set_vprintf(log_vprintf);
    ESP_LOGI("log_ring", "ready (capacity=%u lines, %u bytes)",
             (unsigned)LOG_RING_SIZE,
             (unsigned)(LOG_RING_SIZE * sizeof(LogEntry)));
}

size_t log_ring_to_json(char* out, size_t cap) {
    if (!s_ring || !out || cap < 16) return 0;

    size_t pos = 0;
    int w = snprintf(out + pos, cap - pos, "{\"logs\":[");
    if (w < 0 || (size_t)w >= cap - pos) return 0;
    pos += (size_t)w;

    // Walk from oldest to newest. When s_count < RING, oldest is at 0;
    // otherwise the ring has wrapped and oldest is at s_head.
    portENTER_CRITICAL(&s_mux);
    const size_t count = s_count;
    const size_t start = (s_count == LOG_RING_SIZE) ? s_head : 0;
    portEXIT_CRITICAL(&s_mux);

    for (size_t i = 0; i < count; i++) {
        size_t idx = (start + i) % LOG_RING_SIZE;
        // Snapshot under the lock to avoid writer mid-append.
        LogEntry snap;
        portENTER_CRITICAL(&s_mux);
        snap = s_ring[idx];
        portEXIT_CRITICAL(&s_mux);

        if (pos + 4 >= cap) return 0;
        if (i > 0) out[pos++] = ',';
        out[pos++] = '"';

        // Emit ts as ms prefix for UI convenience.
        w = snprintf(out + pos, cap - pos, "%llu ",
                     (unsigned long long)(snap.ts_us / 1000ULL));
        if (w < 0 || (size_t)w >= cap - pos) return 0;
        pos += (size_t)w;

        // Escape string content for JSON. Only the few chars that matter.
        for (const char* p = snap.line; *p && pos + 6 < cap; p++) {
            unsigned char c = (unsigned char)*p;
            if (c == '"' || c == '\\') {
                out[pos++] = '\\';
                out[pos++] = (char)c;
            } else if (c < 0x20) {
                w = snprintf(out + pos, cap - pos, "\\u%04x", c);
                if (w < 0) return 0;
                pos += (size_t)w;
            } else {
                out[pos++] = (char)c;
            }
        }
        if (pos + 2 >= cap) return 0;
        out[pos++] = '"';
    }

    if (pos + 3 > cap) return 0;
    out[pos++] = ']';
    out[pos++] = '}';
    out[pos]   = '\0';
    return pos;
}

// ── Sink public API ─────────────────────────────────────────────────

void log_sinks_set_mqtt(bool enabled, char min_level) {
    s_mqtt_enabled.store(enabled, std::memory_order_release);
    s_mqtt_level.store(min_level ? min_level : 'I', std::memory_order_release);
}

void log_sinks_set_ws(bool enabled, char min_level) {
    s_ws_enabled.store(enabled, std::memory_order_release);
    s_ws_level.store(min_level ? min_level : 'I', std::memory_order_release);
}

bool log_sinks_get_mqtt_enabled() { return s_mqtt_enabled.load(); }
bool log_sinks_get_ws_enabled()   { return s_ws_enabled.load();   }
char log_sinks_get_mqtt_level()   { return s_mqtt_level.load();   }
char log_sinks_get_ws_level()     { return s_ws_level.load();     }

uint32_t log_sinks_mqtt_published()             { return s_mqtt_published.load(); }
uint32_t log_sinks_mqtt_dropped_rate()          { return s_mqtt_dropped_rate.load(); }
uint32_t log_sinks_mqtt_dropped_disconnected()  { return s_mqtt_dropped_disc.load(); }

void log_sinks_publish_p4(char level, const char* tag, const char* msg) {
    if (!msg) return;
    // Also stash into the ring so /api/logs snapshots include P4 lines,
    // formatted in the same style as ESP_LOG entries but without the
    // vprintf round trip.
    if (s_ring) {
        char line[LOG_LINE_MAX];
        int w = snprintf(line, sizeof(line), "%c (%lu) %s: %s",
                          level ? level : 'I',
                          (unsigned long)(esp_timer_get_time() / 1000ULL),
                          tag ? tag : "p4", msg);
        if (w > 0) {
            portENTER_CRITICAL(&s_mux);
            LogEntry& e = s_ring[s_head];
            e.ts_us = esp_timer_get_time();
            size_t n = ((size_t)w < sizeof(line)) ? (size_t)w : sizeof(line) - 1;
            memcpy(e.line, line, n); e.line[n] = '\0';
            s_head = (s_head + 1) % LOG_RING_SIZE;
            if (s_count < LOG_RING_SIZE) s_count++;
            portEXIT_CRITICAL(&s_mux);
        }
    }
    dispatch_to_sinks("p4", level ? level : 'I', tag ? tag : "p4", msg);
}
