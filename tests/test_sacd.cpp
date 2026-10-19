// SPDX-License-Identifier: GPL-3.0-or-later
// SACD ISOs built here sector by sector (Scarlet Book layout, as sacd.cpp reads
// it), served as DSF tracks; DST frames checked against FFmpeg's decoder.
#include <unistd.h>

#include <algorithm>
#include <bit>
#include <cstring>

#include "cache.hpp"
#include "dst.hpp"
#include "id3.hpp"
#include "sacd.hpp"
#include "test.hpp"

using namespace wm;

namespace {

constexpr size_t SECTOR = 2048;
constexpr uint8_t AUDIO = 2;

void put_be16_at(Bytes& b, size_t at, uint16_t v) {
    b[at] = uint8_t(v >> 8);
    b[at + 1] = uint8_t(v);
}
void put_be32_at(Bytes& b, size_t at, uint32_t v) {
    for (size_t i = 0; i < 4; i++) b[at + i] = uint8_t(v >> (24 - 8 * i));
}
void put_tc(Bytes& b, size_t at, uint64_t frame) {
    b[at] = uint8_t(frame / 75 / 60);
    b[at + 1] = uint8_t(frame / 75 % 60);
    b[at + 2] = uint8_t(frame % 75);
}

uint32_t lcg_next(uint32_t& s) {
    s = s * 1664525 + 1013904223;
    return s;
}

/// One audio area: its frames as coded on the disc (plain DSD: byte-interleaved
/// channels, MSB first; DST: DST frames) and the tracks over them.
struct Area {
    uint32_t channels = 2;
    bool dst = false;
    std::vector<Bytes> frames;
    std::vector<std::pair<uint64_t, uint64_t>> tracks;  // frames [start, end)
    std::vector<std::string> titles;
    /// replace the area's last audio sector with one that cannot be parsed (the
    /// last frame must be spread over more than one sector)
    bool broken_end = false;
    /// set by audio_sectors() with `broken_end`: bytes of the last frame before it
    mutable size_t last_frame_kept = 0;
};

/// Audio sectors for `a`, packed as authoring tools do: up to 7 packets and 7
/// frame infos per sector, packets of at most 2047 bytes.
std::vector<Bytes> audio_sectors(const Area& a) {
    struct Packet {
        bool start;
        size_t frame, from, len;
    };
    std::vector<std::vector<Packet>> layout(1);
    size_t fi_len = a.dst ? 4 : 3, used = 1, infos = 0;
    const auto& frames = a.frames;
    for (size_t f = 0; f < frames.size(); f++) {
        for (size_t pos = 0; pos < frames[f].size();) {
            bool start = pos == 0;
            size_t need = 2 + (start ? fi_len : 0);
            if (layout.back().size() == 7 || (start && infos == 7) || used + need >= SECTOR) {
                layout.emplace_back();
                used = 1;
                infos = 0;
            }
            size_t len = std::min({frames[f].size() - pos, size_t(2047), SECTOR - used - need});
            layout.back().push_back({start, f, pos, len});
            used += need + len;
            infos += start;
            pos += len;
        }
    }
    // a DST frame info says over how many sectors the frame is spread
    std::vector<size_t> first(frames.size()), last(frames.size());
    for (size_t s = 0; s < layout.size(); s++)
        for (auto& p : layout[s]) {
            if (p.start) first[p.frame] = s;
            last[p.frame] = s;
        }
    std::vector<Bytes> out;
    for (size_t s = 0; s < layout.size(); s++) {
        Bytes b(SECTOR, 0);
        auto& pk = layout[s];
        size_t nfi = size_t(std::count_if(pk.begin(), pk.end(), [](auto& p) { return p.start; }));
        b[0] = uint8_t(pk.size() << 5 | nfi << 2 | (a.dst ? 1 : 0));
        size_t p = 1;
        for (auto& x : pk) {
            put_be16_at(b, p, uint16_t((x.start ? 0x8000 : 0) | AUDIO << 11 | x.len));
            p += 2;
        }
        for (auto& x : pk) {
            if (!x.start) continue;
            put_tc(b, p, x.frame);
            if (a.dst) b[p + 3] = uint8_t((last[x.frame] - first[x.frame] + 1) << 2);
            p += fi_len;
        }
        for (auto& x : pk) {
            std::memcpy(&b[p], frames[x.frame].data() + x.from, x.len);
            p += x.len;
        }
        out.push_back(std::move(b));
    }
    if (a.broken_end) {
        CHECK(first.back() < layout.size() - 1);
        a.last_frame_kept = frames.back().size();
        for (auto& x : layout.back()) a.last_frame_kept -= x.len;
        out.back().assign(SECTOR, 0xFF);  // 7 packets of 2047 bytes: overruns
    }
    return out;
}

/// An ISO with a stereo area and/or a multichannel area.
Bytes make_iso(const Area* stereo, const Area* mc) {
    std::vector<Bytes> sectors(512, Bytes(SECTOR, 0));
    Bytes& m = sectors[510];
    std::memcpy(m.data(), "SACDMTOC", 8);
    put_be16_at(m, 16, 1);  // set size
    put_be16_at(m, 18, 1);  // sequence
    std::memcpy(&m[24], "CAT-0001", 8);
    m[104] = 1;  // general genre table
    put_be16_at(m, 106, 5);  // Classical
    put_be16_at(m, 120, 2001);
    m[138] = 2;  // ISO 8859-1
    Bytes& t = sectors[511];
    std::memcpy(t.data(), "SACDText", 8);
    put_be16_at(t, 16, 64);  // album title
    std::memcpy(&t[64], "Album \xC9t\xE9", 9);
    put_be16_at(t, 18, 96);  // album artist
    std::memcpy(&t[96], "The Artist", 10);
    auto add_area = [&](const Area& a, bool multichannel) {
        uint32_t toc = uint32_t(sectors.size());
        for (int i = 0; i < 3; i++) sectors.emplace_back(SECTOR, 0);
        auto audio = audio_sectors(a);
        uint32_t start = uint32_t(sectors.size()), end = start + uint32_t(audio.size()) - 1;
        Bytes& at = sectors[toc];
        std::memcpy(at.data(), multichannel ? "MULCHTOC" : "TWOCHTOC", 8);
        put_be16_at(at, 10, 3);
        at[0x14] = 4;  // 64 x 44.1 kHz
        at[0x15] = a.dst ? 0 : 2;
        at[0x20] = uint8_t(a.channels);
        at[0x45] = uint8_t(a.tracks.size());
        put_be32_at(at, 0x48, start);
        put_be32_at(at, 0x4C, end);
        at[0x5A] = 2;
        Bytes& trl = sectors[toc + 1];
        std::memcpy(trl.data(), "SACDTRL2", 8);
        for (size_t i = 0; i < a.tracks.size(); i++) {
            put_tc(trl, 8 + 4 * i, a.tracks[i].first);
            put_tc(trl, 8 + 1020 + 4 * i, a.tracks[i].second - a.tracks[i].first);
        }
        Bytes& tt = sectors[toc + 2];
        std::memcpy(tt.data(), "SACDTTxt", 8);
        size_t pos = 600;
        for (size_t i = 0; i < a.titles.size(); i++) {
            put_be16_at(tt, 8 + 2 * i, uint16_t(pos));
            tt[pos] = 1;  // one item: the title
            tt[pos + 4] = 1;
            tt[pos + 5] = 0x20;
            std::memcpy(&tt[pos + 6], a.titles[i].data(), a.titles[i].size());
            pos += (6 + a.titles[i].size() + 1 + 3) / 4 * 4;
        }
        for (auto& s : audio) sectors.push_back(std::move(s));
        sectors.emplace_back(SECTOR, 0);  // stands for the area's backup TOC
        return toc;
    };
    if (stereo) put_be32_at(sectors[510], 64, add_area(*stereo, false));
    if (mc) put_be32_at(sectors[510], 72, add_area(*mc, true));
    Bytes iso;
    for (auto& s : sectors) append(iso, s);
    return iso;
}

/// Plain DSD frames of pseudo-random audio.
std::vector<Bytes> dsd_frames(size_t n, uint32_t channels, uint32_t seed) {
    std::vector<Bytes> out(n, Bytes(SACD_FRAME_BYTES * channels));
    for (auto& f : out)
        for (auto& b : f) b = uint8_t(lcg_next(seed) >> 24);
    return out;
}

/// The DSF file a track of plain DSD `frames` must come out as (without its ID3 tag).
Bytes expected_dsf_audio(const std::vector<Bytes>& frames, uint64_t f0, uint64_t f1, uint32_t channels) {
    std::array<uint8_t, 256> rev{};
    for (int i = 0; i < 256; i++)
        for (int k = 0; k < 8; k++)
            if (i >> k & 1) rev[size_t(i)] |= uint8_t(0x80 >> k);
    std::vector<Bytes> per_ch(channels);
    for (uint64_t f = f0; f < f1; f++)
        for (size_t j = 0; j < frames[f].size(); j++) per_ch[j % channels].push_back(rev[frames[f][j]]);
    size_t blocks = (per_ch[0].size() + 4095) / 4096;
    Bytes out;
    for (size_t k = 0; k < blocks; k++)
        for (auto& c : per_ch) {
            size_t a = k * 4096, b = std::min(c.size(), a + 4096);
            out.insert(out.end(), c.begin() + ptrdiff_t(a), c.begin() + ptrdiff_t(b));
            out.resize(out.size() + (4096 - (b - a)), 0);
        }
    return out;
}

/// MSB-first bit writer for DST frames.
struct BitWriter {
    Bytes b;
    size_t n = 0;
    void put(uint32_t v, uint32_t bits) {
        for (uint32_t i = bits; i-- > 0; n++) {
            if (n % 8 == 0) b.push_back(0);
            if (v >> i & 1) b.back() |= uint8_t(0x80 >> (n % 8));
        }
    }
    /// sr_golomb as DST reads it: |v| as a Rice code (zeros, a one, k bits), then its sign if not 0
    void signed_rice(int32_t v, uint32_t k) {
        uint32_t m = uint32_t(std::abs(v));
        for (uint32_t q = m >> k; q > 0; q--) put(0, 1);
        put(1, 1);
        put(m & ((1u << k) - 1), k);
        if (v) put(v < 0, 1);
    }
};

/// A compressed DST frame exercising every part of the format FFmpeg decodes:
/// channel maps, half probabilities, filter and probability tables (stored
/// plain and predicted with each method), then `ac_bytes` of arithmetic-coded
/// data (any bits are a valid code).
Bytes dst_frame(uint32_t channels, uint32_t& seed, size_t ac_bytes) {
    BitWriter w;
    auto rnd = [&](uint32_t n) { return lcg_next(seed) >> 8 & (n - 1); };  // n: a power of 2
    w.put(1, 1);  // compressed
    w.put(7, 3);  // same segmentation, for all channels, end of channel segmentation
    bool same_map = channels == 1 || rnd(2);
    w.put(same_map, 1);
    auto channel_map = [&] {
        w.put(0, 1);  // explicit
        uint32_t elements = 1;
        for (uint32_t ch = 1; ch < channels; ch++) {
            uint32_t v = ch == 1 ? 1 : std::min(rnd(4), elements);
            w.put(v, uint32_t(std::bit_width(elements)));
            if (v == elements) elements++;
        }
        return elements;
    };
    uint32_t fsets = channel_map(), probs = same_map ? fsets : channel_map();
    for (uint32_t ch = 0; ch < channels; ch++) w.put(rnd(2), 1);  // half probability
    const int32_t fsets_pred[3][3] = {{-8, 0, 0}, {-16, 8, 0}, {-9, -5, 6}};
    const int32_t probs_pred[3][3] = {{-8, 0, 0}, {-16, 8, 0}, {-24, 24, -8}};
    auto table = [&](uint32_t elements, uint32_t length_bits, uint32_t coeff_bits, bool is_signed, const int32_t pred[3][3]) {
        for (uint32_t e = 0; e < elements; e++) {
            uint32_t len = 3 + rnd(1u << (length_bits - 2));
            // filter coefficients small enough for the 16-bit filter tables; probabilities 1..128
            auto value = [&] { return is_signed ? int32_t(rnd(128)) - 64 : int32_t(rnd(128)) + 1; };
            auto plain = [&](int32_t v) { w.put(is_signed ? uint32_t(v) & ((1u << coeff_bits) - 1) : uint32_t(v - 1), coeff_bits); };
            w.put(len - 1, length_bits);
            uint32_t method = rnd(4);
            if (method == 3) {
                w.put(0, 1);
                for (uint32_t j = 0; j < len; j++) plain(value());
                continue;
            }
            w.put(1, 1);
            w.put(method, 2);
            std::vector<int32_t> c;
            for (uint32_t j = 0; j <= method; j++) plain(c.emplace_back(value()));
            uint32_t k = rnd(8);
            w.put(k, 3);
            for (uint32_t j = method + 1; j < len; j++) {
                int64_t x = 0;
                for (uint32_t m = 0; m <= method; m++) x += pred[method][m] * c[j - m - 1];
                int32_t correction = int32_t(x >= 0 ? -((x + 4) / 8) : (-x + 3) / 8);
                int32_t v = value();
                w.signed_rice(v - correction, k);
                c.push_back(v);
            }
        }
    };
    table(fsets, 7, 9, true, fsets_pred);
    table(probs, 6, 7, false, probs_pred);
    w.put(0, 1);  // arithmetic-coded data follows
    for (size_t i = 0; i < ac_bytes; i++) w.put(lcg_next(seed) >> 24, 8);
    return w.b;
}

/// A DSDIFF file (FFmpeg reads DST from these): `frames` are DST frames, or
/// with `plain` byte-interleaved DSD.
Bytes dff(uint32_t channels, const std::vector<Bytes>& frames, bool plain) {
    auto chunk = [](std::string_view id, const Bytes& body) {
        Bytes c(id.begin(), id.end());
        put_be64(c, body.size());
        append(c, body);
        if (body.size() % 2) c.push_back(0);
        return c;
    };
    Bytes fs, chnl, cmpr, prop(4), fver;
    put_be32(fver, 0x01050000);
    put_be32(fs, DSD64_RATE);
    put_be16(chnl, uint16_t(channels));
    for (uint32_t ch = 0; ch < channels; ch++) append(chnl, std::string_view(std::format("C{:03}", ch)));
    append(cmpr, std::string_view(plain ? "DSD " : "DST "));
    cmpr.push_back(0);
    cmpr.push_back(0);
    std::memcpy(prop.data(), "SND ", 4);
    append(prop, chunk("FS  ", fs));
    append(prop, chunk("CHNL", chnl));
    append(prop, chunk("CMPR", cmpr));
    Bytes audio;
    if (plain) {
        for (auto& f : frames) append(audio, f);
    } else {
        Bytes frte;
        put_be32(frte, uint32_t(frames.size()));
        put_be16(frte, 75);
        append(audio, chunk("FRTE", frte));
        for (auto& f : frames) append(audio, chunk("DSTF", f));
    }
    Bytes body(4);
    std::memcpy(body.data(), "DSD ", 4);
    append(body, chunk("FVER", fver));
    append(body, chunk("PROP", prop));
    append(body, chunk(plain ? "DSD " : "DST ", audio));
    return chunk("FRM8", body);
}

struct Dir {
    fs::path path = fs::temp_directory_path() / std::format("wm-sacd-{}-{}", ::getpid(), counter()++);
    Dir() { fs::create_directories(path); }
    ~Dir() { fs::remove_all(path); }
    static int& counter() {
        static int n = 0;
        return n;
    }
};

/// Check the DSF track `t` of `disc` against the area it came from.
void check_dsf(const std::shared_ptr<const SacdDisc>& disc, size_t t, const Area& a, const Bytes& want_audio) {
    DsfTrack dsf(disc, t, disc->track_tags(t));
    Bytes whole = dsf.read_at(0, size_t(dsf.size()));
    CHECK(whole.size() == dsf.size());
    CHECK(std::memcmp(whole.data(), "DSD ", 4) == 0 && le64(&whole[4]) == 28 && le64(&whole[12]) == whole.size());
    uint64_t meta = le64(&whole[20]), frames = a.tracks[t].second - a.tracks[t].first;
    CHECK(std::memcmp(&whole[28], "fmt ", 4) == 0 && le32(&whole[28 + 24]) == a.channels && le32(&whole[28 + 28]) == 2'822'400);
    CHECK_MSG(le64(&whole[28 + 36]) == frames * SACD_FRAME_BYTES * 8, "sample count {}", le64(&whole[28 + 36]));
    CHECK(std::memcmp(&whole[80], "data", 4) == 0 && le64(&whole[84]) == 12 + want_audio.size() && meta == 92 + want_audio.size());
    CHECK_MSG(Bytes(whole.begin() + 92, whole.begin() + ptrdiff_t(meta)) == want_audio, "track {}: audio", t + 1);
    CHECK(std::memcmp(&whole[meta], "ID3", 3) == 0);
    // reads anywhere agree with the whole file
    for (uint64_t o : {uint64_t(0), uint64_t(91), whole.size() / 3, whole.size() / 2 + 4095, whole.size() - 10})
        CHECK_MSG(dsf.read_at(o, 70000) == Bytes(whole.begin() + ptrdiff_t(o), whole.begin() + ptrdiff_t(std::min<uint64_t>(o + 70000, whole.size()))),
                  "track {}: read at {}", t + 1, o);
}

}  // namespace

