// SPDX-License-Identifier: GPL-3.0-or-later
// Floating-point WavPack images split into WavPack tracks; standalone ones retagged.
#include <unistd.h>
#include <wavpack/wavpack.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "apetag.hpp"
#include "library.hpp"
#include "md5.hpp"
#include "test.hpp"
#include "writeback.hpp"

using namespace wm;

namespace {

/// Channels of a test image: the mask covers the first ones, `ids` names the rest
/// (as libwavpack takes them); `block` samples per block (0: libwavpack's choice).
struct Shape {
    int channels = 2;
    int mask = 3;
    std::string ids;
    int block = 0;
};

/// A WavPack file as the wavpack tool writes it: the original WAV header stored
/// in the first block, block checksums, an audio MD5 in a block of its own.
Bytes make_float_wavpack(const std::vector<int32_t>& pcm, uint32_t rate, const Shape& shape = {}) {
    Bytes out;
    auto sink = [](void* id, void* data, int32_t n) -> int {
        auto* o = static_cast<Bytes*>(id);
        o->insert(o->end(), static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + n);
        return 1;
    };
    WavpackContext* w = WavpackOpenFileOutput(sink, &out, nullptr);
    WavpackConfig cfg;
    std::memset(&cfg, 0, sizeof cfg);
    cfg.bytes_per_sample = 4;
    cfg.bits_per_sample = 32;
    cfg.float_norm_exp = 127;
    cfg.num_channels = shape.channels;
    cfg.channel_mask = shape.mask;
    cfg.sample_rate = int32_t(rate);
    cfg.flags = CONFIG_HIGH_FLAG | CONFIG_MD5_CHECKSUM;
    cfg.block_samples = shape.block;
    uint32_t n = uint32_t(pcm.size() / size_t(shape.channels));
    CHECK(WavpackSetConfiguration64(w, &cfg, n, shape.ids.empty() ? nullptr : reinterpret_cast<const unsigned char*>(shape.ids.c_str())));
    Bytes riff(44, 0);  // stands for the source's WAV header
    std::memcpy(riff.data(), "RIFF", 4);
    CHECK(WavpackAddWrapper(w, riff.data(), uint32_t(riff.size())));
    CHECK(WavpackPackInit(w));
    std::vector<int32_t> buf = pcm;
    CHECK(WavpackPackSamples(w, buf.data(), n) && WavpackFlushSamples(w));
    Md5 md5;
    md5.update({reinterpret_cast<const uint8_t*>(pcm.data()), pcm.size() * 4});
    auto d = md5.finish();
    CHECK(WavpackStoreMD5Sum(w, d.data()) && WavpackFlushSamples(w));
    WavpackCloseFile(w);
    return out;
}

/// Decode a WavPack file with libwavpack (block checksums included); nullopt on any error.
std::optional<std::vector<int32_t>> decode(const fs::path& p) {
    char err[256];
    WavpackContext* w = WavpackOpenFileInput(p.c_str(), err, 0, 0);
    if (!w) return std::nullopt;
    size_t ch = size_t(WavpackGetNumChannels(w));
    std::vector<int32_t> out, buf(4096 * ch);
    while (uint32_t n = WavpackUnpackSamples(w, buf.data(), 4096)) out.insert(out.end(), buf.begin(), buf.begin() + ptrdiff_t(n * ch));
    bool ok = WavpackGetNumErrors(w) == 0 && uint64_t(WavpackGetNumSamples64(w)) == out.size() / ch;
    WavpackCloseFile(w);
    if (!ok) return std::nullopt;
    return out;
}

/// Channel identities of a WavPack file (one byte per channel).
std::string identities(const fs::path& p) {
    char err[256];
    WavpackContext* w = WavpackOpenFileInput(p.c_str(), err, 0, 0);
    if (!w) return "?";
    std::string ids(size_t(WavpackGetNumChannels(w)) + 1, '\0');
    WavpackGetChannelIdentities(w, reinterpret_cast<unsigned char*>(ids.data()));
    WavpackCloseFile(w);
    ids.resize(std::strlen(ids.c_str()));
    return ids;
}

/// Float samples that do not fit integers, `channels` interleaved.
std::vector<int32_t> float_pcm(uint64_t n, size_t channels, uint32_t rate) {
    std::vector<int32_t> pcm(size_t(n) * channels);
    uint32_t lcg = 1;
    for (uint64_t i = 0; i < n; i++)
        for (size_t c = 0; c < channels; c++) {
            lcg = lcg * 1664525 + 1013904223;
            float v = float(0.4 * std::sin(2 * M_PI * double(440 + 110 * c) * double(i) / rate) + double(lcg >> 8) / double(1u << 24) * 1e-3);
            std::memcpy(&pcm[size_t(i) * channels + c], &v, 4);
        }
    return pcm;
}

Bytes concat(const std::vector<Seg>& segs) {
    Bytes b;
    for (auto& s : segs) append(b, std::get<Bytes>(s));
    return b;
}

}  // namespace

