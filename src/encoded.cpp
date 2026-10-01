// SPDX-License-Identifier: GPL-3.0-or-later
#include "encoded.hpp"

#include <algorithm>
#include <cstring>

#include "cache.hpp"
#include "md5.hpp"

namespace wm {

namespace {

const char LAYOUT_MAGIC[8] = {'W', 'M', 'E', 'N', 'C', '0', '0', '1'};
constexpr size_t RECENT = 8;
/// frames decoded per batch while measuring
constexpr uint64_t BATCH = 64;

fs::path layout_path(const AvImage& img, const Ranges& ranges, const Cache& cache) {
    std::string extra = "flac-encoded";
    for (auto& [s, e] : ranges) extra += std::format(" {}-{}", s, e);
    return cache.dir() / "flacidx" / (Cache::name_for(img.path, SrcKey::of(img.path), extra) + ".encidx");
}

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
            for (uint64_t j = j0; j < j1; j++) {
                size_t from = size_t((j - j0) * ENCODED_BLOCK);
                size_t n = size_t(std::min<uint64_t>(ENCODED_BLOCK, b - a - from));
                tl.frame_sizes.push_back(uint32_t(flac::encode_frame(slice(pcm, from, n), img.bps, img.sample_rate, ENCODED_BLOCK, j).size()));
            }
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

EncodedTrack::EncodedTrack(std::shared_ptr<const AvImage> img, uint64_t start, uint64_t end, const Tags& tags, const TrackLayout& layout)
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
    header_ = flac::build_header(si.encode(*mn, *mx), {{flac::BLOCK_VORBIS, flac::build_vorbis("WaveMorphFS", tags.to_pairs())}});
    size_ = header_.size() + pos;
}

Bytes EncodedTrack::read_at(uint64_t off, size_t len) const {
    Bytes out;
    if (off >= size_) return out;
    len = size_t(std::min<uint64_t>(len, size_ - off));
    out.reserve(len);
    copy_overlap(out, header_, 0, off, len);
    uint64_t h = header_.size(), end = off + len;
    if (end <= h) return out;
    uint64_t a = std::max(off, h) - h, b = end - h;  // audio-relative
    size_t j0 = size_t(std::upper_bound(offsets_.begin(), offsets_.end(), a) - offsets_.begin()) - 1;
    size_t j1 = size_t(std::lower_bound(offsets_.begin(), offsets_.end(), b) - offsets_.begin());  // exclusive
    std::vector<std::shared_ptr<const Bytes>> frames(j1 - j0);
    size_t first_missing = j1, last_missing = j0;
    {
        std::lock_guard g(mu_);
        for (size_t j = j0; j < j1; j++) {
            for (auto& [k, v] : recent_)
                if (k == j) frames[j - j0] = v;
            if (!frames[j - j0]) first_missing = std::min(first_missing, j), last_missing = std::max(last_missing, j + 1);
        }
    }
    if (first_missing < j1) {
        uint64_t sa = start_ + uint64_t(first_missing) * ENCODED_BLOCK, sb = std::min(end_, start_ + uint64_t(last_missing) * ENCODED_BLOCK);
        auto pcm = img_->decode_range(sa, sb);
        for (size_t j = first_missing; j < last_missing; j++) {
            if (frames[j - j0]) continue;
            size_t from = (j - first_missing) * ENCODED_BLOCK;
            size_t n = size_t(std::min<uint64_t>(ENCODED_BLOCK, sb - sa - from));
            auto fr = std::make_shared<const Bytes>(flac::encode_frame(slice(pcm, from, n), img_->bps, img_->sample_rate, ENCODED_BLOCK, j));
            WM_ENSURE(fr->size() == offsets_[j + 1] - offsets_[j], "frame {} encoded to {} bytes, layout says {}", j, fr->size(), offsets_[j + 1] - offsets_[j]);
            frames[j - j0] = fr;
            std::lock_guard g(mu_);
            recent_.emplace_back(j, fr);
            if (recent_.size() > RECENT) recent_.erase(recent_.begin());
        }
    }
    for (size_t j = j0; j < j1; j++) copy_overlap(out, *frames[j - j0], h + offsets_[j], off, len);
    WM_ENSURE(out.size() == len, "internal: produced {} of {} bytes at {}", out.size(), len, off);
    return out;
}

std::string EncodedTrack::describe() const { return std::format("flac-encoded:{}#samples={}..{}", img_->path.string(), start_, end_); }

}  // namespace wm
