// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Fake heaps for main.cpp's log_heap_info(), with IDF's cap bits and matching (a heap matches when it has every
// asked-for cap). See run.sh.
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>

#define MALLOC_CAP_32BIT    (1u << 1)
#define MALLOC_CAP_8BIT     (1u << 2)
#define MALLOC_CAP_DMA      (1u << 3)
#define MALLOC_CAP_SPIRAM   (1u << 10)
#define MALLOC_CAP_INTERNAL (1u << 11)
#define MALLOC_CAP_DEFAULT  (1u << 12)
#define MALLOC_CAP_TCM      (1u << 23)

struct Heap { uint32_t caps; size_t free, largest; };
static const uint32_t kCommon = MALLOC_CAP_DEFAULT | MALLOC_CAP_32BIT | MALLOC_CAP_8BIT;
static const Heap kHeaps[] = {
    {kCommon | MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA, 90000, 20000},            // L2MEM, fragmented
    {MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_32BIT,  // the PSRAM DMA reserve
     32768, 32700},
    {MALLOC_CAP_TCM | kCommon | MALLOC_CAP_INTERNAL, 8192, 8000},              // TCM
    {MALLOC_CAP_SPIRAM | kCommon, 30000000, 4000000},                         // PSRAM
};
static size_t heap_caps_get_free_size(uint32_t caps) {
    size_t n = 0;
    for (const Heap& h : kHeaps) if ((h.caps & caps) == caps) n += h.free;
    return n;
}
static size_t heap_caps_get_largest_free_block(uint32_t caps) {
    size_t n = 0;
    for (const Heap& h : kHeaps) if ((h.caps & caps) == caps && h.largest > n) n = h.largest;
    return n;
}

static std::string g_log;
__attribute__((format(printf, 2, 3))) static void log_line(const char*, const char* fmt, ...) {
    char b[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    g_log += b;
    g_log += '\n';
}
#define ESP_LOGI(tag, ...) log_line(tag, __VA_ARGS__)
static const char* TAG = "main";

#include "slice.inc"

int main() {
    log_heap_info();
    std::fputs(g_log.c_str(), stdout);
    const bool ok = g_log.find("main-largest=20000 ") != std::string::npos ||
                    g_log.find("main-largest=20000\n") != std::string::npos;
    std::printf("%s main-largest is the main heap's (20000), not the reserve pool's (32700)\n", ok ? "ok  " : "FAIL");
    return ok ? 0 : 1;
}