TEST(sacd_plain_dsd_area_as_dsf_tracks) {
    Dir d;
    Area a;
    a.frames = dsd_frames(40, 2, 7);
    a.tracks = {{0, 13}, {13, 40}};
    a.titles = {"First", "Second \xFC"};
    fs::path iso = d.path / "disc.iso";
    atomic_write(iso, make_iso(&a, nullptr));
    CHECK(is_sacd(iso));
    Cache cache(d.path / "cache");
    for (int pass = 0; pass < 2; pass++) {  // scanned, then from the cached frame table
        CHECK(sacd_is_cached(cache, iso, false) == (pass == 1));
        auto disc = SacdDisc::open(iso, &cache, false);
        CHECK(disc->channels == 2 && !disc->dst && disc->tracks.size() == 2);
        auto tags = disc->track_tags(1);
        CHECK(tags.get("TITLE") && *tags.get("TITLE") == "Second ü");
        CHECK(tags.get("ALBUM") && *tags.get("ALBUM") == "Album Été");
        CHECK(tags.get("ARTIST") && *tags.get("ARTIST") == "The Artist");
        CHECK(tags.get("DATE") && *tags.get("DATE") == "2001" && tags.get("GENRE") && *tags.get("GENRE") == "Classical");
        CHECK(tags.get("CATALOGNUMBER") && *tags.get("CATALOGNUMBER") == "CAT-0001");
        for (size_t t = 0; t < 2; t++) check_dsf(disc, t, a, expected_dsf_audio(a.frames, a.tracks[t].first, a.tracks[t].second, 2));
    }
}

