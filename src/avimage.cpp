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

namespace wm {

namespace {

const char INDEX_MAGIC[8] = {'W', 'M', 'A', 'V', 'I', 'X', '0', '1'};
constexpr size_t MAX_DECODERS = 8;

std::string av_err(int e) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(e, buf, sizeof buf);
    return buf;
}

using PerChannel = std::vector<std::vector<int32_t>>;

struct AvFree {
    void operator()(AVFormatContext* f) const { avformat_close_input(&f); }
    void operator()(AVCodecContext* c) const { avcodec_free_context(&c); }
    void operator()(AVPacket* p) const { av_packet_free(&p); }
    void operator()(AVFrame* f) const { av_frame_free(&f); }
};
/// Owned libav objects: freed however a Decoder's construction ends.
template <class T>
using AvPtr = std::unique_ptr<T, AvFree>;

/// True if `packets` could describe a file of `total` samples: consecutive sample
/// ranges, and timestamps that increase (packets are found again by timestamp).
bool consistent(const std::vector<AvImage::Packet>& packets, uint64_t total) {
    uint64_t pos = 0;
    for (size_t i = 0; i < packets.size(); i++) {
        if (packets[i].first != pos || (i > 0 && packets[i].pts <= packets[i - 1].pts)) return false;
        pos += packets[i].count;
    }
    return pos == total && total > 0;
}

}  // namespace

/// A demuxer + decoder for one file.
struct AvImage::Decoder {
    AvPtr<AVFormatContext> fmt;
    AvPtr<AVCodecContext> codec;
    AvPtr<AVPacket> pkt{av_packet_alloc()};
    AvPtr<AVFrame> frame{av_frame_alloc()};
    int stream = -1;
    uint32_t bps = 0;

    explicit Decoder(const fs::path& p) {
        WM_ENSURE(pkt && frame, "out of memory");
        AVFormatContext* f = nullptr;
        int r = avformat_open_input(&f, p.c_str(), nullptr, nullptr);  // frees `f` itself on failure
        if (r < 0) fail("cannot open {}: {}", p.string(), av_err(r));
        fmt.reset(f);
        if ((r = avformat_find_stream_info(fmt.get(), nullptr)) < 0) fail("{}: {}", p.string(), av_err(r));
        const AVCodec* dec = nullptr;
        stream = av_find_best_stream(fmt.get(), AVMEDIA_TYPE_AUDIO, -1, -1, &dec, 0);
        WM_ENSURE(stream >= 0 && dec, "{}: no decodable audio stream", p.string());
        auto* par = fmt->streams[stream]->codecpar;
        codec.reset(avcodec_alloc_context3(dec));
        WM_ENSURE(codec, "out of memory");
        if ((r = avcodec_parameters_to_context(codec.get(), par)) < 0) fail("{}: {}", p.string(), av_err(r));
        codec->thread_count = 1;
        // checksums on, and any damage is an error: incomplete files must not pass
        codec->err_recognition = AV_EF_CRCCHECK | AV_EF_BITSTREAM | AV_EF_EXPLODE;
        if ((r = avcodec_open2(codec.get(), dec, nullptr)) < 0) fail("{}: {}", p.string(), av_err(r));
        switch (codec->sample_fmt) {
        case AV_SAMPLE_FMT_U8:
        case AV_SAMPLE_FMT_U8P: bps = 8; break;
        case AV_SAMPLE_FMT_S16:
        case AV_SAMPLE_FMT_S16P: bps = 16; break;
        case AV_SAMPLE_FMT_S32:
        case AV_SAMPLE_FMT_S32P:
            // left-justified in 32 bits; the significant bits, wherever libav says
            bps = par->bits_per_raw_sample > 16 && par->bits_per_raw_sample <= 32 ? uint32_t(par->bits_per_raw_sample)
                  : codec->bits_per_raw_sample > 16 && codec->bits_per_raw_sample <= 32 ? uint32_t(codec->bits_per_raw_sample)
                  : par->bits_per_coded_sample > 16 && par->bits_per_coded_sample <= 32 ? uint32_t(par->bits_per_coded_sample)
                  : 32;
            break;
        default: fail("{}: {} samples are not integer PCM", p.string(), av_get_sample_fmt_name(codec->sample_fmt));
        }
    }

    uint32_t channels() const { return uint32_t(codec->ch_layout.nb_channels); }

    /// Append the current frame's samples of type T, each made a sample by `conv`.
    template <class T, class Conv>
    void take_as(PerChannel& out, Conv conv) const {
        size_t ch = channels(), n = size_t(frame->nb_samples);
        bool planar = av_sample_fmt_is_planar(codec->sample_fmt);
        for (size_t c = 0; c < ch; c++) {
            // planar formats keep channels apart, packed ones interleave them
            const T* src = reinterpret_cast<const T*>(frame->extended_data[planar ? c : 0]) + (planar ? 0 : c);
            size_t step = planar ? 1 : ch;
            auto& o = out[c];
            size_t old = o.size();
            o.resize(old + n);
            for (size_t i = 0; i < n; i++) o[old + i] = conv(src[i * step]);
        }
    }

