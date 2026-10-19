// SPDX-License-Identifier: GPL-3.0-or-later
// WavPack images whose audio FLAC cannot hold (32-bit floating point), served as
// WavPack tracks made of the source's own blocks.
//
// WavPack blocks are self-contained, so a track copies the image's blocks that
// lie inside it unchanged except for their header (sample numbers relative to
// the track, the track's length) and their optional checksum, which covers the
// header. File-level sub-blocks (the original WAV header and trailer, the
// whole-file MD5) are dropped from copied blocks. Only the samples before the
// first whole block of a track (its first block also carries the stream
// configuration a decoder reads) and after its last whole block are encoded
// again, with libwavpack in the source's mode; their sizes are measured once
// and cached, so file sizes are exact before any read.
//
// Output layout:
//   [head: re-encoded blocks]  [copied blocks]  [tail: re-encoded blocks]  [APEv2 tag]
#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <optional>

#include "cover.hpp"
#include "tags.hpp"
#include "vfile.hpp"

namespace wm {

class Cache;

class WvImage {
public:
    /// True if `p` is WavPack with floating-point audio (from its first block).
    static bool is_float(const fs::path& p);
    /// Open (block index from the cache, or verified and indexed now).
    static std::shared_ptr<WvImage> open(const fs::path& p, const Cache* cache);

    struct Block {
        uint64_t off;    // in the file
        uint32_t size;   // whole block
        uint32_t kept;   // without the file-level sub-blocks
        uint64_t first;  // first sample
        uint32_t count;  // samples
    };
    fs::path path;
    /// the source version indexed (cache names derive from it, not from the file
    /// as it is when they are written)
    SrcKey key;
    uint32_t sample_rate = 0, channels = 0;
    uint64_t total = 0;
    uint64_t audio_end = 0;  // end of the last block (tags follow)
    /// the file's APEv2 tag, names as stored (Track/Disc/Year mapped, see apetag.hpp)
    Tags tags;
    std::optional<SrcRange> front_cover;
    /// audio blocks in file order (metadata-only blocks are not listed)
    std::vector<Block> blocks;

    /// Samples [s, e) interleaved, as libwavpack returns them (floats as their bits).
    std::vector<int32_t> decode(uint64_t s, uint64_t e) const;
    /// Samples [s, e) encoded as blocks numbered from `out_first`, in a stream of `out_total` samples.
    Bytes encode(uint64_t s, uint64_t e, uint64_t out_first, uint64_t out_total) const;

private:
    // encoder settings taken from the source
    int bits_ = 0, bytes_ = 0, norm_exp_ = 0, channel_mask_ = 0, qmode_ = 0, mode_ = 0;
    std::string chan_ids_;  // identities of the channels beyond the mask, if any are assigned
    void read_settings();
};

/// A source block made into a track block: numbered `index` in a stream of
/// `total` samples, file-level sub-blocks dropped if `strip`, checksum updated.
Bytes rebuild_block(std::span<const uint8_t> block, uint64_t index, uint64_t total, bool strip);

/// Sizes of a track's re-encoded head and tail.
struct WvLayout {
    uint32_t head = 0, tail = 0;
};
using Ranges = std::vector<std::pair<uint64_t, uint64_t>>;
/// Layouts of tracks [s, e) of `img` from the cache, if they have been measured.
std::optional<std::vector<WvLayout>> load_wv_layout(const WvImage& img, const Ranges& ranges, const Cache& cache);
/// Measure them now (re-encoding each track's head and tail, which can take a
/// while in the slower WavPack modes: a job for a background worker) and cache them.
std::vector<WvLayout> build_wv_layout(const WvImage& img, const Ranges& ranges, const Cache* cache);
/// From the cache, or measured (and cached) now.
std::vector<WvLayout> wv_layouts(const WvImage& img, const Ranges& ranges, const Cache* cache);
fs::path wv_layout_path(const WvImage& img, const Ranges& ranges, const Cache& cache);

class WvTrack : public VFile, public Trimmable, public std::enable_shared_from_this<WvTrack> {
public:
    /// Track covering samples [start, end) of `img`; `cover` replaces the image's front cover.
    WvTrack(std::shared_ptr<const WvImage> img, uint64_t start, uint64_t end, const Tags& tags, const WvLayout& layout, const Cover* cover);
    uint64_t size() const override { return size_; }
    Bytes read_at(uint64_t off, size_t len) const override;
    std::string describe() const override;
    /// Drop the blocks made and block positions computed for reads.
    void drop_cached() const override;

private:
    struct Part {  // re-encoded samples [s, e)
        uint64_t s, e, size;
    };
    using Shared = std::shared_ptr<const Bytes>;
    /// The re-encoded blocks of `p`, checked against their measured size.
    Bytes encode_part(const Part& p) const;
    /// Copied block i (of [b1, b2)) as served, kept briefly: readers often ask for
    /// a few KB at a time, and a block is ~100 KB.
    Shared block_bytes(size_t i, std::optional<File>& file) const;
    std::shared_ptr<const std::vector<uint64_t>> vstarts() const;
    void read_copy(uint64_t off, size_t len, Bytes& out) const;

    std::shared_ptr<const WvImage> img_;
    uint64_t start_, end_;
    std::optional<Part> head_, tail_;
    size_t b1_ = 0, b2_ = 0;  // copied blocks [b1, b2)
    uint64_t copy_size_ = 0;
    Segments tag_;  // pictures are not held
    uint64_t size_ = 0;
    mutable std::mutex mu_;
    LazyBytes head_bytes_, tail_bytes_;
    mutable std::map<size_t, std::pair<Shared, uint64_t>> blocks_;  // recently served copied blocks, last use
    mutable uint64_t clock_ = 0;
    /// start offset (relative to the copy region) of each copied block, + end
    mutable std::shared_ptr<const std::vector<uint64_t>> vstarts_;
};

/// A standalone WavPack file with a new APEv2 tag; its blocks are served unchanged.
std::shared_ptr<Spliced> retag_wavpack(const WvImage& img, const Tags& tags, const Cover* cover);

}  // namespace wm
