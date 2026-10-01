// SPDX-License-Identifier: GPL-3.0-or-later
// FLAC primitives and sample-exact track splitting, checked against the reference
// `flac` tool.
#include <unistd.h>

#include <cmath>

#include "flac.hpp"
#include "md5.hpp"
#include "test.hpp"
#include "track.hpp"

using namespace wm;

TEST(md5_known_values) {
    Md5 a;
    CHECK(hex(a.finish()) == "d41d8cd98f00b204e9800998ecf8427e");
    Md5 b;
    b.update(std::string_view("The quick brown fox jumps over the lazy dog"));
    CHECK(hex(b.finish()) == "9e107d9d372bb6826bd81d3542a419d6");
    Md5 c;  // split updates across a block boundary
    std::string s(1000, 'x');
    c.update(std::string_view(s).substr(0, 63));
    c.update(std::string_view(s).substr(63));
    Md5 d;
    d.update(s);
    CHECK(c.finish() == d.finish());
}

TEST(number_roundtrip) {
    for (uint64_t v : {0ull, 1ull, 0x7Full, 0x80ull, 0x7FFull, 0x800ull, 0xFFFFull, 0x10000ull, 0x1FFFFFull, 0x200000ull, 0x3FFFFFFull, 0x4000000ull,
                       0x7FFFFFFFull, 0x80000000ull, 0xFFFFFFFFFull}) {
        Bytes b;
        flac::encode_number(v, b);
        CHECK(b.size() == flac::coded_len(v));
        auto d = flac::decode_number(b);
        CHECK(d && d->first == v && d->second == b.size());
    }
}

TEST(crc_known_values) {
    // CRC-16/BUYPASS("123456789") = 0xFEE8, CRC-8/SMBUS = 0xF4
    CHECK(flac::crc16(bytes_of("123456789")) == 0xFEE8);
    CHECK(flac::crc8(bytes_of("123456789")) == 0xF4);
}

TEST(verbatim_roundtrip) {
    for (auto [bps, n] : std::vector<std::pair<uint32_t, uint32_t>>{{16, 1}, {16, 17}, {16, 300}, {24, 4096}, {8, 5}, {24, 65535}}) {
        int64_t max = int64_t(1) << (bps - 1);
        std::vector<int32_t> l(n), r(n);
        for (uint32_t i = 0; i < n; i++) {
            l[i] = int32_t((int64_t(i) * 7919) % (2 * max) - max);
            r[i] = int32_t(-int64_t(l[i]) - 1);
        }
        auto fr = flac::encode_verbatim({l, r}, bps, 123456, 9, {});
        CHECK_MSG(fr.size() == flac::verbatim_size(n, 2, bps, 123456, 0), "size bps={} n={}", bps, n);
        auto h = flac::FrameHeader::parse(fr);
        CHECK(h && h->variable && h->number == 123456 && h->block_size == n);
        CHECK(flac::crc16(fr) == 0);
        auto dec = flac::decode_frame(fr, bps, 44100);
        CHECK(dec.size() == 2 && dec[0] == l && dec[1] == r);
    }
}

