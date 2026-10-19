// SPDX-License-Identifier: GPL-3.0-or-later
#include "wavpack.hpp"

#include <wavpack/wavpack.h>

#include <algorithm>
#include <bit>
#include <cstring>

#include "apetag.hpp"
#include "cache.hpp"
#include "md5.hpp"

namespace wm {

namespace {

const char INDEX_MAGIC[8] = {'W', 'M', 'W', 'V', 'I', 'X', '0', '1'};
const char LAYOUT_MAGIC[8] = {'W', 'M', 'W', 'V', 'L', 'Y', '0', '1'};
// Block header (WavpackHeader in wavpack.h), little-endian: byte offsets of the
// fields read or rewritten here. Sample numbers are 40 bits: a low 32-bit word
// plus a high byte.
constexpr size_t HEADER = 32;
constexpr size_t H_SIZE = 4, H_VERSION = 8, H_INDEX_HI = 10, H_TOTAL_HI = 11, H_TOTAL = 12, H_INDEX = 16, H_COUNT = 20, H_FLAGS = 24;
// the stream versions libwavpack decodes (its MIN_STREAM_VERS..MAX_STREAM_VERS)
constexpr uint16_t MIN_VERSION = 0x402, MAX_VERSION = 0x410;
/// samples decoded per call while verifying
constexpr uint32_t CHUNK = 1 << 16;
/// bump when plan() or encode() change what a track's head and tail are
constexpr int LAYOUT_VERSION = 1;
/// copied blocks kept per track while it is read (~1 MB of 192 kHz stereo float)
constexpr size_t CACHE_BLOCKS = 8;

/// Sub-blocks that describe the whole source file (its original WAV header and
/// trailer, its MD5); they do not belong in a track.
bool file_level(uint8_t id) {
    switch (id & ID_UNIQUE) {
    case ID_RIFF_HEADER:
    case ID_RIFF_TRAILER:
    case ID_ALT_HEADER:
    case ID_ALT_TRAILER:
    case ID_MD5_CHECKSUM:
    case ID_ALT_EXTENSION:
    case ID_ALT_MD5_CHECKSUM: return true;
    default: return false;
    }
}

/// f(offset, length with its header, id) for each sub-block of a block.
template <class F>
void sub_blocks(std::span<const uint8_t> b, F&& f) {
    size_t p = HEADER;
    while (p + 2 <= b.size()) {
        uint8_t id = b[p];
        size_t hl = 2, words = b[p + 1];
        if (id & ID_LARGE) {
            WM_ENSURE(p + 4 <= b.size(), "WavPack sub-block header overruns its block");
            hl = 4;
            words = size_t(b[p + 1]) | size_t(b[p + 2]) << 8 | size_t(b[p + 3]) << 16;
        }
        size_t len = hl + 2 * words;
        WM_ENSURE(p + len <= b.size(), "WavPack sub-block overruns its block");
        f(p, len, id);
        p += len;
    }
}

struct Header {
    uint32_t size;  // whole block
    uint64_t index;
    uint32_t count, flags;
};

std::optional<Header> parse_header(std::span<const uint8_t> h) {
    if (h.size() < HEADER || std::memcmp(h.data(), "wvpk", 4) != 0) return std::nullopt;
    // the version check keeps a stray "wvpk" (e.g. in an ID3v2 tag before the audio) from passing as a block
    uint16_t version = uint16_t(h[H_VERSION] | h[H_VERSION + 1] << 8);
    if (version < MIN_VERSION || version > MAX_VERSION) return std::nullopt;
    Header r;
    r.size = le32(&h[H_SIZE]) + 8;  // wraps to < HEADER for a corrupt size
    r.index = uint64_t(h[H_INDEX_HI]) << 32 | le32(&h[H_INDEX]);
    r.count = le32(&h[H_COUNT]);
    r.flags = le32(&h[H_FLAGS]);
    if (r.size < HEADER) return std::nullopt;
    return r;
}

/// Offset of the first block (files may start with other data, e.g. an ID3v2 tag).
std::optional<uint64_t> first_block(const File& f) {
    Bytes b = f.read_vec(0, 1 << 20);
    for (size_t i = 0; i + HEADER <= b.size(); i++)
        if (b[i] == 'w' && parse_header(std::span(b).subspan(i))) return i;
    return std::nullopt;
}

struct Context {
    WavpackContext* w = nullptr;
    explicit Context(WavpackContext* c) : w(c) {}
    ~Context() {
        if (w) WavpackCloseFile(w);
    }
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
};

Context open_input(const fs::path& p) {
    char err[256] = {};
    WavpackContext* w = WavpackOpenFileInput(p.c_str(), err, 0, 0);
    if (!w) fail("{}: {}", p.string(), err);
    return Context(w);
}

}  // namespace

bool WvImage::is_float(const fs::path& p) {
    File f(p);
    auto pos = first_block(f);
    if (!pos) return false;
    return parse_header(f.read_vec(*pos, HEADER))->flags & FLOAT_DATA;
}

void WvImage::read_settings() {
    auto c = open_input(path);
    int qmode = WavpackGetQualifyMode(c.w);
    mode_ = WavpackGetMode(c.w);
    WM_ENSURE(!(mode_ & MODE_HYBRID), "{}: hybrid (lossy) WavPack", path.string());
    WM_ENSURE(!(qmode & QMODE_DSD_AUDIO), "{}: DSD WavPack", path.string());
    // integer WavPack fits FLAC and is served as FLAC (AvImage)
    WM_ENSURE(mode_ & MODE_FLOAT, "{}: not floating-point WavPack", path.string());
    sample_rate = WavpackGetSampleRate(c.w);
    channels = uint32_t(WavpackGetNumChannels(c.w));
    bits_ = WavpackGetBitsPerSample(c.w);
    bytes_ = WavpackGetBytesPerSample(c.w);
    norm_exp_ = WavpackGetFloatNormExp(c.w);
    channel_mask_ = WavpackGetChannelMask(c.w);
    qmode_ = qmode & 0xFF;  // the bits stored in the file
    // Identities of the channels the mask does not cover (it covers the first
    // ones). libwavpack stores them in the first block only, so re-encoded track
    // heads must be given them again; 0xFF (unassigned) is what it assumes anyway.
    std::string ids(channels + 1, '\0');
    WavpackGetChannelIdentities(c.w, reinterpret_cast<unsigned char*>(ids.data()));
    ids.resize(std::strlen(ids.c_str()));
    ids.erase(0, std::min<size_t>(ids.size(), size_t(std::popcount(uint32_t(channel_mask_)))));
    if (ids.find_first_not_of('\xFF') != std::string::npos) chan_ids_ = ids;
}

std::shared_ptr<WvImage> WvImage::open(const fs::path& p, const Cache* cache) {
    auto key = SrcKey::of(p);
    auto img = std::make_shared<WvImage>();
    img->path = p;
    img->read_settings();
    File f(p);
    uint64_t fsize = f.size();
    if (auto t = ape::read([&](uint64_t o, size_t l) { return f.read_vec(o, l); }, fsize)) {
        img->tags = std::move(t->tags);
        img->front_cover = t->front_cover;
    }
    fs::path idx_path = cache ? cache->wv_index_path(p, key) : fs::path();
    if (cache)
        if (auto b = try_read_file(idx_path); b && b->size() >= 40 && std::memcmp(b->data(), INDEX_MAGIC, 8) == 0) {
            uint64_t n = le64(&(*b)[32]);
            if (b->size() == 40 + n * 28 && le32(&(*b)[8]) == img->sample_rate && le32(&(*b)[12]) == img->channels) {
                img->total = le64(&(*b)[16]);
                img->audio_end = le64(&(*b)[24]);
                img->blocks.resize(size_t(n));
                for (size_t i = 0; i < n; i++) {
                    const uint8_t* q = &(*b)[40 + 28 * i];
                    img->blocks[i] = {le64(q), le32(q + 8), le32(q + 12), le64(q + 16), le32(q + 24)};
                }
                return img;
            }
        }

    with_context(p.string(), [&] {
        // the blocks: where each starts, what a track keeps of it
        auto pos = first_block(f);
        WM_ENSURE(pos.has_value(), "no WavPack block");
        uint64_t next = 0;  // first sample of the next frame
        Bytes b;
        while (*pos + HEADER <= fsize) {
            Bytes h = f.read_vec(*pos, HEADER);
            auto hd = parse_header(h);
            if (!hd) break;  // tags follow the last block
            WM_ENSURE(*pos + hd->size <= fsize, "block at {} is cut short", *pos);
            WM_ENSURE(!(hd->flags & HYBRID_FLAG), "hybrid (lossy) WavPack");
            if (hd->count > 0) {
                b.resize(hd->size);
                f.read_exact_at(b.data(), b.size(), *pos);
                uint32_t kept = hd->size;
                sub_blocks(b, [&](size_t, size_t len, uint8_t id) {
                    if (file_level(id)) kept -= uint32_t(len);
                });
                // the blocks of a frame (one per channel pair) share their samples
                bool same_frame = !img->blocks.empty() && img->blocks.back().first == hd->index;
                if (same_frame) WM_ENSURE(img->blocks.back().count == hd->count, "blocks of a frame at {} disagree", *pos);
                else WM_ENSURE(hd->index == next, "block at {} starts at sample {}, expected {}", *pos, hd->index, next);
                img->blocks.push_back({*pos, hd->size, kept, hd->index, hd->count});
                next = hd->index + hd->count;
            }
            *pos += hd->size;
        }
        img->audio_end = *pos;
        img->total = next;
        WM_ENSURE(img->total > 0, "no audio");

        // verify: decode everything with the block checksums, and the file's MD5 if it has one
        auto c = open_input(p);
        int64_t declared = WavpackGetNumSamples64(c.w);
        WM_ENSURE(declared < 0 || uint64_t(declared) == img->total, "{} samples in the blocks, {} declared", img->total, declared);
        Md5 md5;  // of float samples as stored (little-endian), the WavPack convention
        std::vector<int32_t> buf(size_t(CHUNK) * img->channels);
        uint64_t got = 0;
        while (uint32_t n = WavpackUnpackSamples(c.w, buf.data(), CHUNK)) {
            md5.update({reinterpret_cast<const uint8_t*>(buf.data()), size_t(n) * img->channels * 4});
            got += n;
        }
        WM_ENSURE(WavpackGetNumErrors(c.w) == 0, "{} damaged blocks", WavpackGetNumErrors(c.w));
        WM_ENSURE(got == img->total, "decoded {} of {} samples", got, img->total);
        uint8_t want_md5[16];
        if (WavpackGetMD5Sum(c.w, want_md5)) WM_ENSURE(md5.finish() == std::to_array(want_md5), "audio MD5 mismatch");
    });
    WM_ENSURE(SrcKey::of(p) == key, "{} changed while indexing", p.string());
    if (cache) {
        Bytes b(INDEX_MAGIC, INDEX_MAGIC + 8);
        put_le32(b, img->sample_rate);
        put_le32(b, img->channels);
        put_le64(b, img->total);
        put_le64(b, img->audio_end);
        put_le64(b, img->blocks.size());
        for (auto& k : img->blocks) {
            put_le64(b, k.off);
            put_le32(b, k.size);
            put_le32(b, k.kept);
            put_le64(b, k.first);
            put_le32(b, k.count);
        }
        atomic_write(idx_path, b);
    }
    return img;
}

std::vector<int32_t> WvImage::decode(uint64_t s, uint64_t e) const {
    WM_ENSURE(s < e && e <= total, "bad sample range {}..{}", s, e);
    auto c = open_input(path);
    WM_ENSURE(WavpackSeekSample64(c.w, int64_t(s)), "{}: seek to sample {} failed", path.string(), s);
    std::vector<int32_t> out(size_t(e - s) * channels);
    uint64_t got = 0;
    while (got < e - s) {
        uint32_t n = WavpackUnpackSamples(c.w, out.data() + got * channels, uint32_t(std::min<uint64_t>(e - s - got, CHUNK)));
        if (n == 0) break;
        got += n;
    }
    WM_ENSURE(got == e - s && WavpackGetNumErrors(c.w) == 0, "{}: decoding samples {}..{} failed", path.string(), s, e);
    return out;
}

Bytes WvImage::encode(uint64_t s, uint64_t e, uint64_t out_first, uint64_t out_total) const {
    auto pcm = decode(s, e);
    Bytes out;
    auto sink = [](void* id, void* data, int32_t n) -> int {
        auto* o = static_cast<Bytes*>(id);
        o->insert(o->end(), static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + n);
        return 1;
    };
    Context c(WavpackOpenFileOutput(sink, &out, nullptr));
    WM_ENSURE(c.w, "libwavpack: cannot create an encoder");
    WavpackConfig cfg;
    std::memset(&cfg, 0, sizeof cfg);
    cfg.bits_per_sample = bits_;
    cfg.bytes_per_sample = bytes_;
    cfg.num_channels = int(channels);
    cfg.channel_mask = channel_mask_;
    cfg.sample_rate = int32_t(sample_rate);
    cfg.float_norm_exp = norm_exp_;
    cfg.qmode = qmode_;
    // the source's mode; one thread, so the output is the same every time
    if (mode_ & MODE_FAST) cfg.flags |= CONFIG_FAST_FLAG;
    if (mode_ & MODE_HIGH) cfg.flags |= CONFIG_HIGH_FLAG;
    if (mode_ & MODE_VERY_HIGH) cfg.flags |= CONFIG_VERY_HIGH_FLAG;
    if (mode_ & MODE_EXTRA) {
        cfg.flags |= CONFIG_EXTRA_MODE;
        cfg.xmode = (mode_ & MODE_XMODE) >> 12;
    }
    cfg.worker_threads = 0;
    auto ids = chan_ids_.empty() ? nullptr : reinterpret_cast<const unsigned char*>(chan_ids_.c_str());
    uint64_t n = e - s;
    WM_ENSURE(WavpackSetConfiguration64(c.w, &cfg, int64_t(n), ids) && WavpackPackInit(c.w) &&
                  WavpackPackSamples(c.w, pcm.data(), uint32_t(n)) && WavpackFlushSamples(c.w),
              "{}: encoding samples {}..{}: {}", path.string(), s, e, WavpackGetErrorMessage(c.w));
    // number the blocks for the track; without the WAV header libwavpack makes
    // up for a stream that has none (it would describe these few samples)
    Bytes res;
    res.reserve(out.size());
    size_t p = 0;
    while (p < out.size()) {
        auto h = parse_header(std::span(out).subspan(p));
        WM_ENSURE(h && p + h->size <= out.size(), "libwavpack produced a malformed block");
        append(res, rebuild_block(std::span(out).subspan(p, h->size), h->index + out_first, out_total, true));
        p += h->size;
    }
    return res;
}

Bytes rebuild_block(std::span<const uint8_t> block, uint64_t index, uint64_t total, bool strip) {
    Bytes out(block.begin(), block.begin() + HEADER);
    sub_blocks(block, [&](size_t p, size_t len, uint8_t id) {
        if (!(strip && file_level(id))) append(out, block.subspan(p, len));
    });
    auto put32 = [&](size_t at, uint32_t v) {
        for (size_t i = 0; i < 4; i++) out[at + i] = uint8_t(v >> (8 * i));
    };
    put32(H_SIZE, uint32_t(out.size() - 8));
    out[H_INDEX_HI] = uint8_t(index >> 32);
    put32(H_INDEX, uint32_t(index));
    // as libwavpack's SET_TOTAL_SAMPLES: all ones in the low word means "unknown
    // length", so from 2^32 - 1 samples on the value is stored shifted up by one
    // per 2^32 - 1 (GET_TOTAL_SAMPLES subtracts the high byte again)
    uint64_t stored_total = total + total / 0xFFFFFFFF;
    out[H_TOTAL_HI] = uint8_t(stored_total >> 32);
    put32(H_TOTAL, uint32_t(stored_total));
    // the block checksum covers everything before it, header included
    sub_blocks(out, [&](size_t p, size_t len, uint8_t id) {
        if ((id & ID_UNIQUE) != ID_BLOCK_CHECKSUM || (id & ID_LARGE)) return;
        uint32_t cs = 0xFFFFFFFF;
        for (size_t i = 0; i + 1 < p; i += 2) cs = cs * 3 + out[i] + (uint32_t(out[i + 1]) << 8);
        if (len - 2 == 2) {
            cs ^= cs >> 16;
            out[p + 2] = uint8_t(cs);
            out[p + 3] = uint8_t(cs >> 8);
        } else if (len - 2 == 4) {
            for (int i = 0; i < 4; i++) out[p + 2 + size_t(i)] = uint8_t(cs >> (8 * i));
        }
    });
    return out;
}

namespace {

/// How a track is made: re-encoded head and tail, copied blocks [b1, b2).
struct Plan {
    std::optional<std::pair<uint64_t, uint64_t>> head, tail;
    size_t b1 = 0, b2 = 0;
};

Plan plan(const WvImage& img, uint64_t s, uint64_t e) {
    WM_ENSURE(s < e && e <= img.total, "bad track range {}..{} (total {})", s, e, img.total);
    auto& bl = img.blocks;
    Plan r;
    uint64_t c0 = 0;
    if (s > 0) {
        // the first block of a track is always written anew: it carries the
        // stream configuration, which copied blocks after the first lack
        auto it = std::upper_bound(bl.begin(), bl.end(), s, [](uint64_t v, const WvImage::Block& b) { return v < b.first; }) - 1;
        c0 = std::min(e, it->first + it->count);
        r.head = {s, c0};
    }
    r.b1 = size_t(std::lower_bound(bl.begin(), bl.end(), c0, [](const WvImage::Block& b, uint64_t v) { return b.first < v; }) - bl.begin());
    r.b2 = r.b1;
    while (r.b2 < bl.size() && bl[r.b2].first + bl[r.b2].count <= e) r.b2++;
    uint64_t c1 = r.b2 > r.b1 ? bl[r.b2 - 1].first + bl[r.b2 - 1].count : c0;
    if (r.b2 == r.b1) r.b1 = r.b2 = 0;
    if (c1 < e) r.tail = {c1, e};
    return r;
}

}  // namespace

fs::path wv_layout_path(const WvImage& img, const std::vector<std::pair<uint64_t, uint64_t>>& ranges, const Cache& cache) {
    // sizes depend on the encoder, so its version is part of the name, and so is
    // LAYOUT_VERSION, for changes to how tracks are planned or encoded here
    std::string extra = std::format("wavpack-tracks v{} {}", LAYOUT_VERSION, WavpackGetLibraryVersionString());
    for (auto& [s, e] : ranges) extra += std::format(" {}-{}", s, e);
    return cache.dir() / "flacidx" / (Cache::name_for(img.path, SrcKey::of(img.path), extra) + ".wvlayout");
}

std::vector<WvLayout> wv_layouts(const WvImage& img, const std::vector<std::pair<uint64_t, uint64_t>>& ranges, const Cache* cache) {
    fs::path p = cache ? wv_layout_path(img, ranges, *cache) : fs::path();
    std::vector<WvLayout> out;
    if (cache)
        if (auto b = try_read_file(p); b && b->size() == 16 + 8 * ranges.size() && std::memcmp(b->data(), LAYOUT_MAGIC, 8) == 0 &&
                                       le64(&(*b)[8]) == ranges.size()) {
            for (size_t i = 0; i < ranges.size(); i++) out.push_back({le32(&(*b)[16 + 8 * i]), le32(&(*b)[20 + 8 * i])});
            return out;
        }
    for (auto [s, e] : ranges) {
        auto pl = plan(img, s, e);
        WvLayout l;
        if (pl.head) l.head = uint32_t(img.encode(pl.head->first, pl.head->second, 0, e - s).size());
        if (pl.tail) l.tail = uint32_t(img.encode(pl.tail->first, pl.tail->second, pl.tail->first - s, e - s).size());
        out.push_back(l);
    }
    if (cache) {
        Bytes b(LAYOUT_MAGIC, LAYOUT_MAGIC + 8);
        put_le64(b, ranges.size());
        for (auto& l : out) {
            put_le32(b, l.head);
            put_le32(b, l.tail);
        }
        atomic_write(p, b);
    }
    return out;
}

WvTrack::WvTrack(std::shared_ptr<const WvImage> img, uint64_t start, uint64_t end, const Tags& tags, const WvLayout& layout, const Cover* cover)
    : img_(std::move(img)), start_(start), end_(end) {
    auto pl = plan(*img_, start, end);
    if (pl.head) head_ = Part{pl.head->first, pl.head->second, layout.head};
    if (pl.tail) tail_ = Part{pl.tail->first, pl.tail->second, layout.tail};
    WM_ENSURE(bool(pl.head) == (layout.head > 0) && bool(pl.tail) == (layout.tail > 0), "track layout does not match the image");
    b1_ = pl.b1;
    b2_ = pl.b2;
    for (size_t i = b1_; i < b2_; i++) copy_size_ += img_->blocks[i].kept;
    tag_ = Segments(ape::build(tags, cover, cover ? std::nullopt : img_->front_cover));
    size_ = (head_ ? head_->size : 0) + copy_size_ + (tail_ ? tail_->size : 0) + tag_.size();
}

WvTrack::Shared WvTrack::part_bytes(Shared& cell, const Part& p) const {
    {
        std::lock_guard g(mu_);
        if (cell) return cell;
    }
    auto b = std::make_shared<const Bytes>(img_->encode(p.s, p.e, p.s - start_, end_ - start_));
    WM_ENSURE(b->size() == p.size, "{}: re-encoded samples {}..{} are {} bytes, measured {}", img_->path.string(), p.s, p.e, b->size(), p.size);
    std::lock_guard g(mu_);
    if (!cell) cell = std::move(b);
    return cell;
}

void WvTrack::drop_cached() const {
    std::lock_guard g(mu_);
    head_bytes_.reset();
    tail_bytes_.reset();
    blocks_.clear();
    vstarts_.reset();
}

WvTrack::Shared WvTrack::block_bytes(size_t i) const {
    {
        std::lock_guard g(mu_);
        if (auto it = blocks_.find(i); it != blocks_.end()) {
            it->second.second = ++clock_;
            return it->second.first;
        }
    }
    auto& k = img_->blocks[b1_ + i];
    Bytes raw(k.size);
    File(img_->path).read_exact_at(raw.data(), raw.size(), k.off);
    auto b = std::make_shared<const Bytes>(rebuild_block(raw, k.first - start_, end_ - start_, true));
    WM_ENSURE(b->size() == k.kept, "{}: block at {} changed", img_->path.string(), k.off);
    std::lock_guard g(mu_);
    blocks_[i] = {b, ++clock_};
    while (blocks_.size() > CACHE_BLOCKS)
        blocks_.erase(std::min_element(blocks_.begin(), blocks_.end(), [](auto& x, auto& y) { return x.second.second < y.second.second; }));
    return b;
}

std::shared_ptr<const std::vector<uint64_t>> WvTrack::vstarts() const {
    std::lock_guard g(mu_);
    if (!vstarts_) {
        std::vector<uint64_t> v;
        v.reserve(b2_ - b1_ + 1);
        uint64_t pos = 0;
        for (size_t i = b1_; i < b2_; i++) {
            v.push_back(pos);
            pos += img_->blocks[i].kept;
        }
        v.push_back(pos);
        vstarts_ = std::make_shared<const std::vector<uint64_t>>(std::move(v));
    }
    return vstarts_;
}

void WvTrack::read_copy(uint64_t off, size_t len, Bytes& out) const {
    auto keep = vstarts();
    const auto& vs = *keep;
    uint64_t end = off + len;
    size_t i0 = size_t(std::upper_bound(vs.begin(), vs.end(), off) - vs.begin()) - 1;
    size_t i1 = i0;
    while (i1 + 1 < vs.size() && vs[i1] < end) i1++;
    for (size_t i = i0; i < i1; i++) copy_overlap(out, *block_bytes(i), vs[i], off, len);
}

Bytes WvTrack::read_at(uint64_t off, size_t len) const {
    Bytes out;
    if (off >= size_) return out;
    len = size_t(std::min<uint64_t>(len, size_ - off));
    out.reserve(len);
    touch(weak_from_this().lock());
    uint64_t end = off + len, pos = 0;
    if (head_) {
        if (off < pos + head_->size && end > pos) copy_overlap(out, *part_bytes(head_bytes_, *head_), pos, off, len);
        pos += head_->size;
    }
    if (copy_size_ > 0) {
        if (off < pos + copy_size_ && end > pos) {
            uint64_t a = std::max(off, pos) - pos, b = std::min(end, pos + copy_size_) - pos;
            read_copy(a, size_t(b - a), out);
        }
        pos += copy_size_;
    }
    if (tail_) {
        if (off < pos + tail_->size && end > pos) copy_overlap(out, *part_bytes(tail_bytes_, *tail_), pos, off, len);
        pos += tail_->size;
    }
    tag_.read_at_base(img_->path, pos, off, len, out);
    WM_ENSURE(out.size() == len, "internal: produced {} of {} bytes at {}", out.size(), len, off);
    return out;
}

std::string WvTrack::describe() const { return std::format("wavpack-image:{}#samples={}..{}", img_->path.string(), start_, end_); }

std::shared_ptr<Spliced> retag_wavpack(const WvImage& img, const Tags& tags, const Cover* cover) {
    std::vector<Seg> segs = {SrcRange{0, img.audio_end}};
    for (auto& s : ape::build(tags, cover, cover ? std::nullopt : img.front_cover)) segs.push_back(std::move(s));
    return std::make_shared<Spliced>(img.path, "retagged-wv", std::move(segs));
}

}  // namespace wm
