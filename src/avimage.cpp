// SPDX-License-Identifier: GPL-3.0-or-later
#include "avimage.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/defs.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
}

#include <algorithm>
#include <cstring>
#include <functional>

#include "cache.hpp"
#include "md5.hpp"

namespace wm {

namespace {

const char INDEX_MAGIC[8] = {'W', 'M', 'A', 'V', 'I', 'X', '0', '1'};
constexpr size_t RECENT = 6;

std::string av_err(int e) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(e, buf, sizeof buf);
    return buf;
}

using PerChannel = std::vector<std::vector<int32_t>>;

}  // namespace

/// A demuxer + decoder for one file.
struct AvImage::Decoder {
    AVFormatContext* fmt = nullptr;
    AVCodecContext* codec = nullptr;
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    int stream = -1;
    uint32_t bps = 0;

    explicit Decoder(const fs::path& p) {
        int r = avformat_open_input(&fmt, p.c_str(), nullptr, nullptr);
        if (r < 0) fail("cannot open {}: {}", p.string(), av_err(r));
        if ((r = avformat_find_stream_info(fmt, nullptr)) < 0) fail("{}: {}", p.string(), av_err(r));
        const AVCodec* dec = nullptr;
        stream = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &dec, 0);
        WM_ENSURE(stream >= 0 && dec, "{}: no decodable audio stream", p.string());
        codec = avcodec_alloc_context3(dec);
        avcodec_parameters_to_context(codec, fmt->streams[stream]->codecpar);
        codec->thread_count = 1;
        // checksums on, and any damage is an error: incomplete downloads must not pass
        codec->err_recognition = AV_EF_CRCCHECK | AV_EF_BITSTREAM | AV_EF_EXPLODE;
        if ((r = avcodec_open2(codec, dec, nullptr)) < 0) fail("{}: {}", p.string(), av_err(r));
        auto* par = fmt->streams[stream]->codecpar;
        switch (codec->sample_fmt) {
        case AV_SAMPLE_FMT_U8:
        case AV_SAMPLE_FMT_U8P: bps = 8; break;
        case AV_SAMPLE_FMT_S16:
        case AV_SAMPLE_FMT_S16P: bps = 16; break;
        case AV_SAMPLE_FMT_S32:
        case AV_SAMPLE_FMT_S32P:
            bps = par->bits_per_raw_sample > 16 && par->bits_per_raw_sample <= 32 ? uint32_t(par->bits_per_raw_sample)
                  : codec->bits_per_raw_sample > 16 && codec->bits_per_raw_sample <= 32 ? uint32_t(codec->bits_per_raw_sample)
                  : par->bits_per_coded_sample > 16 && par->bits_per_coded_sample <= 32 ? uint32_t(par->bits_per_coded_sample)
                  : 32;
            break;
        default: fail("{}: {} samples are not integer PCM", p.string(), av_get_sample_fmt_name(codec->sample_fmt));
        }
    }

    ~Decoder() {
        av_frame_free(&frame);
        av_packet_free(&pkt);
        avcodec_free_context(&codec);
        avformat_close_input(&fmt);
    }

    uint32_t channels() const { return uint32_t(codec->ch_layout.nb_channels); }

    /// Append the current frame's samples (per channel, as signed values of `bps` bits).
    void take(PerChannel& out) const {
        size_t ch = channels(), n = size_t(frame->nb_samples);
        out.resize(ch);
        bool planar = av_sample_fmt_is_planar(codec->sample_fmt);
        for (size_t c = 0; c < ch; c++) {
            auto& o = out[c];
            o.reserve(o.size() + n);
            for (size_t i = 0; i < n; i++) {
                size_t idx = planar ? i : i * ch + c;
                const uint8_t* base = frame->extended_data[planar ? c : 0];
                switch (codec->sample_fmt) {
                case AV_SAMPLE_FMT_U8:
                case AV_SAMPLE_FMT_U8P: o.push_back(int32_t(base[idx]) - 128); break;
                case AV_SAMPLE_FMT_S16:
                case AV_SAMPLE_FMT_S16P: o.push_back(reinterpret_cast<const int16_t*>(base)[idx]); break;
                default: o.push_back(reinterpret_cast<const int32_t*>(base)[idx] >> (32 - bps)); break;
                }
            }
        }
    }

    /// Decode the packet in `pkt`; append everything it produces.
    void decode_current(PerChannel& out) {
        int r = avcodec_send_packet(codec, pkt);
        if (r < 0) fail("decode error: {}", av_err(r));
        while ((r = avcodec_receive_frame(codec, frame)) >= 0) take(out);
        if (r != AVERROR(EAGAIN) && r != AVERROR_EOF) fail("decode error: {}", av_err(r));
    }

    /// Decode the whole file from the start: sink(packet pts, samples of that packet).
    void decode_all(const std::function<void(int64_t, const PerChannel&)>& sink) {
        PerChannel buf;
        int r;
        while ((r = av_read_frame(fmt, pkt)) >= 0) {
            if (pkt->stream_index == stream) {
                for (auto& c : buf) c.clear();
                int64_t pts = pkt->pts;
                decode_current(buf);
                sink(pts, buf);
            }
            av_packet_unref(pkt);
        }
        if (r != AVERROR_EOF) fail("read error: {}", av_err(r));
        // flush (lossless decoders have no delay, but be exact anyway)
        avcodec_send_packet(codec, nullptr);
        for (auto& c : buf) c.clear();
        while (avcodec_receive_frame(codec, frame) >= 0) take(buf);
        if (!buf.empty() && !buf[0].empty()) sink(AV_NOPTS_VALUE, buf);
    }
};

