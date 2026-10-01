// SPDX-License-Identifier: GPL-3.0-or-later
// A single track cut out of a FLAC image, presented as a standalone FLAC file.
//
// Output layout:
//   [fLaC + STREAMINFO + VORBIS_COMMENT + PICTURE...]
//   [head: VERBATIM frame for the partial source frame at the start]   (optional)
//   [copied source frames, headers rewritten to track-relative sample numbers]
//   [tail: VERBATIM frame for the partial source frame at the end]     (optional)
//
// Every size is computable from the frame index alone, so size() is exact
// before any audio is decoded.
#pragma once

#include <memory>
#include <mutex>
#include <optional>

#include "flac.hpp"
#include "tags.hpp"
#include "vfile.hpp"

namespace wm {

struct FlacImage {
    fs::path path;
    flac::FlacMeta meta;
    flac::FrameIndex index;
    /// sample-rate code and explicit sample-rate bytes of the source frames
    uint8_t sr_code = 0;
    Bytes sr_extra;

    static std::shared_ptr<FlacImage> open(fs::path p, flac::FlacMeta meta, flac::FrameIndex index);
    const flac::StreamInfo& si() const { return meta.streaminfo; }
    uint64_t bs() const { return index.block_size; }
    uint64_t total() const { return si().total_samples; }
    /// End sample (exclusive) of source frame k.
    uint64_t frame_end(uint64_t k) const { return std::min((k + 1) * bs(), total()); }
    /// Decode samples [s, e) per channel by decoding the covering source frames.
    std::vector<std::vector<int32_t>> decode_range(uint64_t s, uint64_t e) const;
};

/// MD5 of the decoded audio (FLAC STREAMINFO convention: interleaved,
/// little-endian, ceil(bps/8) bytes per sample) for each sample range, in one
/// decoding pass over the image.
std::vector<std::array<uint8_t, 16>> track_md5s(const FlacImage& img, const std::vector<std::pair<uint64_t, uint64_t>>& ranges);

class FlacTrack : public VFile {
public:
    /// Track covering samples [start, end) of `img`.
    FlacTrack(std::shared_ptr<const FlacImage> img, uint64_t start, uint64_t end, const Tags& tags, const std::vector<flac::MetaBlock>& pictures,
              std::optional<std::array<uint8_t, 16>> md5);
    uint64_t size() const override { return size_; }
    Bytes read_at(uint64_t off, size_t len) const override;
    std::string describe() const override;

private:
    struct VSeg {  // a partial frame re-emitted as VERBATIM
        uint64_t s, e;
        uint64_t out_sample;  // first sample number in the output stream
        uint64_t size;
    };
    Bytes verbatim(const VSeg& seg) const;
    const Bytes& seg_bytes(std::optional<Bytes>& cell, const VSeg& seg) const;
    const std::vector<uint64_t>& vstarts() const;
    void read_copy(uint64_t off, size_t len, Bytes& out) const;

    std::shared_ptr<const FlacImage> img_;
    uint64_t start_, end_;
    Bytes header_;
    std::optional<VSeg> head_, tail_;
    uint64_t k1_ = 0, k2_ = 0;  // copied source frames [k1, k2)
    uint64_t copy_size_ = 0;
    uint64_t size_ = 0;
    mutable std::mutex mu_;
    mutable std::optional<Bytes> head_bytes_, tail_bytes_;
    /// virtual start offset (relative to the copy region) of each copied frame, + end
    mutable std::optional<std::vector<uint64_t>> vstarts_;
};

}  // namespace wm
