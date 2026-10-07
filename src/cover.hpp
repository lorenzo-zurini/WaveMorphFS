// SPDX-License-Identifier: GPL-3.0-or-later
// Cover images from sidecar "_cover" entries, embedded into generated files by
// reference: the image bytes are read from the image file when a player reads
// that part of a track, never copied into memory per track.
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "vfile.hpp"

namespace wm {

/// Largest image accepted as a cover.
constexpr uint64_t MAX_COVER_BYTES = 2 << 20;

struct Cover {
    std::shared_ptr<const fs::path> path;
    uint64_t size = 0;
    int64_t mtime = 0;
    std::string mime;  // image/jpeg or image/png
    uint32_t width = 0, height = 0, depth = 0;
};

/// Probe an image (JPEG or PNG, at most MAX_COVER_BYTES); nullopt with a warning otherwise.
std::optional<Cover> load_cover(const fs::path& p);

/// Body of a FLAC PICTURE block (front cover) for `c`.
std::vector<Seg> flac_picture(const Cover& c);
/// Body of an ID3v2 APIC frame (front cover) for `c`.
std::vector<Seg> id3_apic(const Cover& c);

}  // namespace wm
