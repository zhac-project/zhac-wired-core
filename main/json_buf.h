// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// PROVENANCE: copied verbatim from zhac-net-core/main/. Both apps need the same
// per-device group mirror and there is no shared component for it yet. Keep the
// two in sync, or promote this to zhac-components -- do NOT let them drift, the
// SPA talks the same protocol to both.
#pragma once
#include <cstddef>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include "utf8_seq.h"   // utf8_seq_len: the strict RFC 3629 validator (one copy, shared with remote_client)

// Bounded copy of a user-supplied UTF-8 string into a fixed field (e.g.
// GrpRecord::name[32]). Unlike strncpy, truncation lands on a code-point
// boundary: a multibyte char that does not fit whole is dropped, never split.
// A byte-blind strncpy stored a dangling lead byte in NVS; every JSON reply
// carrying it was an invalid-UTF-8 WS text frame, which Chrome and Tomcat
// answer by closing the socket (the group.list "not connected" / cloud
// reconnect loop). Copy stops at the first invalid source sequence (browser
// input is valid UTF-8; anything else is garbage we refuse to store). Always
// NUL-terminates.
inline void utf8_safe_copy(char* dst, size_t dstsz, const char* src) {
    if (!dst || dstsz == 0) return;
    size_t o = 0;
    if (src) {
        while (src[o]) {
            size_t n = utf8_seq_len(reinterpret_cast<const unsigned char*>(src) + o, 4);
            if (n == 0 || o + n > dstsz - 1) break;
            for (size_t k = 0; k < n; k++) dst[o + k] = src[o + k];
            o += n;
        }
    }
    dst[o] = '\0';
}

// Checked bounded JSON append (CODEX H-01/H-02).
//
// The hand-rolled `pos += snprintf(buf + pos, cap - pos, ...)` idiom is unsafe:
// on truncation snprintf returns the length it WOULD have written, so `pos` can
// exceed `cap`; the next `buf + pos` then points past the buffer and
// `cap - pos` underflows to a huge size_t, turning the following write into an
// out-of-bounds write. JsonWriter keeps `pos_ <= cap_` at all times: any append
// that would not fit latches `ok_ = false` and stops advancing, so callers fail
// closed (finish() returns 0) instead of walking off the end. Responses are
// length-delimited, so no trailing NUL is required — `fmt()` may write one
// within `cap_` transiently but it is always inside the buffer.
class JsonWriter {
public:
    JsonWriter(char* buf, size_t cap) : buf_(buf), cap_(cap) {}

    // Raw literal (no escaping). NUL-terminated source ONLY — copies until the
    // first NUL. Do NOT pass a buffer whose length you know but which may not be
    // terminated at that length (it will over-read into whatever follows); use
    // the (s, len) overload for that.
    JsonWriter& raw(const char* s) {
        if (!ok_) return *this;
        for (; *s; ++s) if (!put(*s)) break;
        return *this;
    }

    // Length-bounded raw copy — exactly `len` bytes, no NUL scan. Use when
    // appending another writer's output (whose byte length you already have):
    // a NUL-bounded raw() over-reads a non-terminated buffer past its content
    // into uninitialized memory, which corrupted the group.list WS frame.
    JsonWriter& raw(const char* s, size_t len) {
        if (!ok_) return *this;
        for (size_t i = 0; i < len; ++i) if (!put(s[i])) break;
        return *this;
    }
    JsonWriter& ch(char c) { if (ok_) put(c); return *this; }

    // printf-style. Fails closed on truncation.
    JsonWriter& fmt(const char* f, ...) __attribute__((format(printf, 2, 3))) {
        if (!ok_) return *this;
        va_list ap; va_start(ap, f);
        size_t rem = cap_ - pos_;                 // pos_ <= cap_ invariant
        int n = vsnprintf(buf_ + pos_, rem, f, ap);
        va_end(ap);
        if (n < 0 || (size_t)n >= rem) { ok_ = false; return *this; }
        pos_ += (size_t)n;
        return *this;
    }

    // JSON-escaped string CONTENT (caller supplies the surrounding quotes).
    // Bounded by `max` source bytes so a non-terminated on-flash string can't
    // be over-read (H-01: NVS-loaded GrpRecord::name).
    //
    // UTF-8 SANITIZING: responses travel as WebSocket TEXT frames, and RFC
    // 6455 §8.1 lets the peer kill the connection on invalid UTF-8 — Chrome
    // and Tomcat both do (observed: a group name strncpy-truncated mid-
    // sequence in NVS made every group.list reply close the local SPA socket
    // AND the cloud hub link, in an endless reconnect loop). Valid sequences
    // pass through byte-identical; any invalid/truncated sequence is emitted
    // as the JSON escape � (U+FFFD REPLACEMENT CHARACTER) and decoding
    // resyncs on the next byte. This also heals bad bytes already persisted
    // in NVS at serialize time — no storage migration needed.
    JsonWriter& str(const char* s, size_t max) {
        if (!ok_) return *this;
        size_t i = 0;
        while (i < max && s[i]) {
            unsigned char c = (unsigned char)s[i];
            if (c == '"' || c == '\\' || c == '\n' || c == '\r' || c == '\t') {
                char e = (c == '"') ? '"' : (c == '\\') ? '\\' :
                         (c == '\n') ? 'n' : (c == '\r') ? 'r' : 't';
                if (!put('\\') || !put(e)) break;
                i++;
            } else if (c < 0x20) {
                char u[8];
                int n = snprintf(u, sizeof(u), "\\u%04x", c);
                if (n != 6) { ok_ = false; break; }
                bool fit = true;
                for (int k = 0; k < 6 && fit; ++k) fit = put(u[k]);
                if (!fit) break;
                i++;
            } else if (c < 0x80) {
                if (!put((char)c)) break;
                i++;
            } else {
                size_t n = utf8_seq_len(
                    reinterpret_cast<const unsigned char*>(s) + i, max - i);
                if (n) {
                    bool fit = true;
                    for (size_t k = 0; k < n && fit; ++k) fit = put((char)s[i + k]);
                    if (!fit) break;
                    i += n;
                } else {
                    static constexpr char kRep[] = "\\ufffd";   // U+FFFD, ASCII-safe
                    bool fit = true;
                    for (int k = 0; k < 6 && fit; ++k) fit = put(kRep[k]);
                    if (!fit) break;
                    i++;   // resync one byte past the bad lead/continuation
                }
            }
        }
        return *this;
    }

    bool   ok()  const { return ok_; }
    size_t len() const { return pos_; }
    // Final length, or 0 if any append overflowed (fail closed).
    size_t finish() const { return ok_ ? pos_ : 0; }

private:
    bool put(char c) {
        if (pos_ >= cap_) { ok_ = false; return false; }
        buf_[pos_++] = c;
        return true;
    }
    char*  buf_;
    size_t cap_;
    size_t pos_ = 0;
    bool   ok_  = true;
};
