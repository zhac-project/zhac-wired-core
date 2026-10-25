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
// A release is "vYYYYMMDDVV". Any other version (git describe's
// "v2026101601-rc1-20-g6d04f6a" for a developer build) is never called older
// and calls nothing older, as the hub page's version list does
// (www-spa/src/releases.js parseTag). Pure: host-tested in test/host/ota_image.
#pragma once

#include <cstdint>
#include <cstring>

enum class OtaImage { OK, OTHER_PROJECT, OLDER };

// 2026102501 for "v2026102501", else 0.
inline uint64_t ota_release_number(const char* v) {
    if (!v || v[0] != 'v') return 0;
    uint64_t n = 0;
    for (int i = 1; i <= 10; i++) {
        if (v[i] < '0' || v[i] > '9') return 0;
        n = n * 10 + static_cast<uint64_t>(v[i] - '0');
    }
    return v[11] == '\0' ? n : 0;
}

inline OtaImage ota_image_check(const char* running_project, const char* running_version,
                                const char* image_project, const char* image_version, bool allow_older) {
    if (!running_project || !image_project || std::strcmp(running_project, image_project) != 0)
        return OtaImage::OTHER_PROJECT;
    const uint64_t running = ota_release_number(running_version);
    const uint64_t image = ota_release_number(image_version);
    if (!allow_older && running && image && image < running) return OtaImage::OLDER;
    return OtaImage::OK;
}
