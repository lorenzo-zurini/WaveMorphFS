// SPDX-License-Identifier: GPL-3.0-or-later
// On-disk caches, keyed by source path + size + mtime so any change to the
// source invalidates them:
//   flacidx/<key>.idx      frame index of a FLAC image
//   flacidx/<key>.sacdidx  audio frame table of an SACD area
//   images/<key>.flac      lossless FLAC conversion of a non-FLAC image (APE, WV, ...)
//   images/<key>.json      the source image's tags (ffprobe)
//   md5/<key>.txt          per-track audio MD5s of an image
#pragma once

#include <array>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "flac.hpp"
#include "util.hpp"

namespace wm {

using Md5Map = std::map<std::pair<uint64_t, uint64_t>, std::array<uint8_t, 16>>;

class Cache {
public:
    explicit Cache(fs::path dir);
    const fs::path& dir() const { return dir_; }

    /// Cache file name stem for a source version (`extra` distinguishes variants).
    static std::string name_for(const fs::path& src, const SrcKey& key, std::string_view extra = "");

    std::optional<flac::FrameIndex> load_index(const fs::path& src, const SrcKey& key) const;
    void store_index(const fs::path& src, const SrcKey& key, const flac::FrameIndex& idx) const;
    /// Index for a FLAC file, from cache or built (and cached) now.
    flac::FrameIndex index_for(const fs::path& src, const flac::FlacMeta& meta) const;
    /// True if everything needed to present this image is cached (index, and for
    /// non-FLAC sources the verified conversion), so it opens in milliseconds.
    bool image_is_cached(const fs::path& src) const;

    Md5Map load_md5s(const fs::path& flac) const;
    void store_md5s(const fs::path& flac, const Md5Map& entries) const;

    fs::path converted_path(const fs::path& src, const SrcKey& key) const;
    /// Losslessly convert a non-FLAC image to FLAC in the cache, verifying the
    /// decoded PCM MD5 against the source. Returns the cached FLAC path.
    fs::path convert_image(const fs::path& src) const;
    /// Tags of the original (non-FLAC) image saved during conversion.
    std::vector<std::pair<std::string, std::string>> converted_tags(const fs::path& converted) const;

    fs::path sacd_frames_path(const fs::path& src, const SrcKey& key, bool multichannel) const;

private:
    fs::path idx_path(const fs::path& src, const SrcKey& key) const;
    fs::path md5_path(const fs::path& flac, const SrcKey& key) const;
    fs::path dir_;
};

/// ffprobe `-show_entries format_tags -of json` output -> (name, value), names as reported.
std::vector<std::pair<std::string, std::string>> parse_ffprobe_tags(std::string_view json);

}  // namespace wm
