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

/// A WavPack file as the wavpack tool writes it: the original WAV header stored
/// in the first block, block checksums, an audio MD5 in a block of its own.
Bytes make_float_wavpack(const std::vector<int32_t>& pcm, uint32_t rate) {
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
    cfg.num_channels = 2;
    cfg.channel_mask = 3;
    cfg.sample_rate = int32_t(rate);
    cfg.flags = CONFIG_HIGH_FLAG | CONFIG_MD5_CHECKSUM;
    uint32_t n = uint32_t(pcm.size() / 2);
    CHECK(WavpackSetConfiguration64(w, &cfg, n, nullptr));
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
    std::vector<int32_t> out, buf(4096 * 2);
    while (uint32_t n = WavpackUnpackSamples(w, buf.data(), 4096)) out.insert(out.end(), buf.begin(), buf.begin() + n * 2);
    bool ok = WavpackGetNumErrors(w) == 0 && uint64_t(WavpackGetNumSamples64(w)) == out.size() / 2;
    WavpackCloseFile(w);
    if (!ok) return std::nullopt;
    return out;
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
    // 7 s of 48 kHz stereo float that does not fit integers
    constexpr uint32_t rate = 48000;
    const uint64_t n = rate * 7 + 333;
    std::vector<int32_t> pcm(size_t(n) * 2);
    uint32_t lcg = 1;
    for (uint64_t i = 0; i < n; i++)
        for (int c = 0; c < 2; c++) {
            lcg = lcg * 1664525 + 1013904223;
            float v = float(0.4 * std::sin(2 * M_PI * (c ? 330 : 440) * double(i) / rate) + double(lcg >> 8) / double(1u << 24) * 1e-3);
            std::memcpy(&pcm[size_t(i) * 2 + size_t(c)], &v, 4);
        }
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
        CHECK_MSG(tag->tags.m.contains("Year") && *tag->tags.get("YEAR") == "1971" && !tag->tags.get("DATE"), "Year read back as written");
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
