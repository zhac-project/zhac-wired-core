// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "backup_crypto.h"

#include <cstring>

namespace backup {

bool crypto_init() { return psa_crypto_init() == PSA_SUCCESS; }

bool derive_key(const char* pw, size_t pw_len, const uint8_t salt[16], uint32_t iter, uint8_t key[32]) {
    psa_key_derivation_operation_t op = PSA_KEY_DERIVATION_OPERATION_INIT;
    psa_status_t s = psa_key_derivation_setup(&op, PSA_ALG_PBKDF2_HMAC(PSA_ALG_SHA_256));
    if (s == PSA_SUCCESS) s = psa_key_derivation_input_integer(&op, PSA_KEY_DERIVATION_INPUT_COST, iter);
    if (s == PSA_SUCCESS) s = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_SALT, salt, 16);
    if (s == PSA_SUCCESS) s = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_PASSWORD,
                                                             reinterpret_cast<const uint8_t*>(pw), pw_len);
    if (s == PSA_SUCCESS) s = psa_key_derivation_output_bytes(&op, key, 32);
    psa_key_derivation_abort(&op);
    return s == PSA_SUCCESS;
}

bool key_check(const uint8_t key[32], uint8_t out[16]) {
    static constexpr char kLabel[] = "ZHAC backup key check";
    uint8_t buf[sizeof kLabel - 1 + 32], d[32];
    memcpy(buf, kLabel, sizeof kLabel - 1);
    memcpy(buf + sizeof kLabel - 1, key, 32);
    size_t n = 0;
    const bool ok = psa_hash_compute(PSA_ALG_SHA_256, buf, sizeof buf, d, sizeof d, &n) == PSA_SUCCESS;
    memcpy(out, d, 16);
    memset(buf, 0, sizeof buf);
    return ok;
}

void Gcm::end() {
    psa_aead_abort(&op_);
    op_ = PSA_AEAD_OPERATION_INIT;
    if (key_) psa_destroy_key(key_);
    key_ = 0;
}

bool Gcm::begin(const uint8_t key[32], const uint8_t nonce[12], const uint8_t* aad, size_t aad_len, bool encrypt) {
    end();
    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&a, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&a, 256);
    psa_set_key_usage_flags(&a, encrypt ? PSA_KEY_USAGE_ENCRYPT : PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&a, PSA_ALG_GCM);
    psa_status_t s = psa_import_key(&a, key, 32, &key_);
    if (s != PSA_SUCCESS) key_ = 0;
    if (s == PSA_SUCCESS) s = encrypt ? psa_aead_encrypt_setup(&op_, key_, PSA_ALG_GCM)
                                      : psa_aead_decrypt_setup(&op_, key_, PSA_ALG_GCM);
    if (s == PSA_SUCCESS) s = psa_aead_set_nonce(&op_, nonce, 12);
    if (s == PSA_SUCCESS) s = psa_aead_update_ad(&op_, aad, aad_len);
    if (s != PSA_SUCCESS) end();
    return s == PSA_SUCCESS;
}

bool Gcm::update(const uint8_t* in, size_t n, uint8_t* out, size_t out_cap, size_t* out_len) {
    *out_len = 0;
    return psa_aead_update(&op_, in, n, out, out_cap, out_len) == PSA_SUCCESS;
}

bool Gcm::finish_encrypt(uint8_t* out, size_t out_cap, size_t* out_len, uint8_t tag[16]) {
    size_t tl = 0;
    *out_len = 0;
    const bool ok = psa_aead_finish(&op_, out, out_cap, out_len, tag, 16, &tl) == PSA_SUCCESS && tl == 16;
    end();
    return ok;
}

bool Gcm::finish_decrypt(uint8_t* out, size_t out_cap, size_t* out_len, const uint8_t tag[16]) {
    *out_len = 0;
    const bool ok = psa_aead_verify(&op_, out, out_cap, out_len, tag, 16) == PSA_SUCCESS;
    end();
    return ok;
}

bool Sha256::begin() {
    psa_hash_abort(&op_);
    op_ = PSA_HASH_OPERATION_INIT;
    return psa_hash_setup(&op_, PSA_ALG_SHA_256) == PSA_SUCCESS;
}

bool Sha256::update(const uint8_t* in, size_t n) { return psa_hash_update(&op_, in, n) == PSA_SUCCESS; }

bool Sha256::finish(uint8_t out[32]) {
    size_t n = 0;
    return psa_hash_finish(&op_, out, 32, &n) == PSA_SUCCESS && n == 32;
}

}  // namespace backup
