// SPDX-License-Identifier: GPL-3.0-or-later
// On-disk caches, keyed by source path + size + mtime so any change to the
// source invalidates them:
//   flacidx/<key>.idx      frame index of a FLAC image
//   flacidx/<key>.sacdidx  audio frame table of an SACD area
//   flacidx/<key>.avidx    packet index of a non-FLAC image (APE, WV, ...)
//   flacidx/<key>.encidx   frame sizes + MD5s of its tracks as encoded FLAC
//   md5/<key>.txt          per-track audio MD5s of a FLAC image
// Only positions and sizes: audio is always read from the source.
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
    /// True if this image's index is cached, so it opens in milliseconds.
    bool image_is_cached(const fs::path& src) const;

    Md5Map load_md5s(const fs::path& flac) const;
    void store_md5s(const fs::path& flac, const Md5Map& entries) const;

    fs::path sacd_frames_path(const fs::path& src, const SrcKey& key, bool multichannel) const;

private:
    fs::path idx_path(const fs::path& src, const SrcKey& key) const;
    fs::path md5_path(const fs::path& flac, const SrcKey& key) const;
    fs::path dir_;
};


}  // namespace wm
