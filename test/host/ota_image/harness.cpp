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
    // A pre-release tag vYYYYMMDDVV-rcN comes before its release (review LOW M3).
    CHECK(ota_image_check(P, "v2026102501", P, "v2026100301-rc1", false) == OtaImage::OLDER,
          "an older pre-release is refused");
    CHECK(ota_image_check(P, "v2026102501", P, "v2026100301-rc1", true) == OtaImage::OK, "  unless asked for");
    CHECK(ota_image_check(P, "v2026102501", P, "v2026102501-rc2", false) == OtaImage::OLDER,
          "the installed release's own pre-release is older");
    CHECK(ota_image_check(P, "v2026102501-rc2", P, "v2026102501", false) == OtaImage::OK, "  and its release newer");
    CHECK(ota_image_check(P, "v2026102501-rc2", P, "v2026102501-rc1", false) == OtaImage::OLDER, "rc1 is older than rc2");
    CHECK(ota_image_check(P, "v2026102501-rc1", P, "v2026102601-rc1", false) == OtaImage::OK, "a newer pre-release installs");
    CHECK(ota_image_check(P, "v2026102501", P, "v2026100301-rc1-4-gabc1234", false) == OtaImage::OK,
          "a developer build made on a pre-release tag is still not a release");
    CHECK(ota_release_order("v2026102501") > ota_release_order("v2026102501-rc99") &&
          ota_release_order("v2026102501-rc2") > ota_release_order("v2026102501-rc1") &&
          ota_release_order("v2026102501-rc1") > ota_release_order("v2026102401") &&
          ota_release_order("v2026102401") > 0 &&
          ota_release_order("v202610250") == 0 && ota_release_order("v20261025011") == 0 &&
          ota_release_order("v2026102501-rc") == 0 && ota_release_order("v2026102501-rc100") == 0 &&
          ota_release_order("v2026102501-rc1x") == 0 && ota_release_order(nullptr) == 0,
          "release order: v and ten digits, optionally -rc and one or two digits");
    printf("%s (%d failure(s))\n", s_fail ? "FAILED" : "ALL PASS", s_fail);
    return s_fail;
}