TEST(float_wavpack_image_split_by_blocks) {
    fs::path dir = fs::temp_directory_path() / std::format("wm-wv-{}", ::getpid());
    fs::path src = dir / "lib" / "Album", single = dir / "lib" / "Single";
    fs::create_directories(src);
    fs::create_directories(single);
    fs::create_directories(dir / "tags" / "Lib" / "Album");
    fs::create_directories(dir / "tags" / "Lib" / "Single");
    // 7 s of 48 kHz stereo float
    constexpr uint32_t rate = 48000;
    const uint64_t n = rate * 7 + 333;
    std::vector<int32_t> pcm = float_pcm(n, 2, rate);
    Bytes wv = make_float_wavpack(pcm, rate);
    append(wv, concat(ape::build(Tags::from_pairs({{"Album", "Album X"}, {"Artist", "Src Artist"}, {"Year", "1971"}}), nullptr, std::nullopt)));
    atomic_write(src / "image.wv", wv);
    atomic_write(src / "image.cue", std::string_view("FILE \"image.wv\" WAVE\n  TRACK 01 AUDIO\n    TITLE \"A\"\n    INDEX 01 00:00:00\n"
                                                     "  TRACK 02 AUDIO\n    TITLE \"B\"\n    INDEX 01 00:02:37\n"
                                                     "  TRACK 03 AUDIO\n    TITLE \"C\"\n    INDEX 01 00:05:01\n"));
    atomic_write(single / "solo.wv", wv);
    auto ok = [](const ProcResult& r) { CHECK_MSG(r.ok(), "{}", r.err); };
    ok(run({"ffmpeg", "-v", "error", "-f", "lavfi", "-i", "color=c=red:s=64x48", "-frames:v", "1", (dir / "work.jpg").string()}));
    Bytes jpg = *try_read_file(dir / "work.jpg");
    atomic_write(dir / "tags" / "Lib" / "Album" / SIDECAR_NAME, std::format(R"({{"track": {{"2": {{"_cover": "{}"}}}}}})", (dir / "work.jpg").string()));
    atomic_write(dir / "tags" / "Lib" / "Single" / SIDECAR_NAME, R"({"file": {"solo.wv": {"TITLE": "Solo"}}})");
    for (auto& e : fs::recursive_directory_iterator(dir / "lib")) fs::last_write_time(e.path(), fs::file_time_type::clock::now() - std::chrono::hours(1));
    auto library = [&] {
        Config cfg;
        cfg.roots = {{"Lib", dir / "lib"}};
        cfg.tags_dir = dir / "tags";
        cfg.cache_dir = dir / "cache";
        cfg.workers = 0;
        return Library::create(std::move(cfg));
    };
    auto lib = library();
    auto l = lib->list_dir(src);
    CHECK_MSG(l->entries.size() == 3, "{} entries", l->entries.size());
    const uint64_t bounds[] = {0, (2 * 75 + 37) * 640, (5 * 75 + 1) * 640, n};
    std::vector<uint64_t> sizes;
    for (size_t t = 0; t < l->entries.size() && t < 3; t++) {
        auto& e = l->entries[t];
        CHECK_MSG(e.name == std::format("0{} - {}.wv", t + 1, "ABC"[t]), "name {}", e.name);
        CHECK(e.tag_section == "track" && e.tag_ext == "wv" && e.file->describe().starts_with("wavpack-image:"));
        sizes.push_back(e.file->size());
        Bytes whole = e.file->read_at(0, size_t(e.file->size()));
        CHECK(whole.size() == e.file->size());
        auto has_riff = [](const Bytes& b) { return std::search(b.begin(), b.end(), std::begin("RIFF"), std::end("RIFF") - 1) != b.end(); };
        // neither the source's WAV header nor one libwavpack makes up for re-encoded blocks
        CHECK(has_riff(wv) && !has_riff(whole));
        fs::path out = dir / std::format("t{}.wv", t);
        atomic_write(out, whole);
        auto got = decode(out);
        CHECK_MSG(got.has_value(), "track {} decodes without errors (block checksums)", t + 1);
        CHECK_MSG(got && *got == std::vector<int32_t>(pcm.begin() + ptrdiff_t(bounds[t] * 2), pcm.begin() + ptrdiff_t(bounds[t + 1] * 2)),
                  "track {}: the image's samples", t + 1);
        ok(run({"ffmpeg", "-v", "error", "-xerror", "-err_detect", "crccheck", "-i", out.string(), "-f", "null", "-"}));
        // reads at any offset agree with the whole file
        for (uint64_t o : {uint64_t(0), uint64_t(31), whole.size() / 3, whole.size() / 2 + 7, whole.size() - 100})
            CHECK_MSG(e.file->read_at(o, 5000) == Bytes(whole.begin() + ptrdiff_t(o), whole.begin() + ptrdiff_t(std::min<uint64_t>(o + 5000, whole.size()))),
                      "read at {}", o);
        auto tag = ape::read([&](uint64_t o, size_t len) { return e.file->read_at(o, len); }, e.file->size());
        CHECK(tag.has_value());
        if (!tag) continue;
        CHECK(tag->tags.get("TITLE") && *tag->tags.get("TITLE") == std::string(1, "ABC"[t]));
        CHECK(tag->tags.get("TRACKNUMBER") && *tag->tags.get("TRACKNUMBER") == std::to_string(t + 1) && tag->tags.get("TRACKTOTAL"));
        CHECK(tag->tags.get("ALBUM") && *tag->tags.get("ALBUM") == "Album X");
        CHECK_MSG(tag->tags.get("DATE") && *tag->tags.get("DATE") == "1971", "Year read back as DATE");
        CHECK_MSG(bool(tag->front_cover) == (t == 1), "cover on track 2 only");
        if (tag->front_cover) {
            Bytes v(whole.begin() + ptrdiff_t(tag->front_cover->off), whole.begin() + ptrdiff_t(tag->front_cover->off + tag->front_cover->len));
            CHECK(v.size() == 10 + jpg.size() && std::memcmp(v.data(), "cover.jpg\0", 10) == 0 && Bytes(v.begin() + 10, v.end()) == jpg);
        }
        auto wb = read_file_tags("wv", [&](uint64_t o, size_t len) { return e.file->read_at(o, len); }, e.file->size());
        CHECK(wb.get("TITLE") && *wb.get("TITLE") == std::string(1, "ABC"[t]));
    }
    // a fresh process opens from the cached index and layout, to the same sizes
    auto l2 = library()->list_dir(src);
    for (size_t t = 0; t < l2->entries.size() && t < sizes.size(); t++) CHECK(l2->entries[t].file->size() == sizes[t]);

    // a standalone one: its blocks unchanged, a new tag
    auto ls = lib->list_dir(single);
    CHECK(ls->entries.size() == 1);
    if (ls->entries.size() == 1) {
        auto& e = ls->entries[0];
        CHECK(e.name == "solo.wv" && e.tag_section == "file" && e.tag_ext == "wv" && e.file->describe().starts_with("retagged-wv:"));
        auto img = WvImage::open(single / "solo.wv", nullptr);
        CHECK(e.file->read_at(0, size_t(img->audio_end)) == Bytes(wv.begin(), wv.begin() + ptrdiff_t(img->audio_end)));
        auto t = read_file_tags("wv", [&](uint64_t o, size_t len) { return e.file->read_at(o, len); }, e.file->size());
        CHECK(t.get("TITLE") && *t.get("TITLE") == "Solo" && t.get("ALBUM") && *t.get("ALBUM") == "Album X");
    }
    fs::remove_all(dir);
}

