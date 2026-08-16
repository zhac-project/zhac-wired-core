// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Byte-exact tests for the ZCL frame builders.
//
// These frames are the highest-risk code in esp_zigbee_backend: a wrong
// reportable-change width or a misplaced manufacturer code shifts every
// following byte, and the failure mode on air is a device that silently
// ignores the record. Hardware cannot be assumed available; these can run
// anywhere.
//
// Expected bytes are derived from the ZCL spec (§2.4.9 write, §2.5.7 configure
// reporting) and cross-checked against the frames zhac-components' ZNP path
// builds for the same inputs -- the two radios must be byte-identical or the
// design doc's §6.6 parity claim is meaningless.
#include "../../esp_zb_zcl_frame.h"

#include <cstdio>
#include <cstring>
#include <vector>

static int g_fail = 0;

static void expect_bytes(const char* what, const uint8_t* got, size_t got_len,
                         const std::vector<uint8_t>& want) {
    bool ok = (got_len == want.size()) &&
              (std::memcmp(got, want.data(), want.size()) == 0);
    if (!ok) {
        g_fail++;
        std::printf("FAIL %s\n  want:", what);
        for (uint8_t b : want) std::printf(" %02x", b);
        std::printf("\n  got: ");
        for (size_t i = 0; i < got_len; i++) std::printf(" %02x", got[i]);
        std::printf("\n");
    } else {
        std::printf("ok   %s (%zu bytes)\n", what, got_len);
    }
}

static void expect_eq(const char* what, long got, long want) {
    if (got != want) {
        g_fail++;
        std::printf("FAIL %s: got %ld want %ld\n", what, got, want);
    } else {
        std::printf("ok   %s == %ld\n", what, want);
    }
}

int main() {
    using namespace zhc_zcl;
    uint8_t b[128];

    // ── change_width: the table that decides whether a trailing field exists
    expect_eq("width(bool 0x10) discrete",   change_width(0x10), 0);
    expect_eq("width(enum8 0x30) discrete",  change_width(0x30), 0);
    expect_eq("width(map8 0x18) discrete",   change_width(0x18), 0);
    expect_eq("width(u8 0x20)",              change_width(0x20), 1);
    expect_eq("width(u16 0x21)",             change_width(0x21), 2);
    expect_eq("width(s16 0x29)",             change_width(0x29), 2);
    expect_eq("width(u24 0x22)",             change_width(0x22), 3);
    expect_eq("width(u32 0x23)",             change_width(0x23), 4);
    expect_eq("width(float32 0x39)",         change_width(0x39), 4);
    expect_eq("width(string 0x42) unsupported", change_width(0x42), 0xFF);

    // ── Configure Reporting, discrete attribute (onOff 0x0000, bool).
    // No reportable-change field at all -- the record ends after max_interval.
    {
        size_t n = configure_report(b, sizeof(b), 0x11, 0x0000, 0x10,
                                    /*min=*/0, /*max=*/300, /*change=*/0,
                                    /*manu=*/0);
        expect_bytes("cfg_report onOff (discrete, no change field)", b, n,
                     {0x00, 0x11, 0x06,
                      0x00,               // direction
                      0x00, 0x00,         // attr 0x0000
                      0x10,               // bool
                      0x00, 0x00,         // min 0
                      0x2C, 0x01});       // max 300
    }

    // ── Configure Reporting, analog u16 (temperature 0x0000 on 0x0402),
    // reportable change 50 -> 2-byte trailing field.
    {
        size_t n = configure_report(b, sizeof(b), 0x12, 0x0000, 0x29,
                                    10, 600, 50, 0);
        expect_bytes("cfg_report temperature (s16, 2-byte change)", b, n,
                     {0x00, 0x12, 0x06,
                      0x00,
                      0x00, 0x00,
                      0x29,
                      0x0A, 0x00,         // min 10
                      0x58, 0x02,         // max 600
                      0x32, 0x00});       // change 50 LE
    }

    // ── Configure Reporting with a manufacturer code: the code goes BETWEEN
    // frame control and TSN, and FC gains bit 2.
    {
        size_t n = configure_report(b, sizeof(b), 0x13, 0x0055, 0x23,
                                    1, 60, 0x00000100, 0x115F);
        expect_bytes("cfg_report manu-specific (u32, 4-byte change)", b, n,
                     {0x04, 0x5F, 0x11, 0x13, 0x06,
                      0x00,
                      0x55, 0x00,
                      0x23,
                      0x01, 0x00,
                      0x3C, 0x00,
                      0x00, 0x01, 0x00, 0x00});
    }

    // Unsupported type must fail, not truncate.
    expect_eq("cfg_report rejects string type",
              (long)configure_report(b, sizeof(b), 1, 0, 0x42, 0, 1, 0, 0), 0);

    // ── Read Attributes: Basic manufacturerName + modelIdentifier.
    {
        const uint8_t attrs[] = {0x04, 0x00, 0x05, 0x00};
        size_t n = read_attributes(b, sizeof(b), 0x20, attrs, 2, 0);
        expect_bytes("read_attributes basic identity", b, n,
                     {0x00, 0x20, 0x00, 0x04, 0x00, 0x05, 0x00});
    }

    // ── Write Attributes, manufacturer-specific (the Aqara 0xFCC0 shape).
    {
        const uint8_t val[] = {0x01};
        size_t n = write_attribute(b, sizeof(b), 0x21, 0x0009, 0x20, val, 1,
                                   0x115F);
        expect_bytes("write_attribute lumi manu-specific", b, n,
                     {0x04, 0x5F, 0x11, 0x21, 0x02,
                      0x09, 0x00, 0x20, 0x01});
    }

    // ── Cluster-specific command, default-response disabled (flags bit0).
    {
        const uint8_t pl[] = {0xAA, 0xBB};
        size_t n = cluster_command(b, sizeof(b), 0x22, 0x0C, pl, 2, 0x01);
        expect_bytes("cluster_command with disable-default-response", b, n,
                     {0x11, 0x22, 0x0C, 0xAA, 0xBB});
    }
    {
        size_t n = cluster_command(b, sizeof(b), 0x23, 0x00, nullptr, 0, 0x00);
        expect_bytes("cluster_command no payload, default response on", b, n,
                     {0x01, 0x23, 0x00});
    }

    // ── Capacity is respected rather than overrun.
    expect_eq("configure_report refuses a short buffer",
              (long)configure_report(b, 4, 1, 0, 0x23, 0, 1, 0, 0), 0);
    expect_eq("read_attributes refuses a short buffer",
              (long)read_attributes(b, 4, 1, (const uint8_t*)"\x01\x00", 1, 0x115F), 0);

    if (g_fail) {
        std::printf("\n%d FAILURE(S)\n", g_fail);
        return 1;
    }
    std::printf("\nall ZCL frame layouts OK\n");
    return 0;
}
