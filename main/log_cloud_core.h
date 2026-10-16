// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The pure core of "Log to cloud" (spec 2026-10-06 cloud logs §4): the level setting, the line filter, the
// drop-oldest record ring used for both the PSRAM backlog and the restart tail, and (Task 3) the JSON builders
// for log.batch and log.tail. No ESP-IDF here, so main/test/host builds it on the host.
#pragma once

#include <cstddef>
#include <cstdint>

namespace logcloud {

// The hub's "Log to cloud" level; the value is what NVS sys_cfg/log_cloud holds.
enum class Level : uint8_t { Off = 0, Errors = 1, Warnings = 2, Everything = 3 };

const char* level_name(Level l);                  // "off" | "errors" | "warnings" | "everything"
bool        parse_level(const char* s, Level* out); // the four names only
Level       level_from_u8(uint8_t v);             // anything above 3 is Off

// A line enters the backlog when the level lets it through and its tag is not one of the cloud link's own:
// lines that describe the upload would only feed the upload.
bool wants(Level setting, char level, const char* tag);

uint32_t crc32(const void* data, size_t n);       // IEEE 802.3 (reflected 0xEDB88320)

constexpr size_t kTagMax = 32;                     // bytes kept of a tag
constexpr size_t kMsgMax = 191;                    // bytes kept of a message (the log hook's line cap)

struct Line {
    uint32_t seq;                                  // backlog sequence number; 0 = never queued
    uint64_t up_ms;                                // uptime when logged
    char     level;                                // 'E' 'W' 'I'
    char     tag[kTagMax + 1];
    char     msg[kMsgMax + 1];
};

// A drop-oldest ring of variable-size line records kept entirely inside a caller-owned block (header, then
// data), so a block in __NOINIT_ATTR RAM survives a software restart and is checked at the next boot. The
// block must be 8-byte aligned. Not thread-safe: the caller holds its lock around every call.
class Ring {
public:
    struct Header {
        uint32_t magic;
        uint32_t version;
        uint32_t run_id;          // the run (boot epoch) that writes this ring
        uint32_t sent;            // last backlog sequence number handed to the cloud link
        int64_t  clock_base_ms;   // wall clock minus uptime; 0 = the clock was never set
        uint32_t planned;         // Planned code, written just before a planned restart
        uint32_t head;            // offset of the next write in the data area
        uint32_t tail;            // offset of the oldest record
        uint32_t used;            // bytes in use
        uint32_t count;           // records in use
        uint32_t dropped;         // records dropped to make room since take_dropped()
        uint32_t crc;             // crc32 of every field above
    };
    static constexpr uint32_t kVersion = 1;

    // sealed (the restart tail): the header crc is kept current and valid() can vouch for the block. Unsealed
    // (the PSRAM backlog, which never survives a restart): the crc is never computed, so push/pop stay cheap;
    // valid() is not meaningful there.
    Ring(void* block, size_t bytes, bool sealed = true);
    void     format(uint32_t magic, uint32_t run_id);   // empty ring, every header field reset
    bool     valid(uint32_t magic) const;              // header crc, bounds and every record's frame
    void     push(uint32_t seq, uint64_t up_ms, char level, const char* tag, const char* msg);
    bool     peek(Line* out) const;                    // the oldest record
    bool     pop_if(uint32_t seq);                     // drops the oldest record if it carries seq
    void     clear();                                  // empty; keeps run_id, sent, planned, clock_base_ms
    uint32_t take_dropped();
    uint32_t count() const { return h_->count; }
    uint32_t used() const { return h_->used; }
    Header&       hdr()       { return *h_; }
    const Header& hdr() const { return *h_; }
    void     seal();                                   // recompute the crc after editing header fields (no-op if unsealed)

    template <class F> void for_each(F&& f) const {    // oldest first; f(const Line&)
        uint32_t off = h_->tail;
        for (uint32_t i = 0; i < h_->count; i++) {
            Line l;
            uint32_t len = 0;
            if (!decode_at(off, &l, &len)) return;
            f(static_cast<const Line&>(l));
            off = (off + len) % cap_;
        }
    }

private:
    void read_at(uint32_t off, void* dst, size_t n) const;
    void write_at(uint32_t off, const void* src, size_t n);
    bool decode_at(uint32_t off, Line* out, uint32_t* len) const;
    void drop_oldest();

    Header*  h_;
    uint8_t* data_;
    uint32_t cap_;
    bool     sealed_;
};

// One line as JSON: [up_ms,"L","tag","msg"], plus ",seq" for the tail. Escaped, and UTF-8 safe: a byte that is
// not part of a complete sequence (the hook cuts lines at 191 bytes) becomes �. 0 = it does not fit.
size_t line_json(char* out, size_t cap, const Line& l, bool with_seq);

// Builds one log.batch payload (spec 2026-10-06 §4.3) of at most kBudget bytes:
// {"boot","seq","up","clock","dropped","lines":[...]}. begin(), add() lines until it says no, finish() once.
class BatchWriter {
public:
    static constexpr size_t kBudget = 6144;   // one page of the cloud link (remote_xfer.h kRemotePageBudget)
    BatchWriter(char* buf, size_t cap);
    void   begin(uint32_t boot, uint32_t first_seq, uint64_t up_ms, int64_t clock_ms, uint32_t dropped);
    bool   add(const Line& l);                // false = it would not fit; nothing was written
    size_t lines() const { return n_; }
    size_t finish();                          // the payload length; 0 when no line was added

private:
    char*  buf_;
    size_t cap_;
    size_t pos_ = 0;
    size_t n_   = 0;
    bool   ok_  = false;
    char   line_[1536];                       // one encoded line; the worst case (all escapes) is ~1.4 KB
};

// Why the hub restarted, as log.tail names it.
enum class Reset : uint8_t { Other = 0, PowerOn, Panic, IntWdt, TaskWdt, Wdt, Brownout, Sw, Ext };
const char* reset_name(Reset r);              // "power_on" "panic" "int_wdt" "task_wdt" "wdt" "brownout" "sw" "ext" "other"

// Written into the tail just before a planned esp_restart(), so the next run can tell why.
enum class Planned : uint32_t { None = 0, Update = 1, Restart = 2, Rollback = 3 };
const char* planned_name(uint32_t p);         // nullptr for None or an unknown value

const char* riscv_cause_name(uint32_t mcause);   // "load access fault", …; "interrupt"; "unknown cause"

// {"task":"…","pc":"0x…","cause":"…","elf":"…"}, NUL-terminated. 0 (and "") when it does not fit.
size_t crash_json(char* out, size_t cap, const char* task, uint32_t pc, const char* cause, const char* elf);

struct TailHead {
    uint32_t    boot;                 // this run
    uint32_t    prev_boot;            // 0 when the previous run left no tail
    uint64_t    up_ms;
    int64_t     clock_ms;             // 0 = the clock is not set
    const char* reason;               // reset_name()
    const char* planned;              // planned_name(), may be nullptr
    int64_t     prev_clock_base_ms;
    uint32_t    prev_sent;
    const char* crash_json;           // a JSON object, or nullptr
};

// One log.tail payload (spec 2026-10-06 §4.4) of at most BatchWriter::kBudget bytes. `prev` may be nullptr.
// Lines whose seq is 1..prev_sent are left out (the cloud has them); of the rest, the newest that fit are
// kept, oldest first.
size_t tail_json(char* out, size_t cap, const TailHead& h, const Ring* prev);

}  // namespace logcloud
