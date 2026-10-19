// SPDX-License-Identifier: GPL-3.0-or-later
// Edge cases of the audio paths (frame index, split tracks, decoded images,
// on-the-fly encoding) and the helpers they share.
#include <unistd.h>

#include <cmath>
#include <cstring>

#include "flac.hpp"
#include "test.hpp"
#include "track.hpp"

using namespace wm;

namespace {

/// A scratch directory removed when the test ends.
struct TmpDir {
    fs::path path;
    explicit TmpDir(std::string_view name) : path(fs::temp_directory_path() / std::format("wm-audio-{}-{}", ::getpid(), name)) {
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TmpDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

/// Deterministic, somewhat compressible test signal.
std::vector<std::vector<int32_t>> signal(size_t n, size_t ch, uint32_t bps) {
    double amp = double((int64_t(1) << (bps - 1)) - 1) * 0.6;
    uint64_t seed = 99;
    std::vector<std::vector<int32_t>> out(ch, std::vector<int32_t>(n));
    for (size_t c = 0; c < ch; c++)
        for (size_t i = 0; i < n; i++) {
            seed = seed * 6364136223846793005ull + 1442695040888963407ull;
            double noise = (double(seed >> 33) / double(1ull << 31) - 0.5) * amp * 0.1;
            double t = double(i) / 44100.0;
            out[c][i] = int32_t(amp * 0.8 * std::sin(t * 330.0 * double(c + 1) * 2 * M_PI) + noise);
        }
    return out;
}

/// Interleaved little-endian PCM, bps/8 bytes per sample.
Bytes raw_pcm(const std::vector<std::vector<int32_t>>& chans, uint32_t bps) {
    Bytes raw;
    for (size_t i = 0; i < chans[0].size(); i++)
        for (auto& c : chans)
            for (size_t k = 0; k < bps / 8; k++) raw.push_back(uint8_t(uint32_t(c[i]) >> (8 * k)));
    return raw;
}

/// A FLAC image made by the reference encoder; returns its PCM.
Bytes make_flac(const fs::path& out, uint32_t rate, size_t ch, uint32_t bps, uint32_t bs, size_t total) {
    Bytes pcm = raw_pcm(signal(total, ch, bps), bps);
    fs::path raw = out.parent_path() / "in.raw";
    atomic_write(raw, pcm);
    auto st = run({"flac", "-s", "-f", "--force-raw-format", "--endian=little", "--sign=signed", std::format("--channels={}", ch), std::format("--bps={}", bps),
                   std::format("--sample-rate={}", rate), std::format("--blocksize={}", bs), "--no-padding", "-o", out.string(), raw.string()});
    CHECK_MSG(st.ok(), "flac encode: {}", st.err);
    return pcm;
}

/// `bytes` is a valid FLAC file (flac -t) decoding to `want` (bps/8 bytes per sample).
void check_flac(const fs::path& dir, const Bytes& bytes, std::span<const uint8_t> want, std::string_view what) {
    fs::path out = dir / "t.flac";
    atomic_write(out, bytes);
    auto t = run({"flac", "-t", "-s", out.string()});
    CHECK_MSG(t.ok(), "flac -t failed for {}: {}", what, t.err);
    auto d = run({"flac", "-d", "-c", "-s", "--force-raw-format", "--endian=little", "--sign=signed", out.string()});
    CHECK_MSG(d.ok() && d.out == str_of(want), "PCM mismatch for {}", what);
}

Bytes whole(const VFile& v) { return v.read_at(0, size_t(v.size())); }

}  // namespace

// ---- frame index

TEST(index_last_frame_not_extended_into_a_tag_with_zero_crc) {
    TmpDir dir("zerocrc");
    fs::path img = dir.path / "img.flac";
    Bytes pcm = make_flac(img, 44100, 2, 16, 4096, 4096 * 5 + 300);
    uint64_t audio_end = fs::file_size(img);
    // a trailing ID3v2 tag whose own CRC-16 is 0: EOF then also "ends" the last frame
    Bytes tag = {'I', 'D', '3', 4, 0, 0, 0, 0, 0, 16};
    tag.resize(24, 0);
    put_be16(tag, flac::crc16(tag));
    CHECK(flac::crc16(tag) == 0);
    Bytes file = read_file(img);
    append(file, tag);
    atomic_write(img, file);

    auto meta = flac::FlacMeta::read(img);
    auto idx = flac::FrameIndex::build(img, meta);
    CHECK(idx.offsets.back() == audio_end);
    auto image = FlacImage::open(img, meta, idx);
    uint64_t total = image->total();
    FlacTrack tr(image, 1000, total, Tags{}, {}, std::nullopt);
    check_flac(dir.path, whole(tr), std::span(pcm).subspan(1000 * 4), "track to the end");

    // an index cached by the old search (ending at EOF) is corrected on open
    std::vector<uint64_t> stale(idx.offsets.begin(), idx.offsets.end());
    stale.back() = file.size();
    flac::FrameIndex old = idx;
    old.offsets = stale;
    old.storage = nullptr;
    CHECK(FlacImage::open(img, meta, old)->index.offsets.back() == audio_end);
}

TEST(index_accepts_headerless_ape_tag_and_id3v1) {
    TmpDir dir("apetag");
    fs::path img = dir.path / "img.flac";
    make_flac(img, 44100, 1, 16, 1152, 1152 * 7 + 5);
    uint64_t audio_end = fs::file_size(img);
    // APEv2 items + footer only (the header is optional), then an ID3v1 tag
    Bytes item;
    put_le32(item, 5);
    put_le32(item, 0);
    append(item, std::string_view("Title\0Hello", 11));
    Bytes footer(std::begin("APETAGEX"), std::end("APETAGEX") - 1);
    put_le32(footer, 2000);
    put_le32(footer, uint32_t(item.size() + 32));
    put_le32(footer, 1);
    put_le32(footer, 0);
    footer.resize(32, 0);
    Bytes v1(128, 0);
    std::memcpy(v1.data(), "TAG", 3);
    Bytes file = read_file(img);
    append(file, item);
    append(file, footer);
    append(file, v1);
    atomic_write(img, file);
    auto idx = flac::FrameIndex::build(img, flac::FlacMeta::read(img));
    CHECK(idx.offsets.back() == audio_end);
}

// ---- whole-file decoding (track MD5s)

TEST(decode_file_fails_on_a_bad_frame_before_the_end) {
    TmpDir dir("badframe");
    fs::path img = dir.path / "img.flac";
    size_t total = 4096 * 8;
    make_flac(img, 44100, 2, 16, 4096, total);
    auto count = [&](const fs::path& p) {
        uint64_t n = 0;
        flac::decode_file(p, total, [&](uint64_t, const std::vector<std::span<const int32_t>>& ch) { n += ch[0].size(); });
        return n;
    };
    // junk after the audio is ignored
    Bytes file = read_file(img);
    Bytes junk = file;
    for (int i = 0; i < 300; i++) junk.push_back(uint8_t(i * 37 + 11));
    fs::path tail = dir.path / "tail.flac";
    atomic_write(tail, junk);
    CHECK(count(tail) >= total);
    // a damaged frame in the middle: libFLAC delivers it as silence, which must not pass
    file[file.size() / 2] ^= 0x55;
    fs::path bad = dir.path / "bad.flac";
    atomic_write(bad, file);
    CHECK_THROWS(count(bad));
}
