// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Host stand-ins for what the rule ops in main/ws_bridge.cpp touch outside themselves: the heap caps
// allocator, the WebSocket reply sink and pushes, rule_store, simple_rules and PSA's one-shot hash. Everything
// else (JsonWriter, kRemotePageBudget, utf8_seq.h, ArduinoJson, event_bus.h, RuleSlot and the rule ops
// themselves) is the real code.
#pragma once
#include <algorithm>
#include <cinttypes>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>
#include <ArduinoJson.h>

// RuleSlot and ZAP_MAX_RULES, cut out of zhac-components zap_common.h by run.sh (rule_slot.inc)
#include "rule_slot.inc"
static constexpr uint16_t ZAP_MAX_RULES = 256;

// heap_caps: malloc/free, with the Nth call (1-based) made to fail when g_alloc_fail_at > 0
#define MALLOC_CAP_SPIRAM 1
void* heap_caps_malloc(size_t size, uint32_t caps);
void  heap_caps_free(void* p);

// the reply sink: every frame the code under test sends
void ws_server_reply(int fd, const char* buf, size_t len);

// the pushes (rule.added / rule.updated / rule.deleted), captured by slice_tu.cpp
struct Pushed { std::string event, payload; };
extern std::vector<Pushed> g_events;

// rule_store (harness.cpp keeps the fake store)
uint16_t rule_store_load_all(RuleSlot* out, uint16_t max_count);
bool     rule_store_load(uint16_t rule_id, RuleSlot* out);

// simple_rules (harness.cpp: scripted results, and a successful update rewrites the stored slot)
bool        simple_rules_add(const char* name, const char* dsl, uint16_t* out_rule_id);
bool        simple_rules_update(uint16_t rule_id, const char* name, const char* dsl);
bool        simple_rules_check(const char* dsl, char* err, size_t cap);
const char* dsl_last_error();

// PSA one-shot SHA-256 (OpenSSL underneath)
typedef int32_t psa_status_t;
constexpr psa_status_t PSA_SUCCESS = 0;
#define PSA_ALG_SHA_256 ((uint32_t)0x02000009)
psa_status_t psa_hash_compute(uint32_t alg, const uint8_t* input, size_t input_length, uint8_t* hash,
                              size_t hash_size, size_t* hash_length);
