// SPDX-License-Identifier: GPL-3.0-or-later
#include "track.hpp"

#include <algorithm>

#include "md5.hpp"

namespace wm {

/// Blocks shorter than this are only legal as the last frame of a stream.
constexpr uint64_t MIN_BLOCK = 16;

std::shared_ptr<FlacImage> FlacImage::open(fs::path p, flac::FlacMeta meta, flac::FrameIndex index) {
    File f(p);
    uint8_t h[16] = {};
    f.read_exact_at(h, 16, index.offsets[0]);
    auto hdr = flac::FrameHeader::parse(h);
    WM_ENSURE(hdr.has_value(), "first frame header");
    size_t ex = 4 + hdr->number_len;
    size_t bs_extra = hdr->bs_code == 6 ? 1 : hdr->bs_code == 7 ? 2 : 0;
    auto img = std::make_shared<FlacImage>();
    img->path = std::move(p);
    img->meta = std::move(meta);
    img->index = std::move(index);
    img->sr_code = hdr->sr_code;
    img->sr_extra.assign(h + ex + bs_extra, h + ex + hdr->extra_len);
    return img;
}

std::vector<std::vector<int32_t>> FlacImage::decode_range(uint64_t s, uint64_t e) const {
    uint64_t k0 = s / bs(), k1 = (e - 1) / bs();
    uint64_t a = index.offsets[k0], b = index.offsets[k1 + 1];
    File f(path);
    Bytes buf(size_t(b - a));
    WM_ENSURE(f.read_at(buf.data(), buf.size(), a) == buf.size(), "short read");
    size_t ch = si().channels;
    std::vector<std::vector<int32_t>> out(ch);
    for (auto& c : out) c.reserve(size_t(e - s));
    for (uint64_t k = k0; k <= k1; k++) {
        size_t fa = size_t(index.offsets[k] - a), fb = size_t(index.offsets[k + 1] - a);
        auto dec = flac::decode_frame(std::span(buf).subspan(fa, fb - fa), si().bps, si().sample_rate);
        uint64_t fs = k * bs();
        uint64_t lo = std::max(s, fs) - fs, hi = std::min(e, frame_end(k)) - fs;
        for (size_t c = 0; c < ch; c++) out[c].insert(out[c].end(), dec[c].begin() + ptrdiff_t(lo), dec[c].begin() + ptrdiff_t(hi));
    }
    return out;
}

std::vector<std::array<uint8_t, 16>> track_md5s(const FlacImage& img, const std::vector<std::pair<uint64_t, uint64_t>>& ranges) {
    const auto& si = img.si();
    size_t bytes = (si.bps + 7) / 8;
    std::vector<Md5> ctx(ranges.size());
    Bytes pcm;
    flac::decode_file(img.path, img.total(), [&](uint64_t fs, const std::vector<std::span<const int32_t>>& dec) {
        uint64_t fe = fs + dec[0].size();
        for (size_t ri = 0; ri < ranges.size(); ri++) {
            auto [s, e] = ranges[ri];
            if (s >= fe || e <= fs) continue;
            size_t lo = size_t(std::max(s, fs) - fs), hi = size_t(std::min(e, fe) - fs);
            pcm.clear();
            pcm.reserve((hi - lo) * dec.size() * bytes);
            for (size_t i = lo; i < hi; i++)
                for (auto& c : dec) {
                    uint32_t v = uint32_t(c[i]);
                    for (size_t k = 0; k < bytes; k++) pcm.push_back(uint8_t(v >> (8 * k)));
                }
            ctx[ri].update(pcm);
        }
    });
    std::vector<std::array<uint8_t, 16>> out;
    for (auto& c : ctx) out.push_back(c.finish());
    return out;
}

FlacTrack::FlacTrack(std::shared_ptr<const FlacImage> img, uint64_t start, uint64_t end, const Tags& tags,
                     const std::vector<flac::MetaBlock>& pictures, std::optional<std::array<uint8_t, 16>> md5)
    : img_(std::move(img)), start_(start), end_(end) {
    uint64_t total = img_->total(), bs = img_->bs(), nf = img_->index.nframes();
    WM_ENSURE(start < end && end <= total, "bad track range {}..{} (total {})", start, end, total);

    uint64_t k1 = (start + bs - 1) / bs;
    uint64_t k2 = end == total ? nf : end / bs;
    std::optional<std::pair<uint64_t, uint64_t>> head, tail;
    if (k1 > k2) {
        // start and end inside the same source frame
        head = {start, end};
        k1 = k2;
    } else {
        if (start % bs) head = {start, std::min(k1 * bs, end)};
        if (end != total && end % bs) tail = {k2 * bs, end};
        // avoid tiny non-final blocks: merge them with the neighbouring full frame
        if (head && head->second - head->first < MIN_BLOCK && k1 < k2) {
            head = {head->first, img_->frame_end(k1)};
            k1++;
        }
        if (tail && tail->second - tail->first < MIN_BLOCK && k1 < k2) {
            tail = {(k2 - 1) * bs, tail->second};
            k2--;
        }
    }
    const auto& si = img_->si();
    auto mkseg = [&](std::pair<uint64_t, uint64_t> r) {
        uint64_t out_sample = r.first - start;
        return VSeg{r.first, r.second, out_sample, flac::verbatim_size(uint32_t(r.second - r.first), si.channels, si.bps, out_sample, img_->sr_extra.size())};
    };
    if (head) head_ = mkseg(*head);
    if (tail) tail_ = mkseg(*tail);
    k1_ = k1;
    k2_ = k2;
    if (k1 < k2) {
        int64_t cs = int64_t(img_->index.offsets[k2] - img_->index.offsets[k1]);
        for (uint64_t k = k1; k < k2; k++) cs += int64_t(flac::coded_len(k * bs - start)) - int64_t(flac::coded_len(k));
        copy_size_ = uint64_t(cs);
    }

    // STREAMINFO for the track
    std::vector<uint64_t> blocks;
    if (head_) blocks.push_back(head_->e - head_->s);
    for (uint64_t k = k1; k < k2; k++) blocks.push_back(img_->frame_end(k) - k * bs);
    if (tail_) blocks.push_back(tail_->e - tail_->s);
    size_t nonlast = blocks.size() == 1 ? 1 : blocks.size() - 1;
    uint64_t min_b = *std::min_element(blocks.begin(), blocks.begin() + ptrdiff_t(nonlast));
    uint64_t max_b = *std::max_element(blocks.begin(), blocks.end());
    flac::StreamInfo tsi;
    tsi.min_block = uint16_t(min_b);
    tsi.max_block = uint16_t(max_b);
    tsi.sample_rate = si.sample_rate;
    tsi.channels = si.channels;
    tsi.bps = si.bps;
    tsi.total_samples = end - start;
    if (md5) tsi.md5 = *md5;  // all-zero = unknown until the background job has computed it
    std::vector<flac::MetaBlock> meta = {{flac::BLOCK_VORBIS, flac::build_vorbis("WaveMorphFS", tags.to_pairs())}};
    meta.insert(meta.end(), pictures.begin(), pictures.end());
    header_ = flac::build_header(tsi.encode(0, 0), meta);
    size_ = header_.size() + (head_ ? head_->size : 0) + copy_size_ + (tail_ ? tail_->size : 0);
}

Bytes FlacTrack::verbatim(const VSeg& seg) const {
    auto chans = img_->decode_range(seg.s, seg.e);
    std::vector<std::span<const int32_t>> refs(chans.begin(), chans.end());
    auto fr = flac::encode_verbatim(refs, img_->si().bps, seg.out_sample, img_->sr_code, img_->sr_extra);
    WM_ENSURE(fr.size() == seg.size, "verbatim size mismatch {} != {}", fr.size(), seg.size);
    return fr;
}

const Bytes& FlacTrack::seg_bytes(std::optional<Bytes>& cell, const VSeg& seg) const {
    {
        std::lock_guard g(mu_);
        if (cell) return *cell;
    }
    Bytes v = verbatim(seg);
    std::lock_guard g(mu_);
    if (!cell) cell = std::move(v);
    return *cell;
}

const std::vector<uint64_t>& FlacTrack::vstarts() const {
    std::lock_guard g(mu_);
    if (!vstarts_) {
        auto& offs = img_->index.offsets;
        uint64_t bs = img_->bs();
        std::vector<uint64_t> v;
        v.reserve(size_t(k2_ - k1_ + 1));
        int64_t pos = 0;
        for (uint64_t k = k1_; k < k2_; k++) {
            v.push_back(uint64_t(pos));
            pos += int64_t(offs[k + 1] - offs[k]) + int64_t(flac::coded_len(k * bs - start_)) - int64_t(flac::coded_len(k));
        }
        v.push_back(uint64_t(pos));
        vstarts_ = std::move(v);
    }
    return *vstarts_;
}

void FlacTrack::read_copy(uint64_t off, size_t len, Bytes& out) const {
    const auto& vs = vstarts();
    uint64_t end = off + len;
    // first frame i0 with vs[i0] <= off < vs[i0+1]
    size_t i0 = size_t(std::upper_bound(vs.begin(), vs.end(), off) - vs.begin()) - 1;
    size_t i1 = i0;
    while (i1 + 1 < vs.size() && vs[i1] < end) i1++;
    auto& offs = img_->index.offsets;
    uint64_t kf = k1_ + i0, kl = k1_ + i1;
    uint64_t a = offs[kf], b = offs[kl];
    File f(img_->path);
    Bytes raw(size_t(b - a));
    WM_ENSURE(f.read_at(raw.data(), raw.size(), a) == raw.size(), "short read in {}", img_->path.string());
    uint64_t bs = img_->bs();
    for (uint64_t k = kf, j = 0; k < kl; k++, j++) {
        size_t fa = size_t(offs[k] - a), fb = size_t(offs[k + 1] - a);
        std::span<const uint8_t> orig(raw.data() + fa, fb - fa);
        auto h = flac::FrameHeader::parse(orig);
        WM_ENSURE(h.has_value(), "frame {} header", k);
        Bytes frame = flac::rewrite_header(orig, *h, k * bs - start_, std::nullopt);
        frame.insert(frame.end(), orig.begin() + ptrdiff_t(h->len), orig.end() - 2);
        put_be16(frame, flac::crc16(frame));
        copy_overlap(out, frame, vs[i0 + j], off, len);
    }
}

Bytes FlacTrack::read_at(uint64_t off, size_t len) const {
    Bytes out;
    if (off >= size_) return out;
    len = size_t(std::min<uint64_t>(len, size_ - off));
    out.reserve(len);
    uint64_t end = off + len, pos = 0;
    copy_overlap(out, header_, pos, off, len);
    pos += header_.size();
    if (head_) {
        if (off < pos + head_->size && end > pos) copy_overlap(out, seg_bytes(head_bytes_, *head_), pos, off, len);
        pos += head_->size;
    }
    if (copy_size_ > 0) {
        if (off < pos + copy_size_ && end > pos) {
            uint64_t a = std::max(off, pos) - pos, b = std::min(end, pos + copy_size_) - pos;
            read_copy(a, size_t(b - a), out);
        }
        pos += copy_size_;
    }
    if (tail_ && off < pos + tail_->size && end > pos) copy_overlap(out, seg_bytes(tail_bytes_, *tail_), pos, off, len);
    WM_ENSURE(out.size() == len, "internal: produced {} of {} bytes at {}", out.size(), len, off);
    return out;
}

std::string FlacTrack::describe() const { return std::format("flac-image:{}#samples={}..{}", img_->path.string(), start_, end_); }

}  // namespace wm
