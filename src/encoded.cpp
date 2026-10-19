// SPDX-License-Identifier: GPL-3.0-or-later
#include "encoded.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <tuple>

#include "cache.hpp"
#include "md5.hpp"

namespace wm {

fs::path layout_path(const AvImage& img, const Ranges& ranges, const Cache& cache) {
    std::string extra = "flac-encoded";
    for (auto& [s, e] : ranges) extra += std::format(" {}-{}", s, e);
    return cache.dir() / "flacidx" / (Cache::name_for(img.path, img.key, extra) + ".encidx");
}

namespace {

/// 001: no encoder fingerprint (still read: production layouts were measured by
/// the encoder that serves them, and re-measuring every image takes hours);
/// 002: the fingerprint of the encoder that measured follows the magic
const char LAYOUT_MAGIC_V1[8] = {'W', 'M', 'E', 'N', 'C', '0', '0', '1'};
const char LAYOUT_MAGIC[8] = {'W', 'M', 'E', 'N', 'C', '0', '0', '2'};
/// frames encoded per job, frames encoded ahead of the reader (frames kept per
/// track: EncodedTrack::CACHE_FRAMES)
constexpr uint64_t JOB_FRAMES = 32;
constexpr uint64_t AHEAD_FRAMES = 256;  // ~24 s at 44.1 kHz
/// frames decoded per batch while measuring
constexpr uint64_t BATCH = 64;

uint64_t frames_of(uint64_t samples) { return (samples + ENCODED_BLOCK - 1) / ENCODED_BLOCK; }

std::vector<std::span<const int32_t>> slice(const std::vector<std::vector<int32_t>>& pcm, size_t from, size_t n) {
    std::vector<std::span<const int32_t>> out;
    for (auto& c : pcm) out.emplace_back(c.data() + from, n);
    return out;
}

}  // namespace

const std::array<uint8_t, 16>& encoder_fingerprint() {
    static const auto fp = [] {
        // a few frames per common shape, from a signal that exercises LPC and
        // stereo decisions (two triangle waves and noise, made with integers only
        // so that the signal itself is the same everywhere)
        Md5 md5;
        for (auto [ch, bps, rate] : {std::tuple{2u, 16u, 44100u}, {2u, 24u, 96000u}, {1u, 24u, 48000u}, {6u, 16u, 48000u}, {2u, 8u, 22050u}}) {
            size_t n = 3 * ENCODED_BLOCK + 123;
            std::vector<std::vector<int32_t>> pcm(ch, std::vector<int32_t>(n));
            int64_t amp = (int64_t(1) << (bps - 1)) / 4;
            auto triangle = [](int64_t i, int64_t period, int64_t a) { return std::abs((i % period) * 4 * a / period - 2 * a) - a; };
            uint32_t seed = 1;
            for (size_t c = 0; c < ch; c++)
                for (size_t i = 0; i < n; i++) {
                    seed = seed * 1664525u + 1013904223u;
                    int64_t noise = int64_t(seed >> 16) * amp / 4 / 65536;
                    pcm[c][i] = int32_t(triangle(int64_t(i), 100 + 37 * int64_t(c), amp) + triangle(int64_t(i), 451, amp) / 2 + noise);
                }
            for (auto& fr : flac::encode_frames(slice(pcm, 0, n), bps, rate, ENCODED_BLOCK, 0)) md5.update(fr);
        }
        return md5.finish();
    }();
    return fp;
}

std::optional<std::vector<TrackLayout>> load_layout(const AvImage& img, const Ranges& ranges, const Cache& cache) {
    auto path = std::make_shared<const fs::path>(layout_path(img, ranges, cache));
    auto b = try_read_file(*path);
    if (!b || b->size() < 8) return std::nullopt;
    size_t p = 0;
    if (std::memcmp(b->data(), LAYOUT_MAGIC, 8) == 0) {
        // measured by an encoder that encodes differently: measure again
        if (b->size() < 24 || !std::equal(encoder_fingerprint().begin(), encoder_fingerprint().end(), b->begin() + 8)) return std::nullopt;
        p = 24;
    } else if (std::memcmp(b->data(), LAYOUT_MAGIC_V1, 8) == 0) {
        p = 8;
    } else {
        return std::nullopt;
    }
    if (p + 8 > b->size() || le64(&(*b)[p]) != ranges.size()) return std::nullopt;
    p += 8;
    std::vector<TrackLayout> out(ranges.size());
    for (size_t t = 0; t < ranges.size(); t++) {
        out[t].file = path;
        if (p + 20 > b->size()) return std::nullopt;
        std::memcpy(out[t].md5.data(), &(*b)[p], 16);
        uint32_t n = le32(&(*b)[p + 16]);
        p += 20;
        if (n != frames_of(ranges[t].second - ranges[t].first) || p + 4ull * n > b->size()) return std::nullopt;
        out[t].frame_sizes.resize(n);
        for (uint32_t j = 0; j < n; j++) out[t].frame_sizes[j] = le32(&(*b)[p + 4 * j]);
        p += 4ull * n;
    }
    if (p != b->size()) return std::nullopt;
    return out;
}

std::vector<TrackLayout> build_layout(const AvImage& img, const Ranges& ranges, const Cache* cache) {
    std::vector<TrackLayout> out;
    for (auto [s, e] : ranges) {
        TrackLayout tl;
        Md5 md5;
        uint64_t nf = frames_of(e - s);
        for (uint64_t j0 = 0; j0 < nf; j0 += BATCH) {
            uint64_t j1 = std::min(nf, j0 + BATCH);
            uint64_t a = s + j0 * ENCODED_BLOCK, b = std::min(e, s + j1 * ENCODED_BLOCK);
            auto pcm = img.decode_range(a, b);
            auto chans = slice(pcm, 0, size_t(b - a));
            for (auto& fr : flac::encode_frames(chans, img.bps, img.sample_rate, ENCODED_BLOCK, j0)) tl.frame_sizes.push_back(uint32_t(fr.size()));
            md5_pcm(md5, chans, 0, size_t(b - a), img.bps);
        }
        tl.md5 = md5.finish();
        out.push_back(std::move(tl));
    }
    if (cache) {
        auto path = std::make_shared<const fs::path>(layout_path(img, ranges, *cache));
        Bytes b(LAYOUT_MAGIC, LAYOUT_MAGIC + 8);
        append(b, encoder_fingerprint());
        put_le64(b, out.size());
        for (auto& tl : out) {
            append(b, tl.md5);
            put_le32(b, uint32_t(tl.frame_sizes.size()));
            for (auto sz : tl.frame_sizes) put_le32(b, sz);
            tl.file = path;
        }
        atomic_write(*path, b);
    }
    return out;
}

EncodedTrack::EncodedTrack(std::shared_ptr<const AvImage> img, uint64_t start, uint64_t end, const Tags& tags, const TrackLayout& layout,
                           const std::vector<flac::HeaderBlock>& pictures)
    : img_(std::move(img)), start_(start), end_(end), layout_file_(layout.file) {
    WM_ENSURE(start < end && end <= img_->total, "bad track range {}..{}", start, end);
    WM_ENSURE(layout.frame_sizes.size() == frames_of(end - start), "layout does not match the track");
    offsets_.reserve(layout.frame_sizes.size() + 1);
    uint64_t pos = 0;
    for (auto sz : layout.frame_sizes) {
        offsets_.push_back(pos);
        pos += sz;
    }
    offsets_.push_back(pos);
    flac::StreamInfo si;
    uint64_t len = end - start;
    si.min_block = si.max_block = uint16_t(len < ENCODED_BLOCK ? len : ENCODED_BLOCK);
    si.sample_rate = img_->sample_rate;
    si.channels = img_->channels;
    si.bps = img_->bps;
    si.total_samples = len;
    si.md5 = layout.md5;
    auto [mn, mx] = std::minmax_element(layout.frame_sizes.begin(), layout.frame_sizes.end());
    std::vector<flac::HeaderBlock> meta = {{flac::BLOCK_VORBIS, flac::build_vorbis("WaveMorphFS", vorbis_comments(tags))}};
    meta.insert(meta.end(), pictures.begin(), pictures.end());
    meta.push_back({flac::BLOCK_PADDING, Zeros{flac::EDIT_PADDING}});
    header_ = Segments(flac::header_segments(si.encode(*mn, *mx), meta));
    size_ = header_.size() + pos;
}

void EncodedTrack::drop_cached() const {
    std::lock_guard g(mu_);
    frames_.drop_ready();
    last_end_ = UINT64_MAX;
}

EncodedTrack::Promises EncodedTrack::claim(uint64_t a, uint64_t b, std::vector<Frames::Future>* need) const {
    Promises ps;
    for (uint64_t j = a; j < b; j++) {
        Frames::Promise p;
        auto f = frames_.get(j, 1, &p);
        if (need) need->push_back(std::move(f));
        if (p) ps.emplace_back(j, std::move(p));
    }
    // the frames just claimed or touched are about to be used: evict others
    frames_.trim([&](uint64_t j) { return j >= a && j < b; });
    return ps;
}

void EncodedTrack::layout_mismatch(uint64_t j, size_t got) const {
    // The sizes were measured by an encoder that encodes this frame differently
    // (fingerprints only sample the encoder). Every read of this frame would fail
    // the same way, so the stored layout goes and the tracks are measured again.
    std::string what = std::format("{} frame {} encoded to {} bytes, but its measured layout says {}", describe(), j, got, offsets_[j + 1] - offsets_[j]);
    if (!layout_file_) fail("{}", what);
    std::error_code ec;
    fs::remove(*layout_file_, ec);
    fail("{}: removed the layout {}; the tracks are measured again when their folder is listed anew (after a restart at the latest)", what,
         layout_file_->string());
}

void EncodedTrack::produce(const Promises& ps) const {
    if (ps.empty()) return;
    uint64_t fa = ps.front().first, fb = ps.back().first + 1;
    try {
        uint64_t sa = start_ + fa * ENCODED_BLOCK, sb = std::min(end_, start_ + fb * ENCODED_BLOCK);
        auto pcm = img_->decode_range(sa, sb);
        // each run of consecutive claimed frames is encoded by one encoder
        for (size_t r = 0, r2; r < ps.size(); r = r2) {
            for (r2 = r + 1; r2 < ps.size() && ps[r2].first == ps[r2 - 1].first + 1;) r2++;
            size_t from = size_t((ps[r].first - fa) * ENCODED_BLOCK);
            size_t n = size_t(std::min<uint64_t>((r2 - r) * ENCODED_BLOCK, sb - sa - from));
            auto frames = flac::encode_frames(slice(pcm, from, n), img_->bps, img_->sample_rate, ENCODED_BLOCK, ps[r].first);
            for (size_t i = r; i < r2; i++) {
                auto& [j, p] = ps[i];
                auto fr = std::make_shared<const Bytes>(std::move(frames[i - r]));
                if (fr->size() != offsets_[j + 1] - offsets_[j]) layout_mismatch(j, fr->size());
                p->set_value(std::move(fr));
            }
        }
    } catch (...) {
        std::lock_guard g(mu_);
        for (auto& [j, p] : ps) {
            try {
                p->set_exception(std::current_exception());
                frames_.forget(j, p.get());  // let a later read retry
            } catch (const std::future_error&) {  // already fulfilled
            }
        }
    }
}

Bytes EncodedTrack::read_at(uint64_t off, size_t len) const {
    Bytes out;
    if (off >= size_) return out;
    len = size_t(std::min<uint64_t>(len, size_ - off));
    out.reserve(len);
    stitch(out, off, len, Piece{header_.size(), [&](uint64_t a, size_t n, Bytes& o) { header_.read(img_->path, a, n, o); }},
           Piece{offsets_.back(), [&](uint64_t a, size_t n, Bytes& o) { read_frames(a, n, o); }});
    WM_ENSURE(out.size() == len, "internal: produced {} of {} bytes at {}", out.size(), len, off);
    return out;
}

void EncodedTrack::read_frames(uint64_t a, size_t len, Bytes& out) const {
    uint64_t b = a + len;
    uint64_t nframes = offsets_.size() - 1;
    uint64_t j0 = uint64_t(std::upper_bound(offsets_.begin(), offsets_.end(), a) - offsets_.begin()) - 1;
    uint64_t j1 = uint64_t(std::lower_bound(offsets_.begin(), offsets_.end(), b) - offsets_.begin());  // exclusive
    // claim what is needed, then read-ahead; background work needs a shared owner
    // (without one, in tests, everything is done here)
    auto self = weak_from_this().lock();
    touch(self);
    std::vector<std::shared_ptr<Job>> jobs, ahead;
    std::vector<Frames::Future> need;  // held: later claims (read-ahead) may evict finished frames
    {
        std::lock_guard g(mu_);
        for (uint64_t s = j0; s < j1; s += JOB_FRAMES)
            if (auto ps = claim(s, std::min(j1, s + JOB_FRAMES), &need); !ps.empty()) jobs.push_back(std::make_shared<Job>(std::move(ps)));
        // read-ahead only for a reader moving forward
        bool sequential = last_end_ != UINT64_MAX && j0 + 1 >= last_end_ && j0 <= last_end_;
        last_end_ = j1;
        if (self && sequential)
            for (uint64_t s = j1; s < std::min(nframes, j1 + AHEAD_FRAMES); s += JOB_FRAMES)
                if (auto ps = claim(s, std::min(nframes, s + JOB_FRAMES), nullptr); !ps.empty()) ahead.push_back(std::make_shared<Job>(std::move(ps)));
    }
    // The frames this read needs are produced here, in order. Idle pool threads may
    // take some of those jobs first (a large read is encoded in parallel), but the
    // reader only ever waits for work already running, never for work still queued
    // behind other readers' read-ahead.
    if (self) {
        for (size_t i = 1; i < jobs.size(); i++) submit([self, job = jobs[i]] { self->run(*job); }, Lane::Encode);
        for (auto& job : ahead) submit([self, job] { self->run(*job); }, Lane::Encode);
    }
    for (auto& job : jobs) run(*job);
    for (uint64_t j = j0; j < j1; j++) copy_overlap(out, *need[j - j0].get(), offsets_[j], a, len);
}

std::string EncodedTrack::describe() const { return std::format("flac-encoded:{}#samples={}..{}", img_->path.string(), start_, end_); }

}  // namespace wm
