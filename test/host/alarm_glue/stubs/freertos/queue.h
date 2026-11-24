// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Host stub: a real bounded queue.
#pragma once
#include <cstring>
#include <deque>
#include <vector>
#include "FreeRTOS.h"
struct HostQueue { size_t cap, item; std::deque<std::vector<char>> q; };
typedef HostQueue* QueueHandle_t;
inline QueueHandle_t xQueueCreate(UBaseType_t n, UBaseType_t item) { return new HostQueue{n, item, {}}; }
inline BaseType_t xQueueSend(QueueHandle_t h, const void* p, TickType_t) {
    if (h->q.size() >= h->cap) return pdFALSE;
    h->q.emplace_back(static_cast<const char*>(p), static_cast<const char*>(p) + h->item);
    return pdTRUE;
}
inline BaseType_t xQueueReceive(QueueHandle_t h, void* p, TickType_t) {
    if (h->q.empty()) return pdFALSE;
    std::memcpy(p, h->q.front().data(), h->item);
    h->q.pop_front();
    return pdTRUE;
}
