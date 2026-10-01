// SPDX-License-Identifier: GPL-3.0-or-later
// SACD ISO reader (Scarlet Book) and DSF track generator.
//
// Plain-DSD and DST-compressed areas are supported (DST is decoded by dst.cpp).
// The stereo area is used, or the multichannel area on request. A one-time
// sequential scan of the area records where every 1/75 s audio frame starts;
// DSF output is then produced on demand by de-interleaving the byte-interleaved
// channels into 4096-byte blocks and reversing bit order (SACD stores DSD
// MSB-first, DSF LSB-first). Audio bits are untouched.
#pragma once

#include <memory>
#include <mutex>
#include <optional>

#include "tags.hpp"
#include "vfile.hpp"

namespace wm {

class Cache;

/// bytes per channel per 1/75 s frame at 64 x 44.1 kHz
constexpr uint64_t SACD_FRAME_BYTES = 2'822'400 / 8 / 75;  // 4704

bool is_sacd(const fs::path& p);
/// True if the frame table of this ISO area is cached (opening is then cheap).
bool sacd_is_cached(const Cache& c, const fs::path& src, bool multichannel);

struct SacdTrack {
    uint64_t start = 0, end = 0;  // audio frames, area-relative
    std::optional<std::string> title, performer, songwriter, composer, arranger;
};

class SacdDisc {
public:
    /// Open the stereo area, or with `multichannel` the multichannel area.
    static std::shared_ptr<SacdDisc> open(const fs::path& p, const Cache* cache, bool multichannel);

    fs::path path;
    uint32_t channels = 0;
    std::vector<SacdTrack> tracks;
    Tags album;
    bool dst = false;

    Tags track_tags(size_t i) const;
    /// Per-channel DSD bytes for frames [f_first, f_last) (with a small cache):
    /// (first frame actually returned, bytes per channel).
    std::pair<uint64_t, std::shared_ptr<const std::vector<Bytes>>> channel_bytes(uint64_t f_first, uint64_t f_last) const;

private:
    std::vector<Bytes> coded_frames(uint64_t f0, uint64_t f1) const;
    std::vector<Bytes> read_frames(uint64_t f0, uint64_t f1) const;
    /// per frame: (sector, byte offset of the frame's first audio packet); ends with an end marker
    std::vector<std::pair<uint32_t, uint16_t>> frames_;
    mutable std::mutex mu_;
    mutable std::optional<std::pair<uint64_t, std::shared_ptr<const std::vector<Bytes>>>> recent_;
};

/// One SACD track as a DSF file.
class DsfTrack : public VFile {
public:
    DsfTrack(std::shared_ptr<const SacdDisc> disc, size_t track, const Tags& tags);
    uint64_t size() const override { return header_.size() + data_len_ + id3_.size(); }
    Bytes read_at(uint64_t off, size_t len) const override;
    std::string describe() const override;

private:
    void read_data(uint64_t a, uint64_t b, Bytes& out) const;
    std::shared_ptr<const SacdDisc> disc_;
    uint64_t f0_, bytes_per_ch_;
    Bytes header_;
    uint64_t data_len_;
    Bytes id3_;
};

}  // namespace wm
