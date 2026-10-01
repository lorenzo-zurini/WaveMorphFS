// SPDX-License-Identifier: GPL-3.0-or-later
#include "flac.hpp"

#include <FLAC/stream_decoder.h>
#include <FLAC/stream_encoder.h>

#include <algorithm>
#include <cstring>
#include <memory>

namespace wm::flac {

// ------------------------------------------------------------------ CRCs

static const auto CRC8 = [] {
    std::array<uint8_t, 256> t{};
    for (int i = 0; i < 256; i++) {
        uint8_t c = uint8_t(i);
        for (int j = 0; j < 8; j++) c = (c & 0x80) ? uint8_t(c << 1 ^ 0x07) : uint8_t(c << 1);
        t[i] = c;
    }
    return t;
}();

static const auto CRC16 = [] {
    std::array<uint16_t, 256> t{};
    for (int i = 0; i < 256; i++) {
        uint16_t c = uint16_t(i << 8);
        for (int j = 0; j < 8; j++) c = (c & 0x8000) ? uint16_t(c << 1 ^ 0x8005) : uint16_t(c << 1);
        t[i] = c;
    }
    return t;
}();

uint8_t crc8(std::span<const uint8_t> data) {
    uint8_t c = 0;
    for (auto b : data) c = CRC8[c ^ b];
    return c;
}

uint16_t crc16_update(uint16_t crc, std::span<const uint8_t> data) {
    for (auto b : data) crc = uint16_t(crc << 8) ^ CRC16[uint8_t(crc >> 8) ^ b];
    return crc;
}

// ------------------------------------------------------------------ numbers

size_t coded_len(uint64_t v) {
    if (v < 0x80) return 1;
    if (v < 0x800) return 2;
    if (v < 0x10000) return 3;
    if (v < 0x200000) return 4;
    if (v < 0x4000000) return 5;
    if (v < 0x80000000) return 6;
    return 7;
}

void encode_number(uint64_t v, Bytes& out) {
    size_t n = coded_len(v);
    if (n == 1) {
        out.push_back(uint8_t(v));
        return;
    }
    static const uint8_t lead[8] = {0, 0, 0xC0, 0xE0, 0xF0, 0xF8, 0xFC, 0xFE};
    size_t rest = n - 1;
    uint8_t first = n == 7 ? 0 : uint8_t(v >> (6 * rest));
    out.push_back(lead[n] | first);
    for (size_t i = rest; i-- > 0;) out.push_back(uint8_t(0x80 | ((v >> (6 * i)) & 0x3F)));
}

std::optional<std::pair<uint64_t, size_t>> decode_number(std::span<const uint8_t> b) {
    if (b.empty()) return std::nullopt;
    uint8_t f = b[0];
    size_t n;
    uint64_t v;
    if (f < 0x80) return std::pair{uint64_t(f), size_t(1)};
    if (f >= 0xC0 && f <= 0xDF) n = 2, v = f & 0x1F;
    else if (f >= 0xE0 && f <= 0xEF) n = 3, v = f & 0x0F;
    else if (f >= 0xF0 && f <= 0xF7) n = 4, v = f & 0x07;
    else if (f >= 0xF8 && f <= 0xFB) n = 5, v = f & 0x03;
    else if (f >= 0xFC && f <= 0xFD) n = 6, v = f & 0x01;
    else if (f == 0xFE) n = 7, v = 0;
    else return std::nullopt;
    if (b.size() < n) return std::nullopt;
    for (size_t i = 1; i < n; i++) {
        if ((b[i] & 0xC0) != 0x80) return std::nullopt;
        v = v << 6 | (b[i] & 0x3F);
    }
    return std::pair{v, n};
}

// ------------------------------------------------------------------ frame headers

std::optional<FrameHeader> FrameHeader::parse(std::span<const uint8_t> b) {
    if (b.size() < 6 || b[0] != 0xFF || (b[1] & 0xFE) != 0xF8) return std::nullopt;
    FrameHeader h;
    h.variable = b[1] & 1;
    h.bs_code = b[2] >> 4;
    h.sr_code = b[2] & 0x0F;
    h.ch_code = b[3] >> 4;
    h.bps_code = (b[3] >> 1) & 7;
    if (h.bs_code == 0 || h.sr_code == 0x0F || h.ch_code > 0x0A || h.bps_code == 3 || (b[3] & 1)) return std::nullopt;
    auto num = decode_number(b.subspan(4));
    if (!num) return std::nullopt;
    h.number = num->first;
    h.number_len = num->second;
    if (!h.variable && h.number_len > 6) return std::nullopt;
    size_t p = 4 + h.number_len;
    if (h.bs_code == 1) h.block_size = 192;
    else if (h.bs_code >= 2 && h.bs_code <= 5) h.block_size = 576u << (h.bs_code - 2);
    else if (h.bs_code == 6) {
        if (p >= b.size()) return std::nullopt;
        h.block_size = b[p] + 1u;
        p += 1;
    } else if (h.bs_code == 7) {
        if (p + 1 >= b.size()) return std::nullopt;
        h.block_size = be16(&b[p]) + 1u;
        p += 2;
    } else h.block_size = 256u << (h.bs_code - 8);
    if (h.sr_code == 12) p += 1;
    else if (h.sr_code == 13 || h.sr_code == 14) p += 2;
    if (p >= b.size()) return std::nullopt;
    if (crc8(b.subspan(0, p)) != b[p]) return std::nullopt;
    h.extra_len = p - 4 - h.number_len;
    h.len = p + 1;
    return h;
}

Bytes rewrite_header(std::span<const uint8_t> orig, const FrameHeader& h, uint64_t sample_number, std::optional<uint8_t> bps_code) {
    Bytes out;
    out.reserve(h.len + 2);
    out.push_back(0xFF);
    out.push_back(0xF9);
    out.push_back(orig[2]);
    out.push_back(bps_code ? uint8_t((orig[3] & 0xF1) | (*bps_code << 1)) : orig[3]);
    encode_number(sample_number, out);
    size_t es = 4 + h.number_len;
    out.insert(out.end(), orig.begin() + es, orig.begin() + es + h.extra_len);
    out.push_back(crc8(out));
    return out;
}

uint8_t bps_code_for(uint32_t bps) {
    switch (bps) {
    case 8: return 1;
    case 12: return 2;
    case 16: return 4;
    case 20: return 5;
    case 24: return 6;
    case 32: return 7;
    default: return 0;
    }
}

// ------------------------------------------------------------------ metadata

StreamInfo StreamInfo::parse(std::span<const uint8_t> b) {
    WM_ENSURE(b.size() >= 34, "short STREAMINFO");
    uint64_t x = be64(&b[10]);
    StreamInfo s;
    s.min_block = be16(&b[0]);
    s.max_block = be16(&b[2]);
    s.sample_rate = uint32_t(x >> 44);
    s.channels = uint32_t((x >> 41) & 7) + 1;
    s.bps = uint32_t((x >> 36) & 0x1F) + 1;
    s.total_samples = x & 0xFFFFFFFFFull;
    std::memcpy(s.md5.data(), &b[18], 16);
    return s;
}

std::array<uint8_t, 34> StreamInfo::encode(uint32_t min_frame, uint32_t max_frame) const {
    Bytes b;
    put_be16(b, min_block);
    put_be16(b, max_block);
    put_be24(b, min_frame);
    put_be24(b, max_frame);
    put_be64(b, uint64_t(sample_rate) << 44 | uint64_t(channels - 1) << 41 | uint64_t(bps - 1) << 36 | (total_samples & 0xFFFFFFFFFull));
    append(b, md5);
    std::array<uint8_t, 34> out;
    std::copy(b.begin(), b.end(), out.begin());
    return out;
}

FlacMeta FlacMeta::read(const fs::path& p) {
    return with_context("open " + p.string(), [&] {
        File f(p);
        uint8_t magic[4];
        f.read_exact_at(magic, 4, 0);
        uint64_t pos = 4;
        if (std::memcmp(magic, "ID3", 3) == 0) {
            // leading ID3v2 (non-standard but seen in the wild): skip it
            uint8_t rest[6];
            f.read_exact_at(rest, 6, 4);
            uint64_t sz = 0;
            for (int i = 2; i < 6; i++) sz = sz << 7 | (rest[i] & 0x7F);
            pos = 10 + sz;
            f.read_exact_at(magic, 4, pos);
            pos += 4;
        }
        WM_ENSURE(std::memcmp(magic, "fLaC", 4) == 0, "not a FLAC file");
        FlacMeta m;
        bool have_si = false;
        while (true) {
            uint8_t h[4];
            f.read_exact_at(h, 4, pos);
            bool last = h[0] & 0x80;
            uint8_t kind = h[0] & 0x7F;
            size_t len = size_t(h[1]) << 16 | size_t(h[2]) << 8 | h[3];
            Bytes data(len);
            f.read_exact_at(data.data(), len, pos + 4);
            pos += 4 + len;
            if (kind == BLOCK_STREAMINFO) {
                m.streaminfo = StreamInfo::parse(data);
                have_si = true;
            } else if (kind != BLOCK_PADDING) {
                m.blocks.push_back({kind, std::move(data)});
            }
            if (last) break;
        }
        WM_ENSURE(have_si, "no STREAMINFO");
        m.audio_start = pos;
        return m;
    });
}

std::vector<std::pair<std::string, std::string>> FlacMeta::vorbis_comments() const {
    std::vector<std::pair<std::string, std::string>> out;
    for (auto& b : blocks)
        if (b.kind == BLOCK_VORBIS)
            for (auto& kv : parse_vorbis(b.data)) out.push_back(kv);
    return out;
}

std::vector<const MetaBlock*> FlacMeta::pictures() const {
    std::vector<const MetaBlock*> out;
    for (auto& b : blocks)
        if (b.kind == BLOCK_PICTURE) out.push_back(&b);
    return out;
}

std::vector<std::pair<std::string, std::string>> parse_vorbis(std::span<const uint8_t> b) {
    std::vector<std::pair<std::string, std::string>> out;
    auto rd = [&](size_t p) -> std::optional<size_t> {
        if (p + 4 > b.size()) return std::nullopt;
        return size_t(le32(&b[p]));
    };
    auto vlen = rd(0);
    if (!vlen) return out;
    size_t p = 4 + *vlen;
    auto n = rd(p);
    if (!n) return out;
    p += 4;
    for (size_t i = 0; i < *n; i++) {
        auto l = rd(p);
        if (!l) break;
        p += 4;
        if (p + *l > b.size()) break;
        std::string s = lossy_utf8(b.subspan(p, *l));
        p += *l;
        auto eq = s.find('=');
        if (eq != std::string::npos) out.emplace_back(s.substr(0, eq), s.substr(eq + 1));
    }
    return out;
}

Bytes build_vorbis(const std::string& vendor, const std::vector<std::pair<std::string, std::string>>& tags) {
    Bytes b;
    put_le32(b, uint32_t(vendor.size()));
    append(b, vendor);
    put_le32(b, uint32_t(tags.size()));
    for (auto& [k, v] : tags) {
        put_le32(b, uint32_t(k.size() + 1 + v.size()));
        append(b, k);
        b.push_back('=');
        append(b, v);
    }
    return b;
}

Bytes build_header(const std::array<uint8_t, 34>& si, const std::vector<MetaBlock>& blocks) {
    Bytes out = {'f', 'L', 'a', 'C'};
    auto push = [&](uint8_t kind, std::span<const uint8_t> data, bool last) {
        out.push_back(kind | (last ? 0x80 : 0));
        put_be24(out, uint32_t(data.size()));
        append(out, data);
    };
    push(BLOCK_STREAMINFO, si, blocks.empty());
    for (size_t i = 0; i < blocks.size(); i++) push(blocks[i].kind, blocks[i].data, i + 1 == blocks.size());
    return out;
}

// ------------------------------------------------------------------ frame index

namespace {

/// Sliding read window over a file used by the indexer.
struct Window {
    const File& f;
    uint64_t flen;
    Bytes buf;
    uint64_t off;  // file offset of buf[0]
    bool eof = false;
    static constexpr size_t CHUNK = 4 << 20;

