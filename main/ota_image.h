// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Whether a downloaded image may replace the running firmware (review
// 2026-10-24 HS-10). esp_https_ota checks only that an image is valid for this
// chip, so the C6 radio installer, a dual-chip build or any older release
// installed; a wrong project took the hub off the network until a power cycle
// rolled it back. This checks the image's app description: the same project,
// and no older release unless the owner asked for one (an older release need
// not read what this one stored: below the 1 MB device store it shows no
// devices until it is updated again).
//
// A release is "vYYYYMMDDVV", and "vYYYYMMDDVV-rcN" (N one or two digits) a
// pre-release that comes before it (review LOW M3: an older -rcN was offered
// and installed as if it were no release). Any other version (git describe's
// "v2026101601-rc1-20-g6d04f6a" for a developer build) is never called older
// and calls nothing older, as the hub page's version list does
// (www-spa/src/releases.js tagOrder). Pure: host-tested in test/host/ota_image.
#pragma once

#include <cstdint>
#include <cstring>

enum class OtaImage { OK, OTHER_PROJECT, OLDER };

// A key that orders releases and pre-releases: "v2026102501" -> 2026102501999,
// "v2026102501-rc2" -> 2026102501002; 0 for anything else.
inline uint64_t ota_release_order(const char* v) {
    if (!v || v[0] != 'v') return 0;
    uint64_t n = 0;
    for (int i = 1; i <= 10; i++) {
        if (v[i] < '0' || v[i] > '9') return 0;
        n = n * 10 + static_cast<uint64_t>(v[i] - '0');
    }
    if (v[11] == '\0') return n * 1000 + 999;
    if (std::strncmp(v + 11, "-rc", 3) != 0) return 0;
    const char* d = v + 14;
    if (d[0] < '0' || d[0] > '9') return 0;
    uint64_t rc = static_cast<uint64_t>(d[0] - '0');
    if (d[1] >= '0' && d[1] <= '9') { rc = rc * 10 + static_cast<uint64_t>(d[1] - '0'); d++; }
    return d[1] == '\0' ? n * 1000 + rc : 0;
}

inline OtaImage ota_image_check(const char* running_project, const char* running_version,
                                const char* image_project, const char* image_version, bool allow_older) {
    if (!running_project || !image_project || std::strcmp(running_project, image_project) != 0)
        return OtaImage::OTHER_PROJECT;
    const uint64_t running = ota_release_order(running_version);
    const uint64_t image = ota_release_order(image_version);
    if (!allow_older && running && image && image < running) return OtaImage::OLDER;
    return OtaImage::OK;
}