    /// Append the current frame's samples (per channel, as signed values of `bps` bits).
    void take(PerChannel& out) const {
        out.resize(channels());
        switch (av_get_packed_sample_fmt(codec->sample_fmt)) {
        case AV_SAMPLE_FMT_U8: take_as<uint8_t>(out, [](uint8_t v) { return int32_t(v) - 128; }); break;
        case AV_SAMPLE_FMT_S16: take_as<int16_t>(out, [](int16_t v) { return int32_t(v); }); break;
        default: {
            int shift = int(32 - bps);
            uint32_t low = 0;
            take_as<int32_t>(out, [&](int32_t v) {
                low |= uint32_t(v);
                return v >> shift;
            });
            // samples must be exact: bits below the ones kept would be lost
            WM_ENSURE((low & ((uint32_t(1) << shift) - 1)) == 0, "decoder output has more than the {} bits per sample it was taken for", bps);
        }
        }
    }

    /// Decode the packet in `pkt`; append everything it produces.
    void decode_current(PerChannel& out) {
        int r = avcodec_send_packet(codec.get(), pkt.get());
        if (r < 0) fail("decode error: {}", av_err(r));
        receive(out);
    }

    /// Append the frames the decoder has ready.
    void receive(PerChannel& out) {
        int r;
        while ((r = avcodec_receive_frame(codec.get(), frame.get())) >= 0) take(out);
        if (r != AVERROR(EAGAIN) && r != AVERROR_EOF) fail("decode error: {}", av_err(r));
    }

    /// Append what the decoder still holds at the end of the stream (lossless
    /// decoders have no delay, but be exact anyway). Seek and flush to reuse it.
    void drain(PerChannel& out) {
        int r = avcodec_send_packet(codec.get(), nullptr);
        if (r < 0 && r != AVERROR_EOF) fail("decode error: {}", av_err(r));
        receive(out);
    }

    /// Decode the whole file from the start: sink(packet pts, samples of that packet).
    void decode_all(const std::function<void(int64_t, const PerChannel&)>& sink) {
        PerChannel buf;
        int r;
        while ((r = av_read_frame(fmt.get(), pkt.get())) >= 0) {
            if (pkt->stream_index == stream) {
                for (auto& c : buf) c.clear();
                int64_t pts = pkt->pts;
                decode_current(buf);
                sink(pts, buf);
            }
            av_packet_unref(pkt.get());
        }
        if (r != AVERROR_EOF) fail("read error: {}", av_err(r));
        for (auto& c : buf) c.clear();
        drain(buf);
        if (!buf.empty() && !buf[0].empty()) sink(AV_NOPTS_VALUE, buf);
    }
};

AvImage::~AvImage() = default;