namespace {

/// Deterministic, somewhat compressible test signal.
std::vector<std::vector<int32_t>> signal(size_t n, size_t ch, uint32_t bps) {
    double amp = double((int64_t(1) << (bps - 1)) - 1) * 0.6;
    uint64_t seed = 12345;
    std::vector<std::vector<int32_t>> out(ch, std::vector<int32_t>(n));
    for (size_t c = 0; c < ch; c++)
        for (size_t i = 0; i < n; i++) {
            seed = seed * 6364136223846793005ull + 1442695040888963407ull;
            double noise = (double(seed >> 33) / double(1ull << 31) - 0.5) * amp * 0.1;
            double t = double(i) / 44100.0;
            out[c][i] = int32_t(amp * 0.8 * std::sin(t * 440.0 * double(c + 1) * 2 * M_PI) + noise);
        }
    return out;
}

Bytes raw_pcm(const std::vector<std::vector<int32_t>>& chans, uint32_t bps) {
    size_t bytes = bps / 8;
    Bytes raw;
    for (size_t i = 0; i < chans[0].size(); i++)
        for (auto& c : chans)
            for (size_t k = 0; k < bytes; k++) raw.push_back(uint8_t(uint32_t(c[i]) >> (8 * k)));
    return raw;
}

void run_case(uint32_t rate, size_t ch, uint32_t bps, uint32_t bs, size_t total, const std::vector<std::pair<uint64_t, uint64_t>>& ranges) {
    fs::path dir = fs::temp_directory_path() / std::format("wm-test-{}-{}-{}", ::getpid(), rate, bs);
    fs::create_directories(dir);
    fs::path raw = dir / "in.raw", img = dir / "img.flac";
    auto sig = signal(total, ch, bps);
    Bytes all = raw_pcm(sig, bps);
    atomic_write(raw, all);
    auto st = run({"flac", "-s", "-f", "--force-raw-format", "--endian=little", "--sign=signed", std::format("--channels={}", ch),
                   std::format("--bps={}", bps), std::format("--sample-rate={}", rate), std::format("--blocksize={}", bs), "-o", img.string(),
                   raw.string()});
    CHECK_MSG(st.ok(), "flac encode: {}", st.err);
    auto meta = flac::FlacMeta::read(img);
    auto idx = flac::FrameIndex::build(img, meta);
    CHECK(idx.block_size == bs);
    auto image = FlacImage::open(img, meta, idx);
    size_t frame_bytes = bps / 8 * ch;
    for (auto [s, e] : ranges) {
        Tags tags;
        tags.set("TITLE", std::format("{}-{}", s, e));
        auto md5 = track_md5s(*image, {{s, e}})[0];
        FlacTrack tr(image, s, e, tags, {}, md5);
        Bytes bytes = tr.read_at(0, size_t(tr.size()));
        CHECK(bytes.size() == tr.size());
        fs::path out = dir / "t.flac";
        atomic_write(out, bytes);
        auto t = run({"flac", "-t", "-s", out.string()});
        CHECK_MSG(t.ok(), "flac -t failed for {}/{}ch/{}bit/bs{} range {}..{}: {}", rate, ch, bps, bs, s, e, t.err);
        auto d = run({"flac", "-d", "-c", "-s", "--force-raw-format", "--endian=little", "--sign=signed", out.string()});
        CHECK(d.ok());
        std::string_view want = str_of(std::span(all).subspan(size_t(s) * frame_bytes, size_t(e - s) * frame_bytes));
        CHECK_MSG(d.out == want, "PCM mismatch for {}/{}ch/{}bit/bs{} range {}..{}", rate, ch, bps, bs, s, e);
        // random-access reads agree with the whole-file read
        for (uint64_t off : {uint64_t(0), uint64_t(41), tr.size() / 3, tr.size() - 7}) {
            Bytes part = tr.read_at(off, 5000);
            CHECK(std::equal(part.begin(), part.end(), bytes.begin() + ptrdiff_t(off)));
        }
    }
    fs::remove_all(dir);
}

std::vector<std::pair<uint64_t, uint64_t>> edge_ranges(uint64_t bs, uint64_t total) {
    return {
        {0, total},              // whole image
        {0, bs},                 // exactly one frame
        {bs, 3 * bs},            // aligned both ends
        {1, bs + 1},             // off by one
        {bs - 1, 3 * bs + 1},    // 1-sample head (merged), 1-sample tail (merged)
        {bs - 15, 3 * bs + 15},  // 15-sample head/tail (merged)
        {bs - 16, 3 * bs + 16},  // 16-sample head/tail (not merged)
        {bs + 7, bs + 12},       // 5 samples inside one frame
        {bs - 3, bs + 3},        // 6 samples straddling a boundary
        {2 * bs + 100, total},   // to the end (tiny last frame)
        {total - 5, total},      // last 5 samples only
        {total - bs - 2, total}, // tail merge near the end
    };
}

}  // namespace