TEST(sacd_multichannel_area) {
    Dir d;
    Area st, mc;
    st.frames = dsd_frames(10, 2, 1);
    st.tracks = {{0, 10}};
    mc.channels = 6;
    mc.frames = dsd_frames(21, 6, 2);
    mc.tracks = {{0, 5}, {5, 21}};
    fs::path iso = d.path / "disc.iso";
    atomic_write(iso, make_iso(&st, &mc));
    auto disc = SacdDisc::open(iso, nullptr, true);
    CHECK(disc->channels == 6 && disc->tracks.size() == 2);
    for (size_t t = 0; t < 2; t++) check_dsf(disc, t, mc, expected_dsf_audio(mc.frames, mc.tracks[t].first, mc.tracks[t].second, 6));
    CHECK(SacdDisc::open(iso, nullptr, false)->channels == 2);
}

TEST(sacd_short_final_frame_before_a_broken_sector) {
    // some rips end the area with an unreadable sector: the frame cut short by
    // it is padded with DSD silence
    Dir d;
    Area a;
    a.frames = dsd_frames(12, 2, 3);
    a.tracks = {{0, 12}};
    a.broken_end = true;
    fs::path iso = d.path / "disc.iso";
    atomic_write(iso, make_iso(&a, nullptr));
    auto disc = SacdDisc::open(iso, nullptr, false);
    auto frames = a.frames;
    CHECK(a.last_frame_kept > 0 && a.last_frame_kept < frames.back().size());
    std::fill(frames.back().begin() + ptrdiff_t(a.last_frame_kept), frames.back().end(), 0x69);
    check_dsf(disc, 0, a, expected_dsf_audio(frames, 0, 12, 2));
}

