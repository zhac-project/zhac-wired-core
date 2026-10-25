// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Which update images the hub takes (main/ota_image.h, review 2026-10-24 HS-10): only its own firmware, and an
// older release only when the owner asks for one. esp_https_ota checks the chip and nothing else, so the C6
// radio installer or a dual-chip build installed and took the hub off the network until a power cycle.
#include "ota_image.h"

#include <cstdio>

static int s_fail = 0;
#define CHECK(c, m) do { if (!(c)) { s_fail++; printf("  FAIL: %s\n", m); } else printf("  ok:   %s\n", m); } while (0)

int main() {
    const char* P = "zhac-wired-core";
    CHECK(ota_image_check(P, "v2026102501", P, "v2026102601", false) == OtaImage::OK, "a newer release installs");
    CHECK(ota_image_check(P, "v2026102501", P, "v2026102501", false) == OtaImage::OK, "the same release installs again");
    CHECK(ota_image_check(P, "v2026102501", "c6_rcp_installer", "v2026102601", false) == OtaImage::OTHER_PROJECT,
          "the C6 radio installer is refused");
    CHECK(ota_image_check(P, "v2026102501", "zhac-main-core", "v2026102601", true) == OtaImage::OTHER_PROJECT,
          "another project is refused, older allowed or not");
    CHECK(ota_image_check(P, "v2026102501", P, "v2026101401", false) == OtaImage::OLDER, "an older release is refused");
    CHECK(ota_image_check(P, "v2026102501", P, "v2026101401", true) == OtaImage::OK, "unless the owner asked for one");
    CHECK(ota_image_check(P, "v2026101601-rc1-20-g6d04f6a-dir", P, "v2026101401", false) == OtaImage::OK,
          "a developer build calls nothing older (the hub page offers it every version)");
    CHECK(ota_image_check(P, "v2026102501", P, "v2026101601-rc1-20-g6d04f6a", false) == OtaImage::OK,
          "and is never called older");
    CHECK(ota_image_check(P, "v2026102501", P, "v20261", false) == OtaImage::OK, "nor is a malformed version");
    CHECK(ota_release_number("v2026102501") == 2026102501ULL && ota_release_number("v202610250") == 0 &&
          ota_release_number("v20261025011") == 0 && ota_release_number("v2026102501-rc1") == 0 &&
          ota_release_number(nullptr) == 0, "release numbers: exactly v and ten digits");
    printf("%s (%d failure(s))\n", s_fail ? "FAILED" : "ALL PASS", s_fail);
    return s_fail;
}
