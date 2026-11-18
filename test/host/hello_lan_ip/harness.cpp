// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Fakes for remote_hello_extra(): Ethernet and Wi-Fi status, the switch and the log level. See run.sh.
#include <cstdio>
#include <cstring>

#include "eth.h"
#ifdef CONFIG_ZHAC_WIFI_HOSTED
#include "wifi_hosted.h"
#endif

static char g_eth_ip[16], g_wifi_ip[16];
void eth_get_status(NetStatus* out) {
    *out = NetStatus{};
    std::snprintf(out->ip, sizeof(out->ip), "%s", g_eth_ip);
}
#ifdef CONFIG_ZHAC_WIFI_HOSTED
void wifi_hosted_status(WifiStatus* out) {
    *out = WifiStatus{};
    out->connected = g_wifi_ip[0] != '\0';
    std::snprintf(out->ip, sizeof(out->ip), "%s", g_wifi_ip);
}
#endif
static constexpr char kRemoteCapsJson[] = "[\"rules.v1\"]";
static bool        sys_remote_scripts() { return false; }
static const char* log_level_now() { return "off"; }

#include "slice.inc"

static int g_fail = 0;
static void expect(const char* what, bool ok) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) g_fail++;
}
static bool hello_has(const char* eth, const char* wifi, const char* want) {
    std::snprintf(g_eth_ip, sizeof(g_eth_ip), "%s", eth);
    std::snprintf(g_wifi_ip, sizeof(g_wifi_ip), "%s", wifi);
    char out[160];
    const size_t n = remote_hello_extra(out, sizeof(out));
    if (n == 0) return false;
    const bool has_ip = std::strstr(out, "\"lan_ip\"") != nullptr;
    return want ? std::strstr(out, want) != nullptr : !has_ip;
}

int main() {
#ifdef CONFIG_ZHAC_WIFI_HOSTED
    std::printf("-- Wi-Fi build\n");
    expect("cable and Wi-Fi: the cable's address",
           hello_has("192.168.1.20", "192.168.1.31", "\"lan_ip\":\"192.168.1.20\""));
    expect("Wi-Fi only: the Wi-Fi address", hello_has("", "192.168.1.31", "\"lan_ip\":\"192.168.1.31\""));
    expect("neither: no address", hello_has("", "", nullptr));
#else
    std::printf("-- wired build\n");
    expect("cable: its address", hello_has("10.0.0.7", "", "\"lan_ip\":\"10.0.0.7\""));
    expect("no cable: no address", hello_has("", "", nullptr));
#endif
    std::printf("%s (%d failed)\n", g_fail ? "FAILED" : "passed", g_fail);
    return g_fail;
}
