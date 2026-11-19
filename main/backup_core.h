// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Full backup core (research 2026-11-18 C1), over any flash: the snapshot and the file writer (download), the stager
// (restore upload into the inactive OTA slot) and the boot-time apply. backup.cpp wires it to esp_partition and the
// web server. Host check on fake NOR flash with power cuts: test/host/backup/.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "backup_crypto.h"
#include "backup_format.h"

namespace backup {

// A partition. On the hub: NOR flash, erase sets 4096-byte sectors to 0xFF, a write only clears bits.
struct Flash {
    virtual ~Flash() = default;
    virtual uint32_t size() const = 0;
    virtual bool read(uint32_t off, void* dst, size_t n) = 0;
    virtual bool write(uint32_t off, const void* src, size_t n) = 0;
    virtual bool erase(uint32_t off, size_t n) = 0;   // on sector boundaries
};

struct Sink {
    virtual ~Sink() = default;
    virtual bool put(const uint8_t* p, size_t n) = 0;
};

constexpr size_t   kPiece = 4096;     // data moved per step; the io buffers hold kPiece + 16
constexpr uint32_t kBlock = 0x10000;  // the stager erases ahead in 64 KB blocks (faster than sectors)
constexpr int      kSnapshotTries = 5;

enum class Err { None, Format, Parts, Newer, Password, Busy, Kdf, Crypto, Sink, Flash, Length, Damaged, Space };

// Copy every partition into its buffer as they all were at one instant: read them all, then read them all again
// comparing. A store that writes in between shows as a difference and the copy starts over. Equal passes mean nothing
// changed between the two reads of any byte, so the first pass is the state at the moment it ended: what a power
// cut then would have left, which NVS and the file system are built to recover from. `tmp` holds kPiece bytes.
inline bool snapshot(Flash* const* parts, uint8_t* const* bufs, size_t n, uint8_t* tmp, int tries, void (*pause)()) {
    for (int t = 0; t < tries; t++) {
        if (t && pause) pause();
        bool ok = true;
        for (size_t i = 0; ok && i < n; i++) ok = parts[i]->read(0, bufs[i], parts[i]->size());
        for (size_t i = 0; ok && i < n; i++) {
            for (uint32_t off = 0; ok && off < parts[i]->size(); off += kPiece) {
                const size_t len = parts[i]->size() - off < kPiece ? parts[i]->size() - off : kPiece;
                ok = parts[i]->read(off, tmp, len) && memcmp(tmp, bufs[i] + off, len) == 0;
            }
        }
        if (ok) return true;
    }
    return false;
}

// The whole file, to `out`: header `h` (everything set but key_check), then the partitions, one snapshot of all of
// them, encrypted, then the tag. Nothing is sent before the snapshot holds. `io` holds kPiece + 16 bytes.
inline Err write_file(Header h, const char* pw, size_t pw_len, Flash* const* parts, uint8_t* const* bufs, Sink& out,
                      uint8_t* io, void (*pause)()) {
    for (uint16_t i = 0; i < h.n_parts; i++) if (parts[i]->size() != h.parts[i].size) return Err::Parts;
    uint8_t key[32], hdr[kHeaderLen];
    if (!derive_key(pw, pw_len, h.salt, h.kdf_iter, key) || !key_check(key, h.key_check)) return Err::Kdf;
    Gcm g;
    pack(h, hdr);
    const bool keyed = g.begin(key, h.nonce, hdr, kHeaderLen, true);
    memset(key, 0, sizeof key);
    if (!keyed) return Err::Crypto;
    if (!snapshot(parts, bufs, h.n_parts, io, kSnapshotTries, pause)) return Err::Busy;
    if (!out.put(hdr, kHeaderLen)) return Err::Sink;
    size_t n = 0;
    for (uint16_t i = 0; i < h.n_parts; i++) {
        for (uint32_t off = 0; off < h.parts[i].size; off += kPiece) {
            const size_t len = h.parts[i].size - off < kPiece ? h.parts[i].size - off : kPiece;
            if (!g.update(bufs[i] + off, len, io, kPiece + 16, &n)) return Err::Crypto;
            if (n && !out.put(io, n)) return Err::Sink;
        }
    }
    uint8_t tag[kTagLen];
    if (!g.finish_encrypt(io, kPiece + 16, &n, tag)) return Err::Crypto;
    if ((n && !out.put(io, n)) || !out.put(tag, kTagLen)) return Err::Sink;
    return Err::None;
}

// Restore, before anything is written: the header against this hub (`why` names a partition that differs).
inline Err check(const uint8_t* hdr, Header* h, const Part* hub, size_t n_hub, const char* hub_fw, const char** why) {
    *why = nullptr;
    if (!parse(hdr, h)) return Err::Format;
    if (!parts_match(*h, hub, n_hub, why)) return Err::Parts;
    if (newer_fw(h->fw, hub_fw)) return Err::Newer;
    return Err::None;
}

// The password's key, refused when its key check is not the file's.
inline Err unlock(const Header& h, const char* pw, size_t pw_len, uint8_t key[32]) {
    uint8_t chk[16];
    if (!derive_key(pw, pw_len, h.salt, h.kdf_iter, key) || !key_check(key, chk)) return Err::Kdf;
    uint8_t diff = 0;
    for (int i = 0; i < 16; i++) diff |= chk[i] ^ h.key_check[i];
    if (diff) {
        memset(key, 0, 32);
        return Err::Password;
    }
    return Err::None;
}

// Decrypts an upload into `slot` from offset 4096, erasing ahead of the data (the first erase, with the first data,
// also wipes an earlier restore's stage header), and arms it only once the tag holds: the header's fields, then its
// magic.
class Stager {
public:
    Err begin(Flash& slot, const Header& h, const uint8_t* hdr, const uint8_t key[32]) {
        if (kStageDataOffset + static_cast<uint64_t>(total(h)) > slot.size()) return Err::Space;
        slot_ = &slot;
        h_ = h;
        w_ = kStageDataOffset;
        erased_ = 0;
        fed_ = 0;
        return gcm_.begin(key, h.nonce, hdr, kHeaderLen, false) && sha_.begin() ? Err::None : Err::Crypto;
    }

