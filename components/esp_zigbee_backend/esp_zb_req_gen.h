// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// esp_zb_req_gen -- match an SDK callback to the request still waiting for it.
//
// The interview and bind waits each share one result slot, and the SDK can
// deliver a reply after its waiter gave up (timeout, preemption): that reply
// then completed the NEXT request with the wrong data -- a Node Descriptor
// answer ending an Active Endpoint wait (review F05/F06).
//
//   req.user_ctx = gen.begin();      // with the stack lock held, after draining the slot
//   callback:  if (!gen.current(user_ctx)) return;   // before touching the slot
//   waiter gives up:  gen.expire();
//
// Callbacks run on the stack task with the stack lock held, so a reply that
// passed current() has posted before begin()'s caller drains the slot.
#pragma once

#include <atomic>
#include <cstdint>

class ReqGen {
public:
    void* begin() { return to_ctx(++gen_); }
    bool  current(const void* ctx) const { return ctx != nullptr && ctx == to_ctx(gen_.load()); }
    void  expire() { ++gen_; }

private:
    static void* to_ctx(uint32_t g) { return reinterpret_cast<void*>(static_cast<uintptr_t>(g)); }
    std::atomic<uint32_t> gen_{0};
};
