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

// ---- shared helpers

#include "futurelru.hpp"

TEST(future_lru_evicts_finished_least_recent_and_spares_kept) {
    FutureLru<int, int> c(3);
    std::vector<FutureLru<int, int>::Promise> ps(6);
    for (int k = 0; k < 4; k++) c.get(k, 1, &ps[size_t(k)]);
    CHECK(c.total_cost() == 4);
    c.trim([](int) { return false; });
    CHECK(c.size() == 4);  // all in flight: nothing can go
    for (int k = 0; k < 4; k++) ps[size_t(k)]->set_value(k * 10);
    FutureLru<int, int>::Promise none;
    CHECK(c.get(0, 1, &none).get() == 0 && !none);  // 0 is now the most recent
    c.trim([](int k) { return k == 1; });           // 1 is spared, 2 (least recent then) goes
    CHECK(c.size() == 3);
    c.get(2, 1, &ps[4]);
    CHECK(ps[4] != nullptr);  // was evicted: claimed anew
    c.get(3, 1, &none);
    CHECK(!none);
    // a failed claim is forgotten only while the entry is still that claim
    c.forget(2, ps[2].get());
    CHECK(c.size() == 4);
    c.forget(2, ps[4].get());
    CHECK(c.size() == 3);
    c.drop_ready();
    CHECK(c.size() == 0 && c.total_cost() == 0);
}

// ---- FLAC primitives

TEST(crc16_shift_updates_a_crc_for_a_changed_prefix) {
    uint64_t seed = 5;
    auto rnd = [&] {
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;
        return uint8_t(seed >> 56);
    };
    for (size_t n : {size_t(0), size_t(1), size_t(2), size_t(255), size_t(4097), size_t(70001), size_t(1) << 20}) {
        Bytes a(1 + n % 13), a2(1 + n % 7), body(n);
        for (auto* v : {&a, &a2, &body})
            for (auto& x : *v) x = rnd();
        Bytes ab = a, a2b = a2;
        append(ab, body);
        append(a2b, body);
        CHECK(flac::crc16(ab) == (flac::crc16_shift(flac::crc16(a), n) ^ flac::crc16(body)));
        CHECK(flac::crc16(a2b) == (flac::crc16(ab) ^ flac::crc16_shift(flac::crc16(a) ^ flac::crc16(a2), n)));
    }
}

TEST(encode_frames_matches_one_encoder_per_frame) {
    // one encoder for a run of frames must give exactly the frames that a fresh
    // encoder per frame gives: layouts measured one way are served the other
    struct Case {
        uint32_t bps, rate, block;
        size_t ch, total;
        int kind;  // 0 tone + noise, 1 silence, 2 full-scale noise, 3 square wave
    };
    std::vector<Case> cases;
    for (uint32_t bps : {8u, 16u, 24u, 32u})
        for (size_t ch : {size_t(1), size_t(2), size_t(6)})
            for (int kind : {0, 1, 2, 3}) cases.push_back({bps, 44100, 4096, ch, 4096 * 5 + 77, kind});
    cases.push_back({16, 48000, 1152, 2, 1152 * 9 + 1, 0});
    cases.push_back({24, 96000, 1000, 2, 1000 * 4, 0});
    cases.push_back({20, 705600, 4096, 2, 4096 * 3 + 5, 0});  // outside the streamable subset
    cases.push_back({28, 44100, 4096, 2, 4096 * 2 + 9, 2});   // likewise
    for (auto& c : cases) {
        auto sig = signal(c.total, c.ch, std::min(c.bps, 24u));
        int64_t hi = (int64_t(1) << (c.bps - 1)) - 1, lo = -hi - 1;
        uint64_t seed = 1;
        for (auto& chan : sig)
            for (size_t i = 0; i < chan.size(); i++) {
                seed = seed * 6364136223846793005ull + 1442695040888963407ull;
                int64_t v = c.kind == 0 ? int64_t(chan[i]) << (c.bps > 24 ? c.bps - 24 : 0)
                            : c.kind == 1 ? 0
                            : c.kind == 2 ? lo + int64_t(seed >> 1) % (hi - lo + 1)
                                          : ((i / 37) % 2 ? hi : lo);
                chan[i] = int32_t(v);
            }
        std::vector<std::span<const int32_t>> all(sig.begin(), sig.end());
        uint64_t first = 1000;
        auto run = flac::encode_frames(all, c.bps, c.rate, c.block, first);
        CHECK(run.size() == (c.total + c.block - 1) / c.block);
        for (size_t k = 0; k < run.size(); k++) {
            std::vector<std::span<const int32_t>> one;
            for (auto& s : all) one.push_back(s.subspan(k * c.block, std::min<size_t>(c.block, c.total - k * c.block)));
            Bytes alone = flac::encode_frame(one, c.bps, c.rate, c.block, first + k);
            CHECK_MSG(alone == run[k], "{} bit {} ch kind {} block {}: frame {} differs", c.bps, c.ch, c.kind, c.block, k);
            // (decode_frame needs codes a frame header can carry for bps and rate)
            if (flac::bps_code_for(c.bps) == 0 || c.rate > 655350) continue;
            auto dec = flac::decode_frame(run[k], c.bps, c.rate);
            for (size_t ci = 0; ci < c.ch; ci++) CHECK(std::equal(dec[ci].begin(), dec[ci].end(), one[ci].begin(), one[ci].end()));
        }
    }
}