TEST(sacd_track_list_not_ending_with_the_last_track) {
    // the TOC's last track need not end last: every track must still be served
    Dir d;
    Area a;
    a.frames = dsd_frames(30, 2, 4);
    a.tracks = {{0, 30}, {10, 20}};
    fs::path iso = d.path / "disc.iso";
    atomic_write(iso, make_iso(&a, nullptr));
    auto disc = SacdDisc::open(iso, nullptr, false);
    for (size_t t = 0; t < 2; t++) check_dsf(disc, t, a, expected_dsf_audio(a.frames, a.tracks[t].first, a.tracks[t].second, 2));
}

TEST(sacd_multichannel_only_disc_is_served_once) {
    Dir d;
    Area mc;
    mc.channels = 6;
    mc.frames = dsd_frames(9, 6, 5);
    mc.tracks = {{0, 9}};
    fs::path iso = d.path / "disc.iso";
    atomic_write(iso, make_iso(nullptr, &mc));
    auto disc = SacdDisc::open(iso, nullptr, false);  // the main area
    CHECK(disc->channels == 6);
    check_dsf(disc, 0, mc, expected_dsf_audio(mc.frames, 0, 9, 6));
    try {
        SacdDisc::open(iso, nullptr, true);
        CHECK_MSG(false, "the multichannel area was offered a second time");
    } catch (const Error& e) {
        CHECK_MSG(std::string(e.what()).find("no multichannel area") != std::string::npos, "{}", e.what());
    }
}

