// SPDX-License-Identifier: GPL-3.0-or-later
// Tracks of a decoded (non-FLAC) image, presented as compressed FLAC encoded on
// the fly. Nothing is stored but a layout: one pass over the image encodes every
// frame once to learn its size (and the track's audio MD5), so file sizes are
// exact before any read; a read then decodes and encodes only the frames it
// covers. Frames are encoded independently (flac::encode_frame), so re-encoding
// a frame reproduces it byte for byte.
#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>

#include "avimage.hpp"
#include "flac.hpp"
#include "futurelru.hpp"
#include "vfile.hpp"

namespace wm {

class Cache;

constexpr uint32_t ENCODED_BLOCK = 4096;

struct TrackLayout {
    std::vector<uint32_t> frame_sizes;
    std::array<uint8_t, 16> md5{};
    /// the cache file it is stored in (if any): dropped when frames stop matching it
    std::shared_ptr<const fs::path> file;
};

using Ranges = std::vector<std::pair<uint64_t, uint64_t>>;

/// MD5 of what this process's FLAC encoder makes of a fixed synthetic signal.
/// Frame sizes measured by an encoder that encodes differently (another libFLAC
/// version, or another CPU's SIMD code paths) are useless, so stored layouts
/// carry the fingerprint of the encoder that measured them.
const std::array<uint8_t, 16>& encoder_fingerprint();

fs::path layout_path(const AvImage& img, const Ranges& ranges, const Cache& cache);
/// The cached layout of these tracks, if present and measured by an encoder with
/// this process's fingerprint (layouts stored before fingerprints are trusted).
std::optional<std::vector<TrackLayout>> load_layout(const AvImage& img, const Ranges& ranges, const Cache& cache);
/// Encode every track once to measure it; stores the result in the cache.
std::vector<TrackLayout> build_layout(const AvImage& img, const Ranges& ranges, const Cache* cache);

class EncodedTrack : public VFile, public Trimmable, public std::enable_shared_from_this<EncodedTrack> {
public:
    /// `pictures`: PICTURE blocks for the header (a sidecar cover).
    EncodedTrack(std::shared_ptr<const AvImage> img, uint64_t start, uint64_t end, const Tags& tags, const TrackLayout& layout,
                 const std::vector<flac::HeaderBlock>& pictures = {});
    uint64_t size() const override { return size_; }
    Bytes read_at(uint64_t off, size_t len) const override;
    std::string describe() const override;
    /// Drop encoded frames (the track is not being read).
    void drop_cached() const override;

private:
    using Frame = std::shared_ptr<const Bytes>;
    using Frames = FutureLru<uint64_t, Frame>;
    using Promises = std::vector<std::pair<uint64_t, Frames::Promise>>;
    /// Claim the frames of [a, b) nobody is producing yet; the futures of all of
    /// them are appended to `need` if given (mu_ held).
    Promises claim(uint64_t a, uint64_t b, std::vector<Frames::Future>* need) const;
    /// Append bytes [a, a+len) of the audio (the frames after the header).
    void read_frames(uint64_t a, size_t len, Bytes& out) const;
    /// Decode and encode claimed frames, fulfilling their promises.
    void produce(const Promises& ps) const;
    /// Frame j encoded to `got` bytes, not the measured size: drop the layout, fail.
    [[noreturn]] void layout_mismatch(uint64_t j, size_t got) const;
    /// Claimed frames produced together, by whichever thread takes the job first.
    struct Job {
        explicit Job(Promises p) : ps(std::move(p)) {}
        Promises ps;
        std::atomic<bool> taken{false};
    };
    /// Produce the job's frames unless another thread has taken it.
    void run(Job& job) const {
        if (!job.taken.exchange(true)) produce(job.ps);
    }

    std::shared_ptr<const AvImage> img_;
    uint64_t start_, end_;
    Segments header_;  // padding is not held
    std::vector<uint64_t> offsets_;  // frame start offsets relative to the audio, + end
    std::shared_ptr<const fs::path> layout_file_;
    uint64_t size_;
    mutable std::mutex mu_;
    static constexpr size_t CACHE_FRAMES = 512;
    mutable Frames frames_{CACHE_FRAMES};  // encoded (or encoding) frames
    mutable uint64_t last_end_ = UINT64_MAX;  // frame after the previous read (mu_)
};

}  // namespace wm