TEST(split_cd_audio_4096) {
    size_t total = 4096 * 20 + 3;  // tiny last frame
    run_case(44100, 2, 16, 4096, total, edge_ranges(4096, total));
}

TEST(split_odd_blocksize_24bit) {
    size_t total = 1152 * 30 + 1000;
    run_case(48000, 2, 24, 1152, total, edge_ranges(1152, total));
}

TEST(split_mono_hires_4608) {
    size_t total = 4608 * 12 + 17;
    run_case(96000, 1, 24, 4608, total, edge_ranges(4608, total));
}

TEST(split_multichannel_8bit) {
    size_t total = 576 * 40 + 9;
    run_case(22050, 6, 8, 576, total, edge_ranges(576, total));
}

// ---- decoded (non-FLAC) images, tracks encoded on the fly

#include "avimage.hpp"
#include "encoded.hpp"

namespace {

void run_decoded_case(const char* codec, const char* ext, uint32_t rate, size_t ch, uint32_t bps, size_t total) {
    fs::path dir = fs::temp_directory_path() / std::format("wm-test-av-{}-{}", ::getpid(), codec);
    fs::create_directories(dir);
    fs::path raw = dir / "in.raw", img = dir / (std::string("img.") + ext);
    auto sig = signal(total, ch, bps);
    Bytes all = raw_pcm(sig, bps);
    atomic_write(raw, all);
    auto st = run({"ffmpeg", "-v", "error", "-y", "-f", bps == 16 ? "s16le" : "s24le", "-ar", std::to_string(rate), "-ac", std::to_string(ch), "-i",
                   raw.string(), "-c:a", codec, img.string()});
    CHECK_MSG(st.ok(), "ffmpeg encode: {}", st.err);
    auto image = AvImage::open(img, nullptr);
    CHECK(image->total == total && image->channels == ch && image->bps == bps && image->sample_rate == rate);
    size_t frame_bytes = bps / 8 * ch;
    uint64_t bs = 4096;
    Ranges ranges = {{0, total}, {1, bs + 1}, {bs - 3, bs + 3}, {12345, total - 777}, {total - 5, total}};
    auto layout = build_layout(*image, ranges, nullptr);
    for (size_t i = 0; i < ranges.size(); i++) {
        auto [s, e] = ranges[i];
        Tags tags;
        tags.set("TITLE", std::format("{}-{}", s, e));
        EncodedTrack tr(image, s, e, tags, layout[i]);
        Bytes bytes = tr.read_at(0, size_t(tr.size()));
        CHECK(bytes.size() == tr.size());
        fs::path out = dir / "t.flac";
        atomic_write(out, bytes);
        auto t = run({"flac", "-t", "-s", out.string()});
        CHECK_MSG(t.ok(), "flac -t failed for {} range {}..{}: {}", codec, s, e, t.err);
        auto d = run({"flac", "-d", "-c", "-s", "--force-raw-format", "--endian=little", "--sign=signed", out.string()});
        std::string_view want = str_of(std::span(all).subspan(size_t(s) * frame_bytes, size_t(e - s) * frame_bytes));
        CHECK_MSG(d.out == want, "PCM mismatch for {} range {}..{}", codec, s, e);
        // compressed, and random access reproduces the same bytes
        if (e - s > 100000) CHECK(tr.size() < (e - s) * frame_bytes * 9 / 10);
        for (uint64_t off : {uint64_t(7), tr.size() / 2, tr.size() - 3000}) {
            EncodedTrack fresh(image, s, e, tags, layout[i]);
            Bytes part = fresh.read_at(off, 9000);
            CHECK(std::equal(part.begin(), part.end(), bytes.begin() + ptrdiff_t(off)));
        }
    }
    fs::remove_all(dir);
}

}  // namespace

TEST(decoded_wavpack_16bit) { run_decoded_case("wavpack", "wv", 44100, 2, 16, 44100 * 7 + 1234); }
TEST(decoded_tta_24bit) { run_decoded_case("tta", "tta", 96000, 2, 24, 96000 * 5 + 99); }