TEST(dst_decoder_matches_ffmpeg) {
    // DST is checked through FFmpeg's decoder: the same frames as DST, and as the
    // DSD decoded here, must give the same PCM (FFmpeg converts both alike)
    Dir d;
    for (uint32_t channels : {2u, 5u, 6u}) {
        uint32_t seed = channels;
        std::vector<Bytes> coded, decoded;
        DstDecoder dec(channels);
        for (int i = 0; i < 12; i++) {
            coded.push_back(dst_frame(channels, seed, 600 + 80 * size_t(i)));
            decoded.emplace_back(SACD_FRAME_BYTES * channels);
            dec.decode(coded.back(), decoded.back());
        }
        auto pcm = [&](const Bytes& file, const char* name) {
            fs::path p = d.path / name;
            atomic_write(p, file);
            auto r = run({"ffmpeg", "-v", "error", "-i", p.string(), "-f", "f32le", "-"});
            CHECK_MSG(r.ok(), "{}", r.err);
            return r.out;
        };
        std::string want = pcm(dff(channels, coded, false), "dst.dff"), got = pcm(dff(channels, decoded, true), "dsd.dff");
        CHECK_MSG(!want.empty() && want == got, "{} channels: {} vs {} bytes of PCM", channels, want.size(), got.size());
    }
}

TEST(sacd_dst_area_and_a_final_frame_cut_short) {
    for (bool broken : {false, true}) {
        Dir d;
        Area a;
        a.dst = true;
        uint32_t seed = 9;
        for (int i = 0; i < 20; i++) a.frames.push_back(dst_frame(2, seed, 1500 + 150 * size_t(i)));  // 1 to 3 sectors each
        a.tracks = {{0, 7}, {7, 20}};
        a.broken_end = broken;
        fs::path iso = d.path / "disc.iso";
        atomic_write(iso, make_iso(&a, nullptr));
        auto disc = SacdDisc::open(iso, nullptr, false);
        CHECK(disc->dst);
        std::vector<Bytes> dsd;
        DstDecoder dec(2);
        for (auto& f : a.frames) dec.decode(f, dsd.emplace_back(SACD_FRAME_BYTES * 2));
        // a compressed frame cannot be decoded in part: cut short, it is silence
        if (broken) std::fill(dsd.back().begin(), dsd.back().end(), 0x69);
        for (size_t t = 0; t < 2; t++) check_dsf(disc, t, a, expected_dsf_audio(dsd, a.tracks[t].first, a.tracks[t].second, 2));
    }
}
