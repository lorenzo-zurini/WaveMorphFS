// SPDX-License-Identifier: GPL-3.0-or-later
#include "track.hpp"

#include <algorithm>

#include "md5.hpp"

namespace wm {

namespace {

/// Blocks shorter than this are only legal as the last frame of a stream.
constexpr uint64_t MIN_BLOCK = 16;
/// Largest block a frame header can describe.
constexpr uint64_t MAX_BLOCK = 65535;

/// How a track is made: the partial source frames at its ends become VERBATIM
/// head and tail blocks, source frames [k1, k2) are copied.
struct Plan {
    std::optional<std::pair<uint64_t, uint64_t>> head, tail;
    uint64_t k1 = 0, k2 = 0;
};

Plan plan(const FlacImage& img, uint64_t start, uint64_t end) {
    uint64_t total = img.total(), bs = img.bs();
    Plan r;
    r.k1 = (start + bs - 1) / bs;
    r.k2 = end == total ? img.index.nframes() : end / bs;
    if (r.k1 > r.k2) {
        // start and end inside the same source frame
        r.head = {start, end};
        r.k1 = r.k2;
        return r;
    }
    if (start % bs) r.head = {start, std::min(r.k1 * bs, end)};
    if (end != total && end % bs) r.tail = {r.k2 * bs, end};
    // A block shorter than MIN_BLOCK is only legal last: merge a short head or tail
    // into the neighbouring frame, or a short head into the tail when no frame lies
    // between them (as long as the block stays describable).
    auto len = [](const auto& p) { return p->second - p->first; };
    if (r.head && len(r.head) < MIN_BLOCK) {
        if (r.k1 < r.k2 && len(r.head) + (img.frame_end(r.k1) - r.k1 * bs) <= MAX_BLOCK) {
            r.head = {r.head->first, img.frame_end(r.k1)};
            r.k1++;
        } else if (r.k1 == r.k2 && r.tail && len(r.head) + len(r.tail) <= MAX_BLOCK) {
            r.head = {r.head->first, r.tail->second};
            r.tail.reset();
        }
    }
    if (r.tail && len(r.tail) < MIN_BLOCK && r.k1 < r.k2 && len(r.tail) + bs <= MAX_BLOCK) {
        r.tail = {(r.k2 - 1) * bs, r.tail->second};
        r.k2--;
    }
    return r;
}

}  // namespace

std::shared_ptr<FlacImage> FlacImage::open(fs::path p, flac::FlacMeta meta, flac::FrameIndex index) {
    File f(p);
    uint8_t h[16] = {};
    f.read_exact_at(h, 16, index.offsets[0]);
    auto hdr = flac::FrameHeader::parse(h);
    WM_ENSURE(hdr.has_value(), "first frame header");
    // Indexes built before the last frame's end was searched from the front may
    // have taken a trailing tag whose own CRC-16 is 0 for audio. Checking that end
    // again is cheap (the last frame and what follows it), unlike re-indexing.
    uint64_t nf = index.nframes();
    auto last_end = flac::last_frame_end(f, index.offsets[nf - 1]);
    WM_ENSURE(last_end.has_value(), "last frame: no valid end followed by EOF or a tag (changed since it was indexed?)");
    if (*last_end != index.offsets[nf]) {
        warn("{}: cached frame index ends the audio at byte {}, but it ends at {}; using the latter", p.string(), index.offsets[nf], *last_end);
        auto fixed = std::make_shared<std::vector<uint64_t>>(index.offsets.begin(), index.offsets.end());
        (*fixed)[nf] = *last_end;
        index.offsets = *fixed;
        index.storage = std::move(fixed);
    }
    auto img = std::make_shared<FlacImage>();
    img->path = std::move(p);
    img->meta = std::move(meta);
    // tags and pictures were taken by the caller; tracks reference pictures in the file
    img->meta.blocks = {};
    img->index = std::move(index);
    img->sr_code = hdr->sr_code;
    auto sr = hdr->fields(h).sr_bytes;
    img->sr_extra.assign(sr.begin(), sr.end());
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
                     const std::vector<flac::HeaderBlock>& pictures, std::optional<std::array<uint8_t, 16>> md5)
    : img_(std::move(img)), start_(start), end_(end) {
    WM_ENSURE(start < end && end <= img_->total(), "bad track range {}..{} (total {})", start, end, img_->total());
    auto pl = plan(*img_, start, end);
    const auto& si = img_->si();
    auto mkseg = [&](std::pair<uint64_t, uint64_t> r) {
        uint64_t out_sample = r.first - start;
        return VSeg{r.first, r.second, out_sample, flac::verbatim_size(uint32_t(r.second - r.first), si.channels, si.bps, out_sample, img_->sr_extra.size())};
    };
    if (pl.head) head_ = mkseg(*pl.head);
    if (pl.tail) tail_ = mkseg(*pl.tail);
    k1_ = pl.k1;
    k2_ = pl.k2;
    for (uint64_t k = k1_; k < k2_; k++) copy_size_ += copied_size(k);

    // STREAMINFO for the track
    uint64_t bs = img_->bs();
    std::vector<uint64_t> blocks;
    if (head_) blocks.push_back(head_->e - head_->s);
    for (uint64_t k = k1_; k < k2_; k++) blocks.push_back(img_->frame_end(k) - k * bs);
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
    std::vector<flac::HeaderBlock> meta = {{flac::BLOCK_VORBIS, flac::build_vorbis("WaveMorphFS", tags.to_pairs())}};
    meta.insert(meta.end(), pictures.begin(), pictures.end());
    meta.push_back({flac::BLOCK_PADDING, Zeros{flac::EDIT_PADDING}});
    header_ = Segments(flac::header_segments(tsi.encode(0, 0), meta));
    size_ = header_.size() + (head_ ? head_->size : 0) + copy_size_ + (tail_ ? tail_->size : 0);
}

Bytes FlacTrack::verbatim(const VSeg& seg) const {
    auto chans = img_->decode_range(seg.s, seg.e);
    std::vector<std::span<const int32_t>> refs(chans.begin(), chans.end());
    auto fr = flac::encode_verbatim(refs, img_->si().bps, seg.out_sample, img_->sr_code, img_->sr_extra);
    WM_ENSURE(fr.size() == seg.size, "verbatim size mismatch {} != {}", fr.size(), seg.size);
    return fr;
}

uint64_t FlacTrack::copied_size(uint64_t k) const {
    auto& offs = img_->index.offsets;
    return offs[k + 1] - offs[k] + flac::coded_len(k * img_->bs() - start_) - flac::coded_len(k);
}

void FlacTrack::drop_cached() const {
    head_bytes_.reset();
    tail_bytes_.reset();
    std::lock_guard g(mu_);
    vstarts_.reset();
}

std::shared_ptr<const std::vector<uint64_t>> FlacTrack::vstarts() const {
    std::lock_guard g(mu_);
    if (!vstarts_) {
        std::vector<uint64_t> v;
        v.reserve(size_t(k2_ - k1_ + 1));
        uint64_t pos = 0;
        for (uint64_t k = k1_; k < k2_; k++) {
            v.push_back(pos);
            pos += copied_size(k);
        }
        v.push_back(pos);
        vstarts_ = std::make_shared<const std::vector<uint64_t>>(std::move(v));
    }
    return vstarts_;
}

void FlacTrack::read_copy(uint64_t off, size_t len, Bytes& out) const {
    auto keep = vstarts();
    const auto& vs = *keep;
    // frames [i0, i1) cover [off, off+len): vs[i0] <= off < vs[i0+1], vs[i1] >= off+len
    size_t i0 = size_t(std::upper_bound(vs.begin(), vs.end(), off) - vs.begin()) - 1;
    size_t i1 = std::min(size_t(std::lower_bound(vs.begin() + ptrdiff_t(i0), vs.end(), off + len) - vs.begin()), vs.size() - 1);
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
        auto hdr = flac::rewrite_header(orig, *h, k * bs - start_);
        auto body = orig.subspan(h->len, orig.size() - h->len - 2);
        // The new CRC-16 follows from the stored one and the header change alone:
        // the body is copied without being read again, and a source frame damaged
        // since it was indexed keeps a CRC that does not match, as it should.
        uint16_t stored = be16(&orig[orig.size() - 2]);
        uint16_t crc = stored ^ flac::crc16_shift(flac::crc16(orig.first(h->len)) ^ flac::crc16(hdr.span()), body.size());
        uint8_t crc_bytes[2] = {uint8_t(crc >> 8), uint8_t(crc)};
        uint64_t v = vs[i0 + j];
        copy_overlap(out, hdr.span(), v, off, len);
        copy_overlap(out, body, v + hdr.len, off, len);
        copy_overlap(out, crc_bytes, v + hdr.len + body.size(), off, len);
    }
}

Bytes FlacTrack::read_at(uint64_t off, size_t len) const {
    Bytes out;
    if (off >= size_) return out;
    len = size_t(std::min<uint64_t>(len, size_ - off));
    out.reserve(len);
    touch(weak_from_this().lock());
    auto boundary = [&](const std::optional<VSeg>& seg, const LazyBytes& bytes) {
        return Piece{seg ? seg->size : 0, [this, s = &seg, b = &bytes](uint64_t a, size_t n, Bytes& o) {
                         copy_overlap(o, *b->get([&] { return verbatim(**s); }), 0, a, n);
                     }};
    };
    stitch(out, off, len, Piece{header_.size(), [&](uint64_t a, size_t n, Bytes& o) { header_.read(img_->path, a, n, o); }}, boundary(head_, head_bytes_),
           Piece{copy_size_, [&](uint64_t a, size_t n, Bytes& o) { read_copy(a, n, o); }}, boundary(tail_, tail_bytes_));
    WM_ENSURE(out.size() == len, "internal: produced {} of {} bytes at {}", out.size(), len, off);
    return out;
}

std::string FlacTrack::describe() const { return std::format("flac-image:{}#samples={}..{}", img_->path.string(), start_, end_); }

}  // namespace wm
