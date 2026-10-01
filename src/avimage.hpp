// SPDX-License-Identifier: GPL-3.0-or-later
// Non-FLAC lossless images (APE, WavPack, TTA, TAK, WAV, ALAC...) decoded on the
// fly with FFmpeg's libraries; nothing is converted or copied.
//
// The first open decodes the whole file once with checksum verification (so an
// incomplete or corrupt download never appears) and records a packet index:
// where each packet's samples start. Reads then decode only the packets that
// cover the requested samples. The index (a few KB) is cached on disk.
#pragma once

#include <array>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "tags.hpp"
#include "util.hpp"

namespace wm {

class Cache;

class AvImage : public std::enable_shared_from_this<AvImage> {
public:
    /// Open (index from the cache, or verified and indexed now).
    static std::shared_ptr<AvImage> open(const fs::path& p, const Cache* cache);
    ~AvImage();

    fs::path path;
    uint32_t sample_rate = 0, channels = 0, bps = 0;
    uint64_t total = 0;
    /// the container's own tags (APEv2 etc.), names as stored
    Tags tags;

    /// Decode samples [s, e) per channel.
    std::vector<std::vector<int32_t>> decode_range(uint64_t s, uint64_t e) const;
    /// Audio MD5 (FLAC convention) of each sample range, in one decoding pass.
    std::vector<std::array<uint8_t, 16>> md5s(const std::vector<std::pair<uint64_t, uint64_t>>& ranges) const;

    struct Packet {
        int64_t pts;     // demuxer timestamp, for seeking
        uint64_t first;  // first sample
        uint32_t count;  // samples
    };
    struct Decoder;

private:
    using Samples = std::shared_ptr<const std::vector<std::vector<int32_t>>>;
    /// Samples of packet k, decoding it (and queueing read-ahead) if needed.
    Samples packet_samples(size_t k) const;
    /// Cached or in-flight packet k; otherwise queued on the pool, or with
    /// `claimed` handed to the caller to decode. mu_ held.
    std::shared_future<Samples> start(size_t k, std::shared_ptr<std::promise<Samples>>* claimed) const;
    void fulfil(size_t k, std::promise<Samples>& promise) const;
    void prefetch_after(size_t k) const;
    Samples decode_packet(size_t k) const;
    std::unique_ptr<Decoder> take_decoder() const;
    void give_decoder(std::unique_ptr<Decoder> d) const;

    std::vector<Packet> packets_;
    mutable std::mutex mu_;
    mutable std::vector<std::unique_ptr<Decoder>> idle_;  // decoders not in use
    struct Cached {
        std::shared_future<Samples> samples;
        uint64_t used;
    };
    mutable std::map<size_t, Cached> cache_;  // decoded (or decoding) packets
    mutable uint64_t clock_ = 0;
    mutable size_t last_packet_ = SIZE_MAX;  // most recent packet a reader asked for (mu_)
};

}  // namespace wm
