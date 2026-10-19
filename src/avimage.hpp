// SPDX-License-Identifier: GPL-3.0-or-later
// Non-FLAC lossless images (APE, WavPack, TTA, TAK, WAV, ALAC...) decoded on the
// fly with FFmpeg's libraries; nothing is converted or copied.
//
// The first open decodes the whole file once with checksum verification (so an
// incomplete or corrupt file never appears) and records a packet index:
// where each packet's samples start. Reads then decode only the packets that
// cover the requested samples. The index (20 bytes per packet: a few KB for APE,
// under 1 MB per hour for WAV/AIFF's 4096-sample packets) is cached on disk.
#pragma once

#include <array>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "futurelru.hpp"
#include "tags.hpp"
#include "util.hpp"
#include "vfile.hpp"

namespace wm {

class Cache;

class AvImage : public Trimmable, public std::enable_shared_from_this<AvImage> {
public:
    /// Open (index from the cache, or verified and indexed now).
    static std::shared_ptr<AvImage> open(const fs::path& p, const Cache* cache);
    ~AvImage();

    fs::path path;
    /// the file's identity when it was opened: what is cached from this image
    /// (index, track layouts) is filed under it, not under whatever is there later
    SrcKey key;
    uint32_t sample_rate = 0, channels = 0, bps = 0;
    uint64_t total = 0;
    /// the container's own tags (APEv2 etc.), names as stored
    Tags tags;
    /// the file's own front cover (an APEv2 "Cover Art (Front)" image), as the
    /// range of the file holding the image, and its type (image/jpeg, image/png);
    /// see flac::picture_body
    std::optional<SrcRange> front_cover;
    std::string front_cover_mime;

    /// Decode samples [s, e) per channel.
    std::vector<std::vector<int32_t>> decode_range(uint64_t s, uint64_t e) const;
    /// Drop decoded packets and idle decoders (the image is not being read).
    void drop_cached() const override;

    struct Packet {
        int64_t pts;     // demuxer timestamp, for seeking
        uint64_t first;  // first sample
        uint32_t count;  // samples
    };
    struct Decoder;

private:
    /// decode this many samples beyond the one being read, in parallel (~30 s at 44.1 kHz)
    static constexpr uint64_t READ_AHEAD_SAMPLES = 1'500'000;
    /// decoded samples (per channel) kept per image while it is read: the read-ahead
    /// plus as much again behind it (APE packets are up to 294912 samples each)
    static constexpr uint64_t CACHE_SAMPLES = 2 * READ_AHEAD_SAMPLES;

    using Samples = std::shared_ptr<const std::vector<std::vector<int32_t>>>;
    using Packets = FutureLru<size_t, Samples>;
    void find_front_cover();
    /// Samples of packet k, decoding it (and queueing read-ahead) if needed.
    Samples packet_samples(size_t k) const;
    /// Cached or in-flight packet k; otherwise queued on the pool, or with
    /// `claimed` handed to the caller to decode. mu_ held.
    Packets::Future start(size_t k, Packets::Promise* claimed) const;
    void fulfil(size_t k, std::promise<Samples>& promise) const;
    void prefetch_after(size_t k) const;
    Samples decode_packet(size_t k) const;
    /// Packet k is not what the index says: drop the stored index, fail.
    [[noreturn]] void index_mismatch(size_t k, const std::string& what) const;
    std::unique_ptr<Decoder> take_decoder() const;
    void give_decoder(std::unique_ptr<Decoder> d) const;

    std::vector<Packet> packets_;
    fs::path index_file_;  // where the index is cached ("" if it is not)
    mutable std::mutex mu_;
    mutable std::vector<std::unique_ptr<Decoder>> idle_;  // decoders not in use
    mutable Packets cache_{CACHE_SAMPLES};  // decoded (or decoding) packets, cost = samples
    mutable size_t last_packet_ = SIZE_MAX;  // most recent packet a reader asked for (mu_)
};

}  // namespace wm