TEST(wavpack_block_numbers_as_libwavpack_stores_them) {
    std::vector<int32_t> pcm(4000 * 2, 0);
    Bytes wv = make_float_wavpack(pcm, 44100);
    uint32_t len = le32(&wv[4]) + 8;
    Bytes block(wv.begin(), wv.begin() + len);
    // 40-bit sample numbers; totals of 2^32 - 1 and more are stored shifted (all
    // ones in the low word would mean "unknown length")
    for (uint64_t total : {uint64_t(4000), uint64_t(0xFFFFFFFE), uint64_t(0xFFFFFFFF), uint64_t(0x1234567890)}) {
        uint64_t index = total / 2;
        Bytes b = rebuild_block(block, index, total, true);
        WavpackHeader h;
        std::memcpy(&h, b.data(), sizeof h);  // little-endian host, as in the CI images
        CHECK_MSG(GET_BLOCK_INDEX(h) == int64_t(index), "index {}", index);
        CHECK_MSG(GET_TOTAL_SAMPLES(h) == int64_t(total), "total {} read as {}", total, GET_TOTAL_SAMPLES(h));
        CHECK(h.ckSize + 8 == b.size());
    }
}

TEST(float_wavpack_multichannel_tracks_at_block_edges) {
    fs::path dir = fs::temp_directory_path() / std::format("wm-wv-mc-{}", ::getpid());
    fs::path src = dir / "lib" / "Album";
    fs::create_directories(src);
    // four channels in two blocks per frame (pairs), the last two with identities
    // the mask cannot express; 4800-sample blocks = 7.5 cue frames at 48 kHz
    constexpr uint32_t rate = 48000;
    constexpr uint64_t block = 4800, n = block * 10 + 1234;
    Shape shape{4, 3, "\xC8\xC9", int(block)};
    std::vector<int32_t> pcm = float_pcm(n, 4, rate);
    atomic_write(src / "image.wv", make_float_wavpack(pcm, rate, shape));
    // track 2 starts on a block boundary (00:00:15 = 9600), track 3 lies inside
    // one block (12800..14080), track 4 ends on one (..19200), track 5 starts on
    // one and ends at the image's end, which is not
    atomic_write(src / "image.cue", std::string_view("FILE \"image.wv\" WAVE\n  TRACK 01 AUDIO\n    INDEX 01 00:00:00\n"
                                                     "  TRACK 02 AUDIO\n    INDEX 01 00:00:15\n  TRACK 03 AUDIO\n    INDEX 01 00:00:20\n"
                                                     "  TRACK 04 AUDIO\n    INDEX 01 00:00:22\n  TRACK 05 AUDIO\n    INDEX 01 00:00:30\n"));
    const uint64_t bounds[] = {0, 9600, 12800, 14080, 19200, n};
    for (auto& e : fs::recursive_directory_iterator(dir / "lib")) fs::last_write_time(e.path(), fs::file_time_type::clock::now() - std::chrono::hours(1));
    Config cfg;
    cfg.roots = {{"Lib", dir / "lib"}};
    cfg.tags_dir = dir / "tags";
    cfg.cache_dir = dir / "cache";
    cfg.workers = 0;
    auto l = Library::create(std::move(cfg))->list_dir(src);
    CHECK_MSG(l->entries.size() == 5, "{} entries", l->entries.size());
    for (size_t t = 0; t < l->entries.size() && t < 5; t++) {
        auto& e = l->entries[t];
        Bytes whole = e.file->read_at(0, size_t(e.file->size()));
        CHECK(whole.size() == e.file->size());
        fs::path out = dir / std::format("t{}.wv", t);
        atomic_write(out, whole);
        auto got = decode(out);
        CHECK_MSG(got.has_value(), "track {} decodes without errors (block checksums)", t + 1);
        CHECK_MSG(got && *got == std::vector<int32_t>(pcm.begin() + ptrdiff_t(bounds[t] * 4), pcm.begin() + ptrdiff_t(bounds[t + 1] * 4)),
                  "track {}: the image's samples", t + 1);
        // re-encoded first blocks carry the identities as much as copied ones
        CHECK_MSG(identities(out) == "\x01\x02\xC8\xC9", "track {}: channel identities", t + 1);
        for (uint64_t o : {uint64_t(0), whole.size() / 3, whole.size() - 50})
            CHECK_MSG(e.file->read_at(o, 3000) == Bytes(whole.begin() + ptrdiff_t(o), whole.begin() + ptrdiff_t(std::min<uint64_t>(o + 3000, whole.size()))),
                      "track {}: read at {}", t + 1, o);
    }
    fs::remove_all(dir);
}
