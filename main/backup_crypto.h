// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Full backup crypto (research 2026-11-18 C1) on the PSA API: PBKDF2-HMAC-SHA256 for the key, a key check, AES-256-GCM
// streamed over one file, SHA-256 streamed over the staged data. Host check: test/host/backup/ (vectors from Python).
#pragma once

#include <cstddef>
#include <cstdint>

#include "psa/crypto.h"

namespace backup {

bool crypto_init();   // psa_crypto_init(); safe to call again

bool derive_key(const char* pw, size_t pw_len, const uint8_t salt[16], uint32_t iter, uint8_t key[32]);

// SHA-256("ZHAC backup key check" | key), first 16 bytes: a wrong password is told before anything is written.
bool key_check(const uint8_t key[32], uint8_t out[16]);

// One AES-256-GCM pass over a whole file, the header as associated data. `update` may hold back up to 15 bytes
// (out_cap must be n + 15); finish hands them out. Destroying (or begin() again) drops the key.
class Gcm {
public:
    Gcm() = default;
    Gcm(const Gcm&) = delete;
    Gcm& operator=(const Gcm&) = delete;
    ~Gcm() { end(); }
    bool begin(const uint8_t key[32], const uint8_t nonce[12], const uint8_t* aad, size_t aad_len, bool encrypt);
    bool update(const uint8_t* in, size_t n, uint8_t* out, size_t out_cap, size_t* out_len);
    bool finish_encrypt(uint8_t* out, size_t out_cap, size_t* out_len, uint8_t tag[16]);
    bool finish_decrypt(uint8_t* out, size_t out_cap, size_t* out_len, const uint8_t tag[16]);   // false: wrong tag

private:
    void end();
    psa_key_id_t key_ = 0;
    psa_aead_operation_t op_ = PSA_AEAD_OPERATION_INIT;
};

class Sha256 {
public:
    Sha256() = default;
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;
    ~Sha256() { psa_hash_abort(&op_); }
    bool begin();
    bool update(const uint8_t* in, size_t n);
    bool finish(uint8_t out[32]);

private:
    psa_hash_operation_t op_ = PSA_HASH_OPERATION_INIT;
};

}  // namespace backup