    Err feed(const uint8_t* ct, size_t n) {
        if (n > kPiece || fed_ + n > total(h_)) return Err::Length;
        size_t m = 0;
        if (!gcm_.update(ct, n, out_, sizeof out_, &m)) return Err::Crypto;
        fed_ += n;
        return put(out_, m);
    }

    Err finish(const uint8_t tag[kTagLen], uint32_t fc_target) {
        if (fed_ != total(h_)) return Err::Length;
        size_t m = 0;
        if (!gcm_.finish_decrypt(out_, sizeof out_, &m, tag)) return Err::Damaged;
        const Err e = put(out_, m);
        if (e != Err::None) return e;
        Stage s{};
        s.applied = 0xFFFFFFFF;
        s.fc_target = fc_target;
        s.n_parts = h_.n_parts;
        if (!sha_.finish(s.sha256)) return Err::Crypto;
        uint32_t off = kStageDataOffset;
        for (uint16_t i = 0; i < h_.n_parts; i++) {
            memcpy(s.parts[i].name, h_.parts[i].name, sizeof s.parts[i].name);
            s.parts[i].size = h_.parts[i].size;
            s.parts[i].offset = off;
            off += h_.parts[i].size;
        }
        memcpy(s.fw, h_.fw, sizeof s.fw);
        memcpy(s.host, h_.host, sizeof s.host);
        s.created = h_.created;
        uint8_t b[kStageLen];
        pack(s, b);
        if (!slot_->write(8, b + 8, kStageLen - 8) || !slot_->write(0, b, 8)) return Err::Flash;
        return Err::None;
    }

private:
    bool erase_to(uint32_t end) {
        while (erased_ < end) {
            const uint32_t n = slot_->size() - erased_ < kBlock ? slot_->size() - erased_ : kBlock;
            if (!slot_->erase(erased_, n)) return false;
            erased_ += n;
        }
        return true;
    }
    Err put(const uint8_t* p, size_t m) {
        if (!m) return Err::None;
        if (!erase_to(w_ + m) || !slot_->write(w_, p, m)) return Err::Flash;
        if (!sha_.update(p, m)) return Err::Crypto;
        w_ += m;
        return Err::None;
    }

    Flash*   slot_ = nullptr;
    Header   h_{};
    Gcm      gcm_;
    Sha256   sha_;
    uint32_t w_ = 0, erased_ = 0, fed_ = 0;
    uint8_t  out_[kPiece + 16];
};

enum class Applied { Nothing, Done, AlreadyDone, BadSha, Parts, Flash };

// At boot, before anything opens the storage: an armed stage in `slot` is written over this hub's partitions
// (`find` gives one by name). Every partition and the data's SHA-256 are checked before the first erase; a check
// that fails disarms the stage and keeps the storage. A flash error leaves it armed: the next boot starts over.
// Done and AlreadyDone fill `st` (its fc_target). `tmp` holds kPiece bytes.
inline Applied apply(Flash& slot, Flash* (*find)(const char* name, void* ctx), void* ctx, uint8_t* tmp, Stage* st) {
    uint8_t b[kStageLen];
    if (!slot.read(0, b, kStageLen) || !parse(b, st)) return Applied::Nothing;
    if (st->applied == 0) return Applied::AlreadyDone;
    Flash* dst[kMaxParts] = {};
    for (uint16_t i = 0; i < st->n_parts; i++) {
        dst[i] = find(st->parts[i].name, ctx);
        if (!dst[i] || !fits(st->parts[i].name, st->parts[i].size, dst[i]->size()) ||
            static_cast<uint64_t>(st->parts[i].offset) + st->parts[i].size > slot.size()) {
            slot.erase(0, kSector);
            return Applied::Parts;
        }
    }
    Sha256 sha;
    uint8_t d[32];
    bool ok = sha.begin();
    for (uint16_t i = 0; ok && i < st->n_parts; i++) {
        for (uint32_t off = 0; ok && off < st->parts[i].size; off += kPiece) {
            ok = slot.read(st->parts[i].offset + off, tmp, kPiece) && sha.update(tmp, kPiece);
        }
    }
    if (!ok || !sha.finish(d)) return Applied::Flash;
    if (memcmp(d, st->sha256, 32) != 0) {
        slot.erase(0, kSector);
        return Applied::BadSha;
    }
    for (uint16_t i = 0; i < st->n_parts; i++) {
        if (!dst[i]->erase(0, dst[i]->size())) return Applied::Flash;   // all of it: a larger one's rest is free
        for (uint32_t off = 0; off < st->parts[i].size; off += kPiece) {
            if (!slot.read(st->parts[i].offset + off, tmp, kPiece) || !dst[i]->write(off, tmp, kPiece)) return Applied::Flash;
        }
    }
    static const uint8_t kZero[4] = {0, 0, 0, 0};
    if (!slot.write(8, kZero, sizeof kZero)) return Applied::Flash;
    st->applied = 0;
    return Applied::Done;
}

}  // namespace backup
