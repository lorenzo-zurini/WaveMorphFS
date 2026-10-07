// SPDX-License-Identifier: GPL-3.0-or-later
#include "cover.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

#include <cstring>

#include "md5.hpp"

namespace wm {

namespace {

std::optional<Cover> probe(const fs::path& p, uint64_t max_bytes) {
    auto m = mtime_ns(p);
    std::error_code ec;
    auto size = fs::file_size(p, ec);
    if (!m || ec) {
        warn("cover {}: not found", p.string());
        return std::nullopt;
    }
    if (size > max_bytes) {
        warn("cover {}: {} bytes, more than the {} allowed", p.string(), size, max_bytes);
        return std::nullopt;
    }
    Bytes h;
    try {
        File f(p);
        h = f.read_vec(0, size_t(std::min<uint64_t>(size, 65536)));
    } catch (const std::exception& e) {
        warn("cover {}: {}", p.string(), e.what());
        return std::nullopt;
    }
    Cover c;
    c.path = std::make_shared<const fs::path>(p);
    c.size = size;
    c.mtime = *m;
    if (h.size() >= 26 && std::memcmp(h.data(), "\x89PNG\r\n\x1a\n", 8) == 0 && std::memcmp(&h[12], "IHDR", 4) == 0) {
        c.mime = "image/png";
        c.width = be32(&h[16]);
        c.height = be32(&h[20]);
        static const uint32_t channels[] = {1, 0, 3, 1, 2, 0, 4};
        uint8_t ct = h[25];
        c.depth = h[24] * (ct < 7 ? channels[ct] : 0);
        return c;
    }
    if (h.size() >= 4 && h[0] == 0xFF && h[1] == 0xD8) {
        c.mime = "image/jpeg";
        // walk the markers to the frame header (SOFn) for the dimensions
        for (size_t i = 2; i + 9 < h.size();) {
            if (h[i] != 0xFF) break;
            uint8_t mk = h[i + 1];
            if (mk == 0xFF) {
                i++;
                continue;
            }
            uint16_t len = uint16_t(h[i + 2] << 8 | h[i + 3]);
            if (mk >= 0xC0 && mk <= 0xCF && mk != 0xC4 && mk != 0xC8 && mk != 0xCC) {
                c.height = uint32_t(h[i + 5] << 8 | h[i + 6]);
                c.width = uint32_t(h[i + 7] << 8 | h[i + 8]);
                c.depth = uint32_t(h[i + 4]) * h[i + 9];
                break;
            }
            i += 2 + len;
        }
        return c;
    }
    warn("cover {}: not a JPEG or PNG image", p.string());
    return std::nullopt;
}

/// Decode the first picture of `src`, scale it to fit COVER_EDGE and encode it as
/// JPEG (single-threaded, fixed quality: the same input gives the same bytes).
Bytes scale_to_jpeg(const fs::path& src) {
    AVFormatContext* fmt = nullptr;
    WM_ENSURE(avformat_open_input(&fmt, src.c_str(), nullptr, nullptr) == 0, "cannot open");
    std::unique_ptr<AVFormatContext, void (*)(AVFormatContext*)> fmt_guard(fmt, [](AVFormatContext* f) { avformat_close_input(&f); });
    WM_ENSURE(avformat_find_stream_info(fmt, nullptr) >= 0 && fmt->nb_streams > 0, "no image stream");
    auto* par = fmt->streams[0]->codecpar;
    const AVCodec* dec = avcodec_find_decoder(par->codec_id);
    WM_ENSURE(dec, "no decoder");
    using Ctx = std::unique_ptr<AVCodecContext, void (*)(AVCodecContext*)>;
    auto free_ctx = [](AVCodecContext* c) { avcodec_free_context(&c); };
    Ctx dctx(avcodec_alloc_context3(dec), free_ctx);
    avcodec_parameters_to_context(dctx.get(), par);
    dctx->thread_count = 1;
    WM_ENSURE(avcodec_open2(dctx.get(), dec, nullptr) == 0, "cannot open decoder");
    std::unique_ptr<AVFrame, void (*)(AVFrame*)> in(av_frame_alloc(), [](AVFrame* f) { av_frame_free(&f); });
    std::unique_ptr<AVPacket, void (*)(AVPacket*)> pkt(av_packet_alloc(), [](AVPacket* p) { av_packet_free(&p); });
    bool got = false;
    while (!got && av_read_frame(fmt, pkt.get()) >= 0) {
        if (avcodec_send_packet(dctx.get(), pkt.get()) == 0) got = avcodec_receive_frame(dctx.get(), in.get()) == 0;
        av_packet_unref(pkt.get());
    }
    if (!got) {
        avcodec_send_packet(dctx.get(), nullptr);
        got = avcodec_receive_frame(dctx.get(), in.get()) == 0;
    }
    WM_ENSURE(got && in->width > 0 && in->height > 0, "cannot decode");
    int w = in->width, h = in->height;
    double f = double(COVER_EDGE) / std::max(w, h);
    int dw = std::max(2, int(w * f + 0.5) & ~1), dh = std::max(2, int(h * f + 0.5) & ~1);

    const AVCodec* enc = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    WM_ENSURE(enc, "no JPEG encoder");
    Ctx ectx(avcodec_alloc_context3(enc), free_ctx);
    ectx->width = dw;
    ectx->height = dh;
    ectx->pix_fmt = AV_PIX_FMT_YUV420P;
    ectx->color_range = AVCOL_RANGE_JPEG;
    ectx->time_base = {1, 25};
    ectx->thread_count = 1;
    ectx->flags |= AV_CODEC_FLAG_QSCALE;
    ectx->global_quality = FF_QP2LAMBDA * 2;
    WM_ENSURE(avcodec_open2(ectx.get(), enc, nullptr) == 0, "cannot open JPEG encoder");
    std::unique_ptr<AVFrame, void (*)(AVFrame*)> out(av_frame_alloc(), [](AVFrame* fr) { av_frame_free(&fr); });
    out->format = AV_PIX_FMT_YUV420P;
    out->width = dw;
    out->height = dh;
    out->color_range = AVCOL_RANGE_JPEG;
    WM_ENSURE(av_frame_get_buffer(out.get(), 0) == 0, "frame buffer");
    // the old full-range "J" formats are the plain ones with JPEG range
    AVPixelFormat src_fmt = AVPixelFormat(in->format);
    bool full = in->color_range == AVCOL_RANGE_JPEG;
    switch (src_fmt) {
    case AV_PIX_FMT_YUVJ420P: src_fmt = AV_PIX_FMT_YUV420P, full = true; break;
    case AV_PIX_FMT_YUVJ422P: src_fmt = AV_PIX_FMT_YUV422P, full = true; break;
    case AV_PIX_FMT_YUVJ444P: src_fmt = AV_PIX_FMT_YUV444P, full = true; break;
    case AV_PIX_FMT_YUVJ440P: src_fmt = AV_PIX_FMT_YUV440P, full = true; break;
    case AV_PIX_FMT_YUVJ411P: src_fmt = AV_PIX_FMT_YUV411P, full = true; break;
    default: break;
    }
    if (dctx->codec_id == AV_CODEC_ID_MJPEG) full = true;  // JPEG is full range unless it says otherwise
    SwsContext* sws = sws_getContext(w, h, src_fmt, dw, dh, AV_PIX_FMT_YUV420P, SWS_LANCZOS, nullptr, nullptr, nullptr);
    WM_ENSURE(sws, "cannot scale");
    const int* coef = sws_getCoefficients(SWS_CS_DEFAULT);
    sws_setColorspaceDetails(sws, coef, full ? 1 : 0, coef, 1, 0, 1 << 16, 1 << 16);
    sws_scale(sws, in->data, in->linesize, 0, h, out->data, out->linesize);
    sws_freeContext(sws);
    out->quality = ectx->global_quality;
    out->pts = 0;
    WM_ENSURE(avcodec_send_frame(ectx.get(), out.get()) == 0, "encode");
    avcodec_send_frame(ectx.get(), nullptr);
    Bytes jpeg;
    while (avcodec_receive_packet(ectx.get(), pkt.get()) == 0) {
        jpeg.insert(jpeg.end(), pkt->data, pkt->data + pkt->size);
        av_packet_unref(pkt.get());
    }
    WM_ENSURE(!jpeg.empty(), "encoder produced nothing");
    return jpeg;
}

}  // namespace

std::optional<Cover> load_cover(const fs::path& p) { return probe(p, MAX_COVER_BYTES); }

std::optional<Cover> prepare_cover(const fs::path& src, const fs::path& cache_dir) {
    auto c = probe(src, MAX_COVER_SOURCE_BYTES);
    if (!c) return std::nullopt;
    if (c->size <= MAX_COVER_BYTES && c->width && c->height && std::max(c->width, c->height) <= COVER_EDGE) return c;
    // scaled once per version of the source image
    Md5 h;
    h.update(std::format("{}\n{}\n{}\n{}", src.string(), c->size, c->mtime, COVER_EDGE));
    auto d = h.finish();
    fs::path scaled = cache_dir / "covers" / (hex(std::span(d).subspan(0, 8)) + ".jpg");
    if (!fs::exists(scaled)) {
        try {
            Bytes jpeg = scale_to_jpeg(src);
            fs::create_directories(scaled.parent_path());
            atomic_write(scaled, jpeg);
            info("cover {}: scaled to {} px ({} KB)", src.string(), COVER_EDGE, jpeg.size() / 1024);
        } catch (const std::exception& e) {
            warn("cover {}: scaling failed: {}", src.string(), e.what());
            return std::nullopt;
        }
    }
    return load_cover(scaled);
}

std::vector<Seg> flac_picture(const Cover& c) {
    Bytes b;
    put_be32(b, 3);  // front cover
    put_be32(b, uint32_t(c.mime.size()));
    append(b, c.mime);
    put_be32(b, 0);  // description
    put_be32(b, c.width);
    put_be32(b, c.height);
    put_be32(b, c.depth);
    put_be32(b, 0);  // colours (indexed images only)
    put_be32(b, uint32_t(c.size));
    return {std::move(b), FileRange{c.path, 0, c.size}};
}

std::vector<Seg> id3_apic(const Cover& c) {
    Bytes b = {0};  // ISO-8859-1 text
    append(b, c.mime);
    b.push_back(0);
    b.push_back(3);  // front cover
    b.push_back(0);  // empty description
    return {std::move(b), FileRange{c.path, 0, c.size}};
}

}  // namespace wm