std::shared_ptr<AvImage> AvImage::open(const fs::path& p, const Cache* cache) {
    auto key = SrcKey::of(p);
    auto img = std::make_shared<AvImage>();
    img->path = p;
    auto d = std::make_unique<Decoder>(p);
    img->sample_rate = uint32_t(d->codec->sample_rate);
    img->channels = d->channels();
    img->bps = d->bps;
    const AVDictionaryEntry* e = nullptr;
    while ((e = av_dict_iterate(d->fmt->metadata, e))) img->tags.add(e->key, e->value);
    if (cache) img->index_file_ = cache->av_index_path(p, key);
    if (cache)
        if (auto b = try_read_file(img->index_file_); b && b->size() >= 32 && std::memcmp(b->data(), INDEX_MAGIC, 8) == 0) {
            uint64_t n = le64(&(*b)[24]);
            if (b->size() == 32 + n * 20 && le32(&(*b)[8]) == img->sample_rate && (*b)[12] == img->channels && (*b)[13] == img->bps) {
                img->total = le64(&(*b)[16]);
                img->packets_.resize(size_t(n));
                for (size_t i = 0; i < n; i++) {
                    const uint8_t* q = &(*b)[32 + 20 * i];
                    img->packets_[i] = {int64_t(le64(q)), le64(q + 8), le32(q + 16)};
                }
                // (indexes stored before timestamps were checked are re-checked here)
                // (no decoder is kept for reads: images are held long before
                // they are read, and only a read lets idle decoders be trimmed)
                if (consistent(img->packets_, img->total)) return img;
                img->packets_.clear();
            }
        }
    // verify + index: one full decode with checksums (by the decoder that probed)
    uint64_t pos = 0;
    with_context(p.string(), [&] {
        d->decode_all([&](int64_t pts, const PerChannel& s) {
            uint32_t n = s.empty() ? 0 : uint32_t(s[0].size());
            if (pts == AV_NOPTS_VALUE && !img->packets_.empty()) {
                img->packets_.back().count += n;  // trailing flush output belongs to the last packet
            } else {
                WM_ENSURE(pts != AV_NOPTS_VALUE, "packet without timestamp");
                // packets are found again by timestamp, so equal ones would be ambiguous
                WM_ENSURE(img->packets_.empty() || pts > img->packets_.back().pts, "packet timestamps do not increase ({} after {})", pts,
                          img->packets_.back().pts);
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
        atomic_write(img->index_file_, b);
    }
    return img;
}

std::unique_ptr<AvImage::Decoder> AvImage::take_decoder() const {
    {
        std::lock_guard g(mu_);
        if (!idle_.empty()) {
            auto d = std::move(idle_.back());
            idle_.pop_back();
            return d;
        }
    }
    return std::make_unique<Decoder>(path);
}

void AvImage::give_decoder(std::unique_ptr<Decoder> d) const {
    std::lock_guard g(mu_);
    if (idle_.size() < MAX_DECODERS) idle_.push_back(std::move(d));
}

void AvImage::index_mismatch(size_t k, const std::string& what) const {
    // The demuxer no longer splits the file the way the index says (e.g. after a
    // libavformat upgrade): every read of this packet would fail the same way.
    std::string msg = std::format("{}: packet {} {}", path.string(), k, what);
    if (index_file_.empty()) fail("{}", msg);
    std::error_code ec;
    fs::remove(index_file_, ec);
    fail("{}: removed the stale index {}; the image is indexed again when its folder is listed anew (after a restart at the latest)", msg,
         index_file_.string());
}

AvImage::Samples AvImage::decode_packet(size_t k) const {
    auto d = take_decoder();
    const auto& want = packets_[k];
    int r = av_seek_frame(d->fmt.get(), d->stream, want.pts, AVSEEK_FLAG_BACKWARD);
    if (r < 0) fail("{}: seek to packet {} failed: {}", path.string(), k, av_err(r));
    avcodec_flush_buffers(d->codec.get());  // also what lets a drained decoder decode again
    auto out = std::make_shared<PerChannel>(channels);
    while (true) {
        r = av_read_frame(d->fmt.get(), d->pkt.get());
        if (r < 0) fail("{}: packet {} not found after seeking: {}", path.string(), k, av_err(r));
        if (d->pkt->stream_index != d->stream || d->pkt->pts < want.pts) {
            av_packet_unref(d->pkt.get());
            continue;
        }
        if (d->pkt->pts > want.pts) {
            av_packet_unref(d->pkt.get());
            index_mismatch(k, "is not where the index says (seeking overshot it)");
        }
        try {
            d->decode_current(*out);
        } catch (...) {
            av_packet_unref(d->pkt.get());
            throw;  // the decoder is dropped: its state is unknown
        }
        av_packet_unref(d->pkt.get());
        break;
    }
    if (k + 1 == packets_.size()) d->drain(*out);  // the last packet's samples may need a flush
    if ((*out)[0].size() != want.count) index_mismatch(k, std::format("decoded to {} samples, the index says {}", (*out)[0].size(), want.count));
    give_decoder(std::move(d));
    return out;
}

AvImage::Packets::Future AvImage::start(size_t k, Packets::Promise* claimed) const {
    Packets::Promise promise;
    auto f = cache_.get(k, packets_[k].count, &promise);
    if (!promise) return f;  // cached or in flight
    cache_.trim([k](size_t j) { return j == k; });
    if (claimed) {
        *claimed = promise;  // the caller decodes it itself
        return f;
    }
    submit([this, self = shared_from_this(), k, promise] { fulfil(k, *promise); });
    return f;
}

void AvImage::drop_cached() const {
    std::vector<std::unique_ptr<Decoder>> drop;  // closed outside the lock
    std::lock_guard g(mu_);
    cache_.drop_ready();
    drop.swap(idle_);
    last_packet_ = SIZE_MAX;
}

void AvImage::fulfil(size_t k, std::promise<Samples>& promise) const {
    try {
        promise.set_value(decode_packet(k));
    } catch (...) {
        promise.set_exception(std::current_exception());
        std::lock_guard g(mu_);
        cache_.forget(k, &promise);  // let a later read retry
    }
}

void AvImage::prefetch_after(size_t k) const {
    if (weak_from_this().expired()) return;  // not shared: no background work
    std::lock_guard g(mu_);
    uint64_t ahead = 0;
    for (size_t j = k + 1; j < packets_.size() && ahead < READ_AHEAD_SAMPLES; j++) {
        ahead += packets_[j].count;
        start(j, nullptr);
    }
}

AvImage::Samples AvImage::packet_samples(size_t k) const {
    // the packet being read is decoded right here if nobody has started it (pool
    // threads only ever do read-ahead, so they never wait on each other)
    Packets::Future f;
    Packets::Promise mine;
    bool sequential;
    touch(weak_from_this().lock());
    {
        std::lock_guard g(mu_);
        sequential = last_packet_ != SIZE_MAX && (k == last_packet_ || k == last_packet_ + 1);
        last_packet_ = k;
        f = start(k, &mine);
    }
    if (sequential) prefetch_after(k);  // a reader moving forward: decode ahead in parallel
    if (mine) fulfil(k, *mine);
    return f.get();
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

}  // namespace wm