AvImage::~AvImage() = default;

std::shared_ptr<AvImage> AvImage::open(const fs::path& p, const Cache* cache) {
    auto key = SrcKey::of(p);
    auto img = std::make_shared<AvImage>();
    img->path = p;
    {
        Decoder d(p);
        img->sample_rate = uint32_t(d.codec->sample_rate);
        img->channels = d.channels();
        img->bps = d.bps;
        const AVDictionaryEntry* e = nullptr;
        while ((e = av_dict_iterate(d.fmt->metadata, e))) img->tags.add(e->key, e->value);
    }
    fs::path idx_path = cache ? cache->dir() / "flacidx" / (Cache::name_for(p, key, "av") + ".avidx") : fs::path();
    if (cache)
        if (auto b = try_read_file(idx_path); b && b->size() >= 32 && std::memcmp(b->data(), INDEX_MAGIC, 8) == 0) {
            uint64_t n = le64(&(*b)[24]);
            if (b->size() == 32 + n * 20 && le32(&(*b)[8]) == img->sample_rate && (*b)[12] == img->channels && (*b)[13] == img->bps) {
                img->total = le64(&(*b)[16]);
                img->packets_.resize(size_t(n));
                for (size_t i = 0; i < n; i++) {
                    const uint8_t* q = &(*b)[32 + 20 * i];
                    img->packets_[i] = {int64_t(le64(q)), le64(q + 8), le32(q + 16)};
                }
                return img;
            }
        }
    // verify + index: one full decode with checksums
    Decoder d(p);
    uint64_t pos = 0;
    with_context(p.string(), [&] {
        d.decode_all([&](int64_t pts, const PerChannel& s) {
            uint32_t n = s.empty() ? 0 : uint32_t(s[0].size());
            if (pts == AV_NOPTS_VALUE && !img->packets_.empty()) {
                img->packets_.back().count += n;  // trailing flush output belongs to the last packet
            } else {
                WM_ENSURE(pts != AV_NOPTS_VALUE, "packet without timestamp");
                img->packets_.push_back({pts, pos, n});
            }
            pos += n;
        });
    });
    img->total = pos;
    WM_ENSURE(img->total > 0, "{}: no audio", p.string());
    WM_ENSURE(SrcKey::of(p) == key, "{} changed while indexing", p.string());
    if (cache) {
        Bytes b(INDEX_MAGIC, INDEX_MAGIC + 8);
        put_le32(b, img->sample_rate);
        b.push_back(uint8_t(img->channels));
        b.push_back(uint8_t(img->bps));
        b.insert(b.end(), 2, 0);
        put_le64(b, img->total);
        put_le64(b, img->packets_.size());
        for (auto& k : img->packets_) {
            put_le64(b, uint64_t(k.pts));
            put_le64(b, k.first);
            put_le32(b, k.count);
        }
        atomic_write(idx_path, b);
    }
    return img;
}