    void fill() {
        uint64_t pos = off + buf.size();
        if (pos >= flen) {
            eof = true;
            return;
        }
        size_t n = size_t(std::min<uint64_t>(CHUNK, flen - pos));
        size_t old = buf.size();
        buf.resize(old + n);
        f.read_exact_at(buf.data() + old, n, pos);
        if (pos + n >= flen) eof = true;
    }

    /// Drop everything before `cur`, rebase positions, read more.
    void compact(size_t& cur, size_t* p) {
        size_t d = cur;
        buf.erase(buf.begin(), buf.begin() + ptrdiff_t(d));
        off += d;
        cur = 0;
        if (p) *p -= d;
        fill();
    }
};

/// True if the bytes from `end` to EOF are empty or a recognisable trailing tag.
bool trailing_is_tag(const File& f, uint64_t end, uint64_t flen) {
    uint64_t tail = flen - end;
    if (tail == 0) return true;
    if (tail > (1 << 20)) return false;
    uint8_t head[8] = {};
    size_t n = size_t(std::min<uint64_t>(tail, 8));
    try {
        f.read_exact_at(head, n, end);
    } catch (...) {
        return false;
    }
    // No allowance for zero padding on purpose: an unfinished download also ends in zeros.
    return (tail == 128 && std::memcmp(head, "TAG", 3) == 0) || (n >= 8 && std::memcmp(head, "APETAGEX", 8) == 0) ||
           (n >= 3 && std::memcmp(head, "ID3", 3) == 0);
}

}  // namespace

FrameIndex FrameIndex::build(const fs::path& path, const FlacMeta& meta) {
    const auto& si = meta.streaminfo;
    WM_ENSURE(si.total_samples > 0, "STREAMINFO has unknown total sample count");
    File f(path);
    uint64_t flen = f.size();
    uint8_t first[16] = {};
    f.read_exact_at(first, 16, meta.audio_start);
    auto h0o = FrameHeader::parse(first);
    WM_ENSURE(h0o.has_value(), "no valid frame at audio start");
    auto h0 = *h0o;
    WM_ENSURE(!h0.variable, "variable-blocksize images are not supported");
    WM_ENSURE(h0.number == 0, "first frame number is not 0");
    uint32_t bs = h0.block_size;
    WM_ENSURE(si.min_block == si.max_block || si.max_block == bs, "inconsistent block size");
    uint64_t nframes = (si.total_samples + bs - 1) / bs;

    FrameIndex idx;
    idx.block_size = bs;
    idx.bps_in_header = h0.bps_code != 0;
    idx.offsets.reserve(nframes + 1);
    idx.offsets.push_back(meta.audio_start);

    Window w{f, flen, {}, meta.audio_start};
    w.fill();
    size_t cur = 0;  // start of current frame within w.buf
    for (uint64_t k = 0; k < nframes; k++) {
        if (w.buf.size() - cur < 64 && !w.eof) w.compact(cur, nullptr);
        auto hdr = FrameHeader::parse(std::span(w.buf).subspan(cur));
        WM_ENSURE(hdr.has_value(), "frame {} header invalid at byte {}", k, w.off + cur);
        WM_ENSURE(hdr->number == k, "frame {}: header says frame {}", k, hdr->number);
        bool is_last = k + 1 == nframes;
        uint16_t crc = crc16_update(0, std::span(w.buf).subspan(cur, hdr->len));
        size_t p = cur + hdr->len;
        std::vector<uint64_t> last_candidates;
        std::optional<uint64_t> end;
        while (true) {
            if (p >= w.buf.size()) {
                if (w.eof) break;
                w.compact(cur, &p);
                continue;
            }
            crc = uint16_t(crc << 8) ^ CRC16[uint8_t(crc >> 8) ^ w.buf[p]];
            p++;
            // p - cur is invariant under compaction; need at least header + CRC-16
            if (crc != 0 || p - cur < hdr->len + 2) continue;
            if (is_last) {
                // CRC-16 alone false-matches ~1/65536 bytes: collect every candidate
                // up to EOF and pick the one followed only by a recognisable tag.
                last_candidates.push_back(w.off + p);
                continue;
            }
            if (p + 32 > w.buf.size() && !w.eof) w.compact(cur, &p);
            if (auto nh = FrameHeader::parse(std::span(w.buf).subspan(p))) {
                // the final frame usually carries an explicit (smaller) block size
                bool next_is_last = k + 2 == nframes;
                if (nh->number == k + 1 && !nh->variable && nh->sr_code == h0.sr_code && nh->bps_code == h0.bps_code &&
                    (nh->bs_code == h0.bs_code || next_is_last)) {
                    end = w.off + p;
                    break;
                }
            }
        }
        if (is_last) {
            for (auto it = last_candidates.rbegin(); it != last_candidates.rend(); ++it)
                if (trailing_is_tag(f, *it, flen)) {
                    end = *it;
                    break;
                }
            WM_ENSURE(end.has_value(), "last frame: no valid end followed by EOF or a tag (incomplete file?)");
        } else {
            WM_ENSURE(end.has_value(), "frame {}: no valid end found (incomplete file?)", k);
        }
        idx.offsets.push_back(*end);
        cur = size_t(*end - w.off);
    }
    return idx;
}

// ------------------------------------------------------------------ VERBATIM encoder

namespace {
struct BitWriter {
    Bytes buf;
    uint64_t acc = 0;
    uint32_t nbits = 0;
    void put(uint64_t value, uint32_t bits) {
        acc = acc << bits | (value & ((uint64_t(1) << bits) - 1));
        nbits += bits;
        while (nbits >= 8) {
            nbits -= 8;
            buf.push_back(uint8_t(acc >> nbits));
        }
        acc &= (uint64_t(1) << nbits) - 1;
    }
    void align() {
        if (nbits) put(0, 8 - nbits);
    }
};
}  // namespace

uint64_t verbatim_size(uint32_t samples, uint32_t channels, uint32_t bps, uint64_t sample_number, size_t sr_extra) {
    size_t bs_extra = samples <= 256 ? 1 : 2;
    uint64_t header = 4 + coded_len(sample_number) + bs_extra + sr_extra + 1;
    uint64_t bits = uint64_t(channels) * (8 + uint64_t(samples) * bps);
    return header + (bits + 7) / 8 + 2;
}

Bytes encode_verbatim(const std::vector<std::span<const int32_t>>& chans, uint32_t bps, uint64_t sample_number, uint8_t sr_code,
                      std::span<const uint8_t> sr_extra) {
    size_t n = chans[0].size();
    WM_ENSURE(n >= 1 && n <= 65535, "FLAC block size must be 1..=65535");
    Bytes h = {0xFF, 0xF9};
    uint8_t bs_code = n <= 256 ? 6 : 7;
    h.push_back(uint8_t(bs_code << 4 | sr_code));
    h.push_back(uint8_t((chans.size() - 1) << 4 | bps_code_for(bps) << 1));
    encode_number(sample_number, h);
    if (bs_code == 6) h.push_back(uint8_t(n - 1));
    else put_be16(h, uint16_t(n - 1));
    append(h, sr_extra);
    h.push_back(crc8(h));
    BitWriter w{std::move(h)};
    for (auto& ch : chans) {
        w.put(0b00000010, 8);  // zero pad bit, SUBFRAME_VERBATIM, no wasted bits
        for (int32_t s : ch) w.put(uint32_t(s), bps);
    }
    w.align();
    uint16_t crc = crc16(w.buf);
    put_be16(w.buf, crc);
    return std::move(w.buf);
}

// ------------------------------------------------------------------ encoding (libFLAC)

namespace {
struct FrameSink {
    Bytes frame;
};

FLAC__StreamEncoderWriteStatus enc_write(const FLAC__StreamEncoder*, const FLAC__byte buffer[], size_t bytes, uint32_t samples, uint32_t, void* cd) {
    // metadata is written with samples == 0; only the audio frame is kept
    if (samples > 0) static_cast<FrameSink*>(cd)->frame.insert(static_cast<FrameSink*>(cd)->frame.end(), buffer, buffer + bytes);
    return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
}
}  // namespace

Bytes encode_frame(const std::vector<std::span<const int32_t>>& chans, uint32_t bps, uint32_t sample_rate, uint32_t block_size, uint64_t frame_number) {
    size_t n = chans[0].size();
    WM_ENSURE(n >= 1 && n <= block_size, "bad block of {} samples", n);
    FLAC__StreamEncoder* e = FLAC__stream_encoder_new();
    WM_ENSURE(e, "out of memory");
    struct Free {
        FLAC__StreamEncoder* e;
        ~Free() { FLAC__stream_encoder_delete(e); }
    } guard{e};
    FLAC__stream_encoder_set_channels(e, uint32_t(chans.size()));
    FLAC__stream_encoder_set_bits_per_sample(e, bps);
    FLAC__stream_encoder_set_sample_rate(e, sample_rate);
    FLAC__stream_encoder_set_compression_level(e, 5);
    // per-frame stereo decisions only (loose mid-side would carry state between frames)
    FLAC__stream_encoder_set_loose_mid_side_stereo(e, false);
    FLAC__stream_encoder_set_blocksize(e, block_size);
    FLAC__stream_encoder_set_total_samples_estimate(e, n);
    FrameSink sink;
    if (FLAC__stream_encoder_init_stream(e, enc_write, nullptr, nullptr, nullptr, &sink) != FLAC__STREAM_ENCODER_INIT_STATUS_OK)
        fail("FLAC encoder init failed");
    std::vector<const FLAC__int32*> ptrs;
    for (auto& c : chans) ptrs.push_back(c.data());
    bool ok = FLAC__stream_encoder_process(e, ptrs.data(), uint32_t(n)) && FLAC__stream_encoder_finish(e);
    WM_ENSURE(ok, "FLAC encoding failed: {}", FLAC__StreamEncoderStateString[FLAC__stream_encoder_get_state(e)]);
    // the frame was numbered 0; renumber it
    auto h = FrameHeader::parse(sink.frame);
    WM_ENSURE(h.has_value() && !h->variable && h->number == 0, "unexpected encoder output");
    Bytes out(sink.frame.begin(), sink.frame.begin() + 4);
    encode_number(frame_number, out);
    size_t es = 4 + h->number_len;
    out.insert(out.end(), sink.frame.begin() + ptrdiff_t(es), sink.frame.begin() + ptrdiff_t(es + h->extra_len));
    out.push_back(crc8(out));
    out.insert(out.end(), sink.frame.begin() + ptrdiff_t(h->len), sink.frame.end() - 2);
    put_be16(out, crc16(out));
    return out;
}

// ------------------------------------------------------------------ decoding (libFLAC)

namespace {

struct Decoder {
    FLAC__StreamDecoder* d = FLAC__stream_decoder_new();
    ~Decoder() {
        if (d) FLAC__stream_decoder_delete(d);
    }
};

struct MemSource {
    std::span<const uint8_t> data;
    size_t pos = 0;
    std::vector<std::vector<int32_t>> out;
    std::string error;
};

FLAC__StreamDecoderReadStatus mem_read(const FLAC__StreamDecoder*, FLAC__byte buffer[], size_t* bytes, void* cd) {
    auto* s = static_cast<MemSource*>(cd);
    size_t n = std::min(*bytes, s->data.size() - s->pos);
    std::memcpy(buffer, s->data.data() + s->pos, n);
    s->pos += n;
    *bytes = n;
    return n ? FLAC__STREAM_DECODER_READ_STATUS_CONTINUE : FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
}

FLAC__StreamDecoderWriteStatus mem_write(const FLAC__StreamDecoder*, const FLAC__Frame* fr, const FLAC__int32* const buf[], void* cd) {
    auto* s = static_cast<MemSource*>(cd);
    if (!s->out.empty()) return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;  // only one frame expected
    for (unsigned c = 0; c < fr->header.channels; c++) s->out.emplace_back(buf[c], buf[c] + fr->header.blocksize);
    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

void on_error(const FLAC__StreamDecoder*, FLAC__StreamDecoderErrorStatus st, void* cd) {
    static_cast<MemSource*>(cd)->error = FLAC__StreamDecoderErrorStatusString[st];
}

}  // namespace

std::vector<std::vector<int32_t>> decode_frame(std::span<const uint8_t> frame, uint32_t bps, uint32_t sample_rate) {
    auto h = FrameHeader::parse(frame);
    WM_ENSURE(h.has_value(), "bad frame header");
    Bytes owned;
    // libFLAC needs STREAMINFO for headers that defer to it: write explicit codes instead
    if (h->bps_code == 0 || h->sr_code == 0) {
        uint8_t bps_code = h->bps_code ? h->bps_code : bps_code_for(bps);
        WM_ENSURE(bps_code != 0, "unsupported bits per sample {}", bps);
        uint8_t sr_code = h->sr_code;
        Bytes sr_bytes;
        size_t es = 4 + h->number_len;
        size_t bs_extra = h->bs_code == 6 ? 1 : h->bs_code == 7 ? 2 : 0;
        if (sr_code == 0) {
            if (sample_rate <= 65535) sr_code = 13, put_be16(sr_bytes, uint16_t(sample_rate));
            else if (sample_rate % 10 == 0 && sample_rate / 10 <= 65535) sr_code = 14, put_be16(sr_bytes, uint16_t(sample_rate / 10));
            else fail("unsupported sample rate {}", sample_rate);
        } else {
            sr_bytes.assign(frame.begin() + es + bs_extra, frame.begin() + es + h->extra_len);
        }
        owned = {0xFF, frame[1], uint8_t(h->bs_code << 4 | sr_code), uint8_t((frame[3] & 0xF1) | bps_code << 1)};
        owned.insert(owned.end(), frame.begin() + 4, frame.begin() + es + bs_extra);
        append(owned, sr_bytes);
        owned.push_back(crc8(owned));
        owned.insert(owned.end(), frame.begin() + h->len, frame.end() - 2);
        put_be16(owned, crc16(owned));
        frame = owned;
    }
    Decoder dec;
    WM_ENSURE(dec.d, "out of memory");
    MemSource src;
    src.data = frame;
    if (FLAC__stream_decoder_init_stream(dec.d, mem_read, nullptr, nullptr, nullptr, nullptr, mem_write, nullptr, on_error, &src) !=
        FLAC__STREAM_DECODER_INIT_STATUS_OK)
        fail("FLAC decoder init failed");
    for (int i = 0; i < 8 && src.out.empty(); i++) {
        if (!FLAC__stream_decoder_process_single(dec.d)) break;
        if (FLAC__stream_decoder_get_state(dec.d) == FLAC__STREAM_DECODER_END_OF_STREAM) break;
    }
    WM_ENSURE(!src.out.empty(), "decode error: {}", src.error.empty() ? "no frame decoded" : src.error);
    WM_ENSURE(src.error.empty(), "decode error: {}", src.error);
    return std::move(src.out);
}

namespace {
struct FileSink {
    const std::function<void(uint64_t, const std::vector<std::span<const int32_t>>&)>* sink;
    uint64_t pos = 0;
    std::string error;
    std::string exception;
};

FLAC__StreamDecoderWriteStatus file_write(const FLAC__StreamDecoder*, const FLAC__Frame* fr, const FLAC__int32* const buf[], void* cd) {
    auto* s = static_cast<FileSink*>(cd);
    std::vector<std::span<const int32_t>> ch;
    for (unsigned c = 0; c < fr->header.channels; c++) ch.emplace_back(buf[c], fr->header.blocksize);
    try {
        (*s->sink)(s->pos, ch);
    } catch (const std::exception& e) {
        s->exception = e.what();
        return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }
    s->pos += fr->header.blocksize;
    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

void file_error(const FLAC__StreamDecoder*, FLAC__StreamDecoderErrorStatus st, void* cd) {
    auto* s = static_cast<FileSink*>(cd);
    if (s->error.empty()) s->error = FLAC__StreamDecoderErrorStatusString[st];
}
}  // namespace

void decode_file(const fs::path& p, uint64_t expected, const std::function<void(uint64_t, const std::vector<std::span<const int32_t>>&)>& sink) {
    Decoder dec;
    WM_ENSURE(dec.d, "out of memory");
    FileSink s;
    s.sink = &sink;
    if (FLAC__stream_decoder_init_file(dec.d, p.c_str(), file_write, nullptr, file_error, &s) != FLAC__STREAM_DECODER_INIT_STATUS_OK)
        fail("cannot open {} for decoding", p.string());
    bool ok = FLAC__stream_decoder_process_until_end_of_stream(dec.d);
    FLAC__stream_decoder_finish(dec.d);
    if (!s.exception.empty()) throw Error(s.exception);
    if (s.pos >= expected) return;
    WM_ENSURE(ok && s.error.empty(), "decoding {}: {}", p.string(), s.error.empty() ? "decoder failed" : s.error);
}

}  // namespace wm::flac
