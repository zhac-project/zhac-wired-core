// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "log_cloud_core.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "json_buf.h"

namespace logcloud {
namespace {

// A record: len u16 | seq u32 | up_ms u64 | level u8 | taglen u8 | tag | msg. Host byte order: the same chip
// writes and reads it.
constexpr uint32_t kRecHead = 16;
constexpr uint32_t kRecMax  = kRecHead + kTagMax + kMsgMax;

// The cloud link's own tags (and this feature's): their lines describe the upload itself.
const char* const kLinkTags[] = {
    "remote_client", "websocket_client", "transport_ws", "transport_base", "transport",
    "esp-tls", "esp-tls-mbedtls", "log_cloud",
};

uint32_t bounded_len(const char* s, size_t max) {
    uint32_t n = 0;
    while (s && n < max && s[n]) n++;
    return n;
}

}  // namespace

const char* level_name(Level l) {
    switch (l) {
    case Level::Errors:     return "errors";
    case Level::Warnings:   return "warnings";
    case Level::Everything: return "everything";
    default:                return "off";
    }
}

bool parse_level(const char* s, Level* out) {
    if (!s) return false;
    for (uint8_t v = 0; v <= 3; v++) {
        if (std::strcmp(s, level_name(static_cast<Level>(v))) == 0) { *out = static_cast<Level>(v); return true; }
    }
    return false;
}

Level level_from_u8(uint8_t v) { return v <= 3 ? static_cast<Level>(v) : Level::Off; }

bool wants(Level setting, char level, const char* tag) {
    int rank;
    switch (level) {
    case 'E': rank = 1; break;
    case 'W': rank = 2; break;
    case 'I': rank = 3; break;
    default:  return false;
    }
    if (rank > static_cast<int>(setting)) return false;   // Off (0) lets nothing through
    if (tag) {
        for (const char* t : kLinkTags) if (std::strcmp(tag, t) == 0) return false;
    }
    return true;
}

uint32_t crc32(const void* data, size_t n) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint32_t c = 0xFFFFFFFFu;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

Ring::Ring(void* block, size_t bytes, bool sealed)
    : h_(static_cast<Header*>(block)),
      data_(static_cast<uint8_t*>(block) + sizeof(Header)),
      cap_(bytes > sizeof(Header) ? static_cast<uint32_t>(bytes - sizeof(Header)) : 0),
      sealed_(sealed) {}

void Ring::seal() { if (sealed_) h_->crc = crc32(h_, offsetof(Header, crc)); }

void Ring::format(uint32_t magic, uint32_t run_id, uint32_t image) {
    std::memset(h_, 0, sizeof(Header));
    h_->magic   = magic;
    h_->version = kVersion;
    h_->run_id  = run_id;
    h_->image   = image;
    seal();
}

void Ring::clear() {
    h_->head = h_->tail = h_->used = h_->count = 0;
    h_->dropped = 0;
    seal();
}

uint32_t Ring::take_dropped() {
    const uint32_t d = h_->dropped;
    if (d) { h_->dropped = 0; seal(); }
    return d;
}

void Ring::read_at(uint32_t off, void* dst, size_t n) const {
    if (n == 0) return;
    const size_t first = n < cap_ - off ? n : cap_ - off;
    std::memcpy(dst, data_ + off, first);
    if (n > first) std::memcpy(static_cast<uint8_t*>(dst) + first, data_, n - first);
}

void Ring::write_at(uint32_t off, const void* src, size_t n) {
    if (n == 0) return;
    const size_t first = n < cap_ - off ? n : cap_ - off;
    std::memcpy(data_ + off, src, first);
    if (n > first) std::memcpy(data_, static_cast<const uint8_t*>(src) + first, n - first);
}

bool Ring::decode_at(uint32_t off, Line* out, uint32_t* len_out) const {
    uint8_t head[kRecHead];
    read_at(off, head, kRecHead);
    const uint32_t len    = head[0] | (static_cast<uint32_t>(head[1]) << 8);
    const uint32_t taglen = head[15];
    const char     level  = static_cast<char>(head[14]);
    if (len < kRecHead || len > kRecMax || taglen > kTagMax || kRecHead + taglen > len) return false;
    const uint32_t msglen = len - kRecHead - taglen;
    if (msglen > kMsgMax) return false;
    if (level != 'E' && level != 'W' && level != 'I') return false;
    std::memcpy(&out->seq, head + 2, sizeof(out->seq));
    std::memcpy(&out->up_ms, head + 6, sizeof(out->up_ms));
    out->level = level;
    read_at((off + kRecHead) % cap_, out->tag, taglen);
    out->tag[taglen] = '\0';
    read_at((off + kRecHead + taglen) % cap_, out->msg, msglen);
    out->msg[msglen] = '\0';
    *len_out = len;
    return true;
}

bool Ring::valid(uint32_t magic, uint32_t image) const {
    if (cap_ < kRecHead) return false;
    if (h_->magic != magic || h_->version != kVersion || h_->image != image) return false;
    if (h_->crc != crc32(h_, offsetof(Header, crc))) return false;
    if (h_->head >= cap_ || h_->tail >= cap_ || h_->used > cap_ || h_->count > cap_ / kRecHead) return false;
    uint32_t off = h_->tail, total = 0;
    for (uint32_t i = 0; i < h_->count; i++) {
        Line l;
        uint32_t len = 0;
        if (!decode_at(off, &l, &len)) return false;
        total += len;
        if (total > h_->used) return false;
        off = (off + len) % cap_;
    }
    return total == h_->used && off == h_->head;
}

void Ring::drop_oldest() {
    uint8_t head[2];
    read_at(h_->tail, head, sizeof(head));
    const uint32_t len = head[0] | (static_cast<uint32_t>(head[1]) << 8);
    if (h_->count == 0 || len < kRecHead || len > h_->used) {   // never for a ring this code wrote
        h_->head = h_->tail = h_->used = h_->count = 0;
        return;
    }
    h_->tail = (h_->tail + len) % cap_;
    h_->used -= len;
    h_->count--;
    h_->dropped++;
}

void Ring::push(uint32_t seq, uint64_t up_ms, char level, const char* tag, const char* msg) {
    if (level != 'E' && level != 'W' && level != 'I') return;
    const uint32_t taglen = bounded_len(tag, kTagMax);
    const uint32_t msglen = bounded_len(msg, kMsgMax);
    const uint32_t len = kRecHead + taglen + msglen;
    if (len > cap_) return;
    if (cap_ - h_->used < len) {
        // Make room and commit that first: a crash while the new bytes go in must never leave the header
        // describing records whose bytes are being overwritten.
        while (cap_ - h_->used < len) drop_oldest();
        seal();
    }
    uint8_t head[kRecHead];
    head[0] = static_cast<uint8_t>(len & 0xFF);
    head[1] = static_cast<uint8_t>(len >> 8);
    std::memcpy(head + 2, &seq, sizeof(seq));
    std::memcpy(head + 6, &up_ms, sizeof(up_ms));
    head[14] = static_cast<uint8_t>(level);
    head[15] = static_cast<uint8_t>(taglen);
    write_at(h_->head, head, kRecHead);
    write_at((h_->head + kRecHead) % cap_, tag, taglen);
    write_at((h_->head + kRecHead + taglen) % cap_, msg, msglen);
    h_->head = (h_->head + len) % cap_;
    h_->used += len;
    h_->count++;
    seal();
}

bool Ring::peek(Line* out) const {
    if (h_->count == 0) return false;
    uint32_t len = 0;
    return decode_at(h_->tail, out, &len);
}

bool Ring::pop_if(uint32_t seq) {
    if (h_->count == 0) return false;
    uint8_t head[6];
    read_at(h_->tail, head, sizeof(head));
    uint32_t s = 0;
    std::memcpy(&s, head + 2, sizeof(s));
    const uint32_t len = head[0] | (static_cast<uint32_t>(head[1]) << 8);
    if (s != seq || len < kRecHead || len > h_->used) return false;
    h_->tail = (h_->tail + len) % cap_;
    h_->used -= len;
    h_->count--;
    seal();
    return true;
}

size_t line_json(char* out, size_t cap, const Line& l, bool with_seq) {
    JsonWriter w(out, cap);
    w.fmt("[%" PRIu64 ",\"%c\",\"", l.up_ms, l.level);
    w.str(l.tag, kTagMax);
    w.raw("\",\"");
    w.str(l.msg, kMsgMax);
    w.ch('"');
    if (with_seq) w.fmt(",%" PRIu32, l.seq);
    w.ch(']');
    return w.finish();
}

BatchWriter::BatchWriter(char* buf, size_t cap) : buf_(buf), cap_(cap < kBudget ? cap : kBudget) {}

void BatchWriter::begin(uint32_t boot, uint32_t first_seq, uint64_t up_ms, int64_t clock_ms, uint32_t dropped) {
    n_ = 0;
    const int w = std::snprintf(buf_, cap_,
        "{\"boot\":%" PRIu32 ",\"seq\":%" PRIu32 ",\"up\":%" PRIu64 ",\"clock\":%" PRId64 ",\"dropped\":%" PRIu32 ",\"lines\":[",
        boot, first_seq, up_ms, clock_ms, dropped);
    ok_  = w > 0 && static_cast<size_t>(w) < cap_;
    pos_ = ok_ ? static_cast<size_t>(w) : 0;
}

bool BatchWriter::add(const Line& l) {
    if (!ok_) return false;
    const size_t len = line_json(line_, sizeof(line_), l, false);
    if (len == 0) return false;
    const size_t need = (n_ ? 1 : 0) + len + 2;    // a comma, the line, and the closing "]}"
    if (pos_ + need > cap_) return false;
    if (n_) buf_[pos_++] = ',';
    std::memcpy(buf_ + pos_, line_, len);
    pos_ += len;
    n_++;
    return true;
}

size_t BatchWriter::finish() {
    if (!ok_ || n_ == 0 || pos_ + 2 > cap_) return 0;
    buf_[pos_++] = ']';
    buf_[pos_++] = '}';
    ok_ = false;                                    // one finish per begin
    return pos_;
}

const char* reset_name(Reset r) {
    switch (r) {
    case Reset::PowerOn:  return "power_on";
    case Reset::Panic:    return "panic";
    case Reset::IntWdt:   return "int_wdt";
    case Reset::TaskWdt:  return "task_wdt";
    case Reset::Wdt:      return "wdt";
    case Reset::Brownout: return "brownout";
    case Reset::Sw:       return "sw";
    case Reset::Ext:      return "ext";
    default:              return "other";
    }
}

const char* planned_name(uint32_t p) {
    switch (static_cast<Planned>(p)) {
    case Planned::Update:   return "update";
    case Planned::Restart:  return "restart";
    case Planned::Rollback: return "rollback";
    default:                return nullptr;
    }
}

PrevRun prev_run(bool tail_valid, uint32_t tail_planned, bool new_image, Reset reason, bool rolled_back) {
    constexpr auto code = [](Planned p) { return static_cast<uint32_t>(p); };
    // On trial, the previous run was always another image: whatever sits at this address is not ours to read.
    if (new_image) return {false, code(Planned::Update)};
    // A block that announced an image change was left before another image ran: this image's tail from an
    // earlier life (update, then rollback), not its previous run.
    const bool left = tail_planned == code(Planned::Update) || tail_planned == code(Planned::Rollback);
    if (tail_valid && !left) return {true, tail_planned};
    // The flag outlives the boot after the rollback, so it counts only when no tail of this image's own runs
    // explains a software restart: that is the boot right after the rollback.
    if (reason == Reset::Sw && rolled_back) return {false, code(Planned::Rollback)};
    return {false, code(Planned::None)};
}

const char* riscv_cause_name(uint32_t mcause) {
    static const char* const kNames[] = {
        "instruction address misaligned", "instruction access fault", "illegal instruction", "breakpoint",
        "load address misaligned", "load access fault", "store address misaligned", "store access fault",
        "environment call from U-mode", "environment call from S-mode", nullptr, "environment call from M-mode",
        "instruction page fault", "load page fault", nullptr, "store page fault",
    };
    if (mcause & 0x80000000u) return "interrupt";
    const uint32_t code = mcause & 0x7FFFFFFFu;
    if (code < sizeof(kNames) / sizeof(kNames[0]) && kNames[code]) return kNames[code];
    return "unknown cause";
}

size_t crash_json(char* out, size_t cap, const char* task, uint32_t pc, const char* cause, const char* elf) {
    JsonWriter w(out, cap);
    w.raw("{\"task\":\"");
    w.str(task ? task : "", 16);
    w.fmt("\",\"pc\":\"0x%08" PRIx32 "\",\"cause\":\"", pc);
    w.str(cause ? cause : "", 96);
    w.raw("\",\"elf\":\"");
    w.str(elf ? elf : "", 16);
    w.raw("\"}");
    const size_t n = w.finish();
    if (n == 0 || n >= cap) {                       // no room for the terminating NUL either
        if (cap) out[0] = '\0';
        return 0;
    }
    out[n] = '\0';
    return n;
}

size_t tail_json(char* out, size_t cap, const TailHead& h, const Ring* prev) {
    const size_t budget = cap < BatchWriter::kBudget ? cap : BatchWriter::kBudget;
    JsonWriter w(out, budget);
    w.fmt("{\"boot\":%" PRIu32 ",\"prev_boot\":%" PRIu32 ",\"up\":%" PRIu64 ",\"clock\":%" PRId64 ",\"reason\":\"",
          h.boot, h.prev_boot, h.up_ms, h.clock_ms);
    w.str(h.reason ? h.reason : "other", 16);
    w.raw("\",\"planned\":");
    if (h.planned) { w.ch('"'); w.str(h.planned, 16); w.ch('"'); } else { w.raw("null"); }
    w.fmt(",\"prev_clock_base\":%" PRId64 ",\"prev_sent\":%" PRIu32 ",\"crash\":", h.prev_clock_base_ms, h.prev_sent);
    w.raw(h.crash_json ? h.crash_json : "null");
    w.raw(",\"lines\":[");
    if (!w.ok()) return 0;

    // Pass 1: the encoded size of every line the cloud does not have yet.
    constexpr size_t kMaxLines = 256;               // a 4 KB tail holds at most ~240 records
    uint16_t sizes[kMaxLines];
    size_t n = 0;
    char line[1536];
    auto unsent = [&](const Line& l) { return l.seq == 0 || l.seq > h.prev_sent; };
    if (prev) {
        prev->for_each([&](const Line& l) {
            if (n < kMaxLines && unsent(l)) sizes[n++] = static_cast<uint16_t>(line_json(line, sizeof(line), l, true));
        });
    }
    // The newest that fit: walk back from the end; k lines take their sizes plus k-1 commas.
    const size_t room = budget - w.len() - 2;      // keep "]}"
    size_t first = n, total = 0;
    while (first > 0) {
        const size_t add = sizes[first - 1] + (total ? 1 : 0);
        if (sizes[first - 1] == 0 || total + add > room) break;
        total += add;
        first--;
    }
    // Pass 2: write lines [first, n).
    size_t idx = 0;
    bool any = false;
    if (prev) {
        prev->for_each([&](const Line& l) {
            if (!unsent(l) || idx >= kMaxLines) return;
            if (idx++ < first) return;
            const size_t len = line_json(line, sizeof(line), l, true);
            if (any) w.ch(',');
            w.raw(line, len);
            any = true;
        });
    }
    w.raw("]}");
    return w.finish();
}

}  // namespace logcloud
