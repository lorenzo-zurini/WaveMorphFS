// SPDX-License-Identifier: GPL-3.0-or-later
#include "encoded.hpp"

#include <algorithm>
#include <cstring>

#include "cache.hpp"
#include "md5.hpp"

namespace wm {

fs::path layout_path(const AvImage& img, const Ranges& ranges, const Cache& cache) {
    std::string extra = "flac-encoded";
    for (auto& [s, e] : ranges) extra += std::format(" {}-{}", s, e);
    return cache.dir() / "flacidx" / (Cache::name_for(img.path, SrcKey::of(img.path), extra) + ".encidx");
}

namespace {

const char LAYOUT_MAGIC[8] = {'W', 'M', 'E', 'N', 'C', '0', '0', '1'};
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

std::optional<std::vector<TrackLayout>> load_layout(const AvImage& img, const Ranges& ranges, const Cache& cache) {
    auto b = try_read_file(layout_path(img, ranges, cache));
    if (!b || b->size() < 16 || std::memcmp(b->data(), LAYOUT_MAGIC, 8) != 0 || le64(&(*b)[8]) != ranges.size()) return std::nullopt;
    std::vector<TrackLayout> out(ranges.size());
    size_t p = 16;
    for (size_t t = 0; t < ranges.size(); t++) {
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
    size_t bytes = (img.bps + 7) / 8;
    Bytes pcm_bytes;
    for (auto [s, e] : ranges) {
        TrackLayout tl;
        Md5 md5;
        uint64_t nf = frames_of(e - s);
        for (uint64_t j0 = 0; j0 < nf; j0 += BATCH) {
            uint64_t j1 = std::min(nf, j0 + BATCH);
            uint64_t a = s + j0 * ENCODED_BLOCK, b = std::min(e, s + j1 * ENCODED_BLOCK);
            auto pcm = img.decode_range(a, b);
            for (auto& fr : flac::encode_frames(slice(pcm, 0, size_t(b - a)), img.bps, img.sample_rate, ENCODED_BLOCK, j0))
                tl.frame_sizes.push_back(uint32_t(fr.size()));
            pcm_bytes.clear();
            for (size_t i = 0; i < pcm[0].size(); i++)
                for (auto& c : pcm) {
                    uint32_t v = uint32_t(c[i]);
                    for (size_t k = 0; k < bytes; k++) pcm_bytes.push_back(uint8_t(v >> (8 * k)));
                }
            md5.update(pcm_bytes);
        }
        tl.md5 = md5.finish();
        out.push_back(std::move(tl));
    }
    if (cache) {
        Bytes b(LAYOUT_MAGIC, LAYOUT_MAGIC + 8);
        put_le64(b, out.size());
        for (auto& tl : out) {
            append(b, tl.md5);
            put_le32(b, uint32_t(tl.frame_sizes.size()));
            for (auto sz : tl.frame_sizes) put_le32(b, sz);
        }
        atomic_write(layout_path(img, ranges, *cache), b);
    }
    return out;
}

EncodedTrack::EncodedTrack(std::shared_ptr<const AvImage> img, uint64_t start, uint64_t end, const Tags& tags, const TrackLayout& layout,
                           const std::vector<flac::HeaderBlock>& pictures)
    : img_(std::move(img)), start_(start), end_(end) {
    WM_ENSURE(start < end && end <= img_->total, "bad track range {}..{}", start, end);
    WM_ENSURE(layout.frame_sizes.size() == frames_of(end - start), "layout does not match the track");
    offsets_.reserve(layout.frame_sizes.size() + 1);
    uint64_t pos = 0;
    for (auto sz : layout.frame_sizes) {
        offsets_.push_back(uint32_t(pos));
        pos += sz;
    }
    WM_ENSURE(pos <= UINT32_MAX, "track too large");
    offsets_.push_back(uint32_t(pos));
    flac::StreamInfo si;
    uint64_t len = end - start;
    si.min_block = si.max_block = uint16_t(len < ENCODED_BLOCK ? len : ENCODED_BLOCK);
    si.sample_rate = img_->sample_rate;
    si.channels = img_->channels;
    si.bps = img_->bps;
    si.total_samples = len;
    si.md5 = layout.md5;
    auto [mn, mx] = std::minmax_element(layout.frame_sizes.begin(), layout.frame_sizes.end());
    std::vector<flac::HeaderBlock> meta = {{flac::BLOCK_VORBIS, flac::build_vorbis("WaveMorphFS", tags.to_pairs())}};
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
                if (fr->size() != offsets_[j + 1] - offsets_[j])
                    fail("frame {} encoded to {} bytes, layout says {}", j, fr->size(), offsets_[j + 1] - offsets_[j]);
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
    header_.read(img_->path, off, len, out);
    uint64_t h = header_.size(), end = off + len;
    if (end <= h) return out;
    uint64_t a = std::max(off, h) - h, b = end - h;  // audio-relative
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
    for (uint64_t j = j0; j < j1; j++) copy_overlap(out, *need[j - j0].get(), h + offsets_[j], off, len);
    WM_ENSURE(out.size() == len, "internal: produced {} of {} bytes at {}", out.size(), len, off);
    return out;
}

std::string EncodedTrack::describe() const { return std::format("flac-encoded:{}#samples={}..{}", img_->path.string(), start_, end_); }

}  // namespace wm