AvImage::Samples AvImage::packet_samples(size_t k) const {
    std::lock_guard g(mu_);
    for (auto it = recent_.begin(); it != recent_.end(); ++it)
        if (it->first == k) {
            auto v = it->second;
            recent_.erase(it);
            recent_.emplace_back(k, v);
            return v;
        }
    if (!dec_) dec_ = std::make_unique<Decoder>(path);
    auto& d = *dec_;
    const auto& want = packets_[k];
    int r = av_seek_frame(d.fmt, d.stream, want.pts, AVSEEK_FLAG_BACKWARD);
    if (r < 0) fail("{}: seek to packet {} failed: {}", path.string(), k, av_err(r));
    avcodec_flush_buffers(d.codec);
    auto out = std::make_shared<PerChannel>(channels);
    while (true) {
        r = av_read_frame(d.fmt, d.pkt);
        if (r < 0) {
            dec_.reset();
            fail("{}: packet {} not found after seeking: {}", path.string(), k, av_err(r));
        }
        if (d.pkt->stream_index != d.stream || d.pkt->pts < want.pts) {
            av_packet_unref(d.pkt);
            continue;
        }
        if (d.pkt->pts > want.pts) {
            av_packet_unref(d.pkt);
            dec_.reset();
            fail("{}: seeking overshot packet {}", path.string(), k);
        }
        try {
            d.decode_current(*out);
        } catch (...) {
            av_packet_unref(d.pkt);
            dec_.reset();
            throw;
        }
        av_packet_unref(d.pkt);
        break;
    }
    // a packet's samples may come out over several frames; the count is known from indexing
    if (k + 1 == packets_.size()) {
        avcodec_send_packet(d.codec, nullptr);
        while (avcodec_receive_frame(d.codec, d.frame) >= 0) d.take(*out);
        dec_.reset();  // the decoder is drained; reopen for the next read
    }
    WM_ENSURE((*out)[0].size() == want.count, "{}: packet {} decoded to {} samples, expected {}", path.string(), k, (*out)[0].size(), want.count);
    Samples v = out;
    recent_.emplace_back(k, v);
    if (recent_.size() > RECENT) recent_.erase(recent_.begin());
    return v;
}

std::vector<std::vector<int32_t>> AvImage::decode_range(uint64_t s, uint64_t e) const {
    WM_ENSURE(s < e && e <= total, "bad sample range {}..{}", s, e);
    PerChannel out(channels);
    for (auto& c : out) c.reserve(size_t(e - s));
    size_t k = size_t(std::upper_bound(packets_.begin(), packets_.end(), s, [](uint64_t v, const Packet& p) { return v < p.first; }) - packets_.begin()) - 1;
    for (; k < packets_.size() && packets_[k].first < e; k++) {
        const auto& pk = packets_[k];
        if (pk.count == 0) continue;
        auto smp = packet_samples(k);
        uint64_t lo = std::max(s, pk.first) - pk.first, hi = std::min(e, pk.first + pk.count) - pk.first;
        for (size_t c = 0; c < channels; c++) out[c].insert(out[c].end(), (*smp)[c].begin() + ptrdiff_t(lo), (*smp)[c].begin() + ptrdiff_t(hi));
    }
    WM_ENSURE(out[0].size() == e - s, "decoded {} of {} samples", out[0].size(), e - s);
    return out;
}

std::vector<std::array<uint8_t, 16>> AvImage::md5s(const std::vector<std::pair<uint64_t, uint64_t>>& ranges) const {
    std::vector<Md5> ctx(ranges.size());
    size_t bytes = (bps + 7) / 8;
    uint64_t pos = 0;
    Bytes pcm;
    Decoder d(path);
    d.decode_all([&](int64_t, const PerChannel& s) {
        if (s.empty()) return;
        uint64_t n = s[0].size(), fe = pos + n;
        for (size_t ri = 0; ri < ranges.size(); ri++) {
            auto [a, b] = ranges[ri];
            if (a >= fe || b <= pos) continue;
            size_t lo = size_t(std::max(a, pos) - pos), hi = size_t(std::min(b, fe) - pos);
            pcm.clear();
            for (size_t i = lo; i < hi; i++)
                for (auto& c : s) {
                    uint32_t v = uint32_t(c[i]);
                    for (size_t k = 0; k < bytes; k++) pcm.push_back(uint8_t(v >> (8 * k)));
                }
            ctx[ri].update(pcm);
        }
        pos = fe;
    });
    std::vector<std::array<uint8_t, 16>> out;
    for (auto& c : ctx) out.push_back(c.finish());
    return out;
}

}  // namespace wm
