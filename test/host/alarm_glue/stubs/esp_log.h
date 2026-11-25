// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Host stub: every line is kept in g_logs (harness.cpp), and printed too when g_log.
#pragma once
void host_log(const char* level, const char* tag, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
#define ESP_LOGE(tag, fmt, ...) host_log("E", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) host_log("W", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) host_log("I", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) do {} while (0)
