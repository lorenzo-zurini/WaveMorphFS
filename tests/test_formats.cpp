// SPDX-License-Identifier: GPL-3.0-or-later
// CUE / charset detection, ID3, MP4 and ffprobe-tag parsing.
#include <unistd.h>

#include "charset.hpp"
#include "cue.hpp"
#include "id3.hpp"
#include "mp4.hpp"
#include "test.hpp"

using namespace wm;

static const char* SAMPLE_CUE = R"(REM GENRE Classical
REM DATE 1991
REM COMMENT "ExactAudioCopy v1.0"
PERFORMER "Bayerisches Staatsorchester, Wolfgang Sawallisch"
TITLE "Bruckner: Symphonie Nr. 5"
CATALOG 0012345678905
FILE "image.wav" WAVE
  TRACK 01 AUDIO
    TITLE "I. Introduktion"
    PERFORMER "BSO"
    INDEX 01 00:00:00
  TRACK 02 AUDIO
    TITLE "II. Adagio"
    INDEX 00 20:11:40
    INDEX 01 20:13:02
  TRACK 03 AUDIO
    TITLE "III. Scherzo"
    SONGWRITER "Anton Bruckner"
    INDEX 01 38:00:74
)";

TEST(cue_parses_sample) {
    auto c = CueSheet::parse(SAMPLE_CUE);
    CHECK(c.is_image());
    CHECK(c.files == std::vector<std::string>{"image.wav"});
    CHECK(c.fields["GENRE"] == "Classical");
    CHECK(c.fields["COMMENT"] == "ExactAudioCopy v1.0");
    CHECK(c.fields["CATALOG"] == "0012345678905");
    CHECK(c.tracks.size() == 3);
    CHECK(c.tracks[1].index00 == uint64_t((20 * 60 + 11) * 75 + 40));
    CHECK(c.tracks[1].index01 == uint64_t((20 * 60 + 13) * 75 + 2));
    CHECK(c.tracks[2].fields["SONGWRITER"] == "Anton Bruckner");
    CHECK(c.tracks[0].fields["PERFORMER"] == "BSO");
}

TEST(cue_windows_1250_romanian) {
    // Romanian diacritics in CP1250: ş=0xBA ţ=0xFE â=0xE2 î=0xEE
    std::string raw = "PERFORMER \"Phoenix \xBA\xFE\"\r\nFILE \"a.flac\" WAVE\r\n  TRACK 01 AUDIO\r\n    INDEX 01 00:00:00\r\n  TRACK 02 AUDIO\r\n    INDEX 01 01:00:00\r\n";
    raw += "  TRACK 03 AUDIO\r\n    TITLE \"C\xE2ntec \xEEn \xBAir\"\r\n    INDEX 01 02:00:00\r\n";
    auto c = CueSheet::parse_bytes(bytes_of(raw));
    CHECK_MSG(starts_with(c.encoding, "windows-125"), "{}", c.encoding);
    CHECK(c.tracks.size() == 3);
    CHECK_MSG(c.tracks[2].fields["TITLE"] == "Cântec în şir", "{} ({})", c.tracks[2].fields["TITLE"], c.encoding);
}

TEST(cue_rejects_bad_order) {
    CHECK_THROWS(CueSheet::parse("FILE \"a.flac\" WAVE\nTRACK 01 AUDIO\nINDEX 01 01:00:00\nTRACK 02 AUDIO\nINDEX 01 00:30:00\n"));
}

TEST(charset_detection) {
    struct Case {
        const char* bytes;
        const char* want;
    };
    // (legacy-encoded bytes, expected UTF-8 text)
    std::vector<Case> cases = {
        {"Orchestre symphonique de Montr\xE9" "al - Concertos n\xB0" "1", "Orchestre symphonique de Montréal - Concertos n°1"},  // cp1252
        {"\xD0\xF3\xF1\xF1\xEA\xE0\xFF \xEC\xF3\xE7\xFB\xEA\xE0 - \xD0\xE0\xF5\xEC\xE0\xED\xE8\xED\xEE\xE2", "Русская музыка - Рахманинов"},  // cp1251
        {"Dvo\xF8\xE1k - Symfonie \xE8. 9 \x84Z Nov\xE9ho sv\xECta\x93", "Dvořák - Symfonie č. 9 „Z Nového světa“"},  // cp1250
        {"\x8F\xAC\xE0\x56\x90\xAA\x8E\xA2 - \x83\x73\x83\x41\x83\x6D", "小澤征爾 - ピアノ"},  // Shift_JIS
        {"Gr\xFCn und Bl\xE4ue - Stra\xDF" "e", "Grün und Bläue - Straße"},  // cp1252 (German)
    };
    for (auto& c : cases) {
        auto d = decode_text(bytes_of(c.bytes));
        CHECK_MSG(d.text == c.want, "got \"{}\" ({}), want \"{}\"", d.text, d.encoding, c.want);
    }
    CHECK(decode_text(bytes_of("plain ascii")).encoding == "UTF-8");
    CHECK(decode_text(bytes_of("\xEF\xBB\xBFwith bom")).text == "with bom");
}

TEST(id3_builds_valid_header) {
    Tags t;
    t.set("TITLE", "Promenade");
    t.set("TRACKNUMBER", "1");
    t.set("TRACKTOTAL", "15");
    t.set("CATALOGNUMBER", "CLSC2201 SA");
    auto b = id3::build(t);
    CHECK(std::string_view(reinterpret_cast<const char*>(b.data()), 5) == std::string_view("ID3\x04\x00", 5));
    size_t size = 0;
    for (int i = 6; i < 10; i++) size = size << 7 | b[size_t(i)];
    CHECK(size + 10 == b.size());
    std::string s(b.begin(), b.end());
    CHECK(s.find("TIT2") != std::string::npos && s.find("Promenade") != std::string::npos && s.find("1/15") != std::string::npos &&
          s.find("CATALOGNUMBER") != std::string::npos);
}

TEST(id3_ffprobe_and_picard_names) {
    auto t = Tags::from_pairs({{"album_artist", "AA"}, {"track", "3/12"}, {"MusicBrainz Album Id", "mbid"}, {"TSRC", "USRC17607839"}});
    auto b = id3::build(t);
    std::string s(b.begin(), b.end());
    CHECK(s.find("TPE2") != std::string::npos && s.find("AA") != std::string::npos);
    CHECK(s.find("TRCK") != std::string::npos && s.find("3/12") != std::string::npos);
    CHECK_MSG(s.find("TXXX") != std::string::npos && s.find("MusicBrainz Album Id") != std::string::npos, "spelling kept in TXXX");
    CHECK(s.find("TSRC") != std::string::npos && s.find("USRC17607839") != std::string::npos);
}

TEST(id3_retag_keeps_unnamed_frames_and_replaces_named) {
    // a v2.3 tag: TIT2 (UTF-16), TYER, APIC, TXXX "MusicBrainz Album Id", TRCK
    Bytes body;
    auto v23 = [&](const char* id, std::string_view data) {
        append(body, std::string_view(id, 4));
        put_be32(body, uint32_t(data.size()));
        body.push_back(0);
        body.push_back(0);
        append(body, data);
    };
    using namespace std::string_view_literals;
    v23("TIT2", "\x01\xFF\xFEO\0l\0d\0"sv);
    v23("TYER", "\x00" "1999"sv);
    v23("APIC", "\x00image/jpeg\x00\x03\x00JPEGDATA"sv);
    v23("TXXX", "\x00MusicBrainz Album Id\x00old"sv);
    v23("TRCK", "\x00" "4/10"sv);
    Bytes tag = {'I', 'D', '3', 3, 0, 0};
    uint32_t sz = uint32_t(body.size() + 16);
    for (int sh : {21, 14, 7, 0}) tag.push_back(uint8_t((sz >> sh) & 0x7F));
    append(tag, body);
    tag.insert(tag.end(), 16, 0);  // padding
    auto [src, len] = id3::parse_tag(tag);
    CHECK(len == tag.size());
    auto ov = Tags::from_pairs({{"title", "New"}, {"MUSICBRAINZ ALBUM ID", "new"}, {"TRACKTOTAL", "12"}});
    id3::apply(src.frames, ov);
    auto out = id3::serialize(src.flags, src.frames);
    auto fr = id3::parse_tag(out).first.frames;
    auto has = [&](std::string_view id) { return std::any_of(fr.begin(), fr.end(), [&](auto& f) { return f.sid() == id; }); };
    CHECK_MSG(has("APIC"), "picture kept");
    CHECK_MSG(!has("TYER") && has("TDRC"), "v2.3 date upgraded");
    CHECK(std::count_if(fr.begin(), fr.end(), [](auto& f) { return f.sid() == "TIT2"; }) == 1);
    std::string s(out.begin(), out.end());
    CHECK(s.find("New") != std::string::npos && s.find("O\0l\0d"sv) == std::string::npos);
    CHECK_MSG(s.find("MusicBrainz Album Id") == std::string::npos && s.find("MUSICBRAINZ ALBUM ID") != std::string::npos, "TXXX replaced case-insensitively");
    CHECK_MSG(s.find("4/12") != std::string::npos, "track number kept, total set");
    CHECK(s.find("1999") != std::string::npos);
}

TEST(mp4_rewrites_ilst_and_shifts_offsets) {
    auto bx = [](const char* t, const Bytes& payload) {
        Bytes v;
        put_be32(v, uint32_t(payload.size() + 8));
        append(v, std::string_view(t, 4));
        append(v, payload);
        return v;
    };
    auto cat = [](std::initializer_list<Bytes> parts) {
        Bytes o;
        for (auto& p : parts) append(o, p);
        return o;
    };
    auto item = [&](const char* t, uint32_t typ, std::string_view p) {
        Bytes d;
        put_be32(d, typ);
        put_be32(d, 0);
        append(d, p);
        return bx(t, bx("data", d));
    };
    Bytes ilst = bx("ilst", cat({item("\xA9nam", 1, "Old"), item("covr", 13, "JPEG")}));
    Bytes meta = bx("meta", cat({Bytes{0, 0, 0, 0}, bx("hdlr", Bytes(25, 0)), ilst}));
    Bytes ftyp = bx("ftyp", Bytes{'M', '4', 'A', ' ', 0, 0, 0, 0});
    auto build = [&](uint32_t off) {
        Bytes st = {0, 0, 0, 0};
        put_be32(st, 1);
        put_be32(st, off);
        Bytes trak = bx("trak", bx("mdia", bx("minf", bx("stbl", bx("stco", st)))));
        return bx("moov", cat({trak, bx("udta", meta)}));
    };
    uint32_t moov_len = uint32_t(build(0).size());
    uint32_t audio_off = uint32_t(ftyp.size()) + moov_len + 8;
    Bytes file = cat({ftyp, build(audio_off), bx("mdat", Bytes{'A', 'U', 'D', 'I', 'O'})});
    fs::path p = fs::temp_directory_path() / std::format("wm-mp4-{}", ::getpid());
    atomic_write(p, file);
    auto ov = Tags::from_pairs({{"title", "A much longer new title"}, {"MusicBrainz Album Id", "mbid"}, {"track", "3/12"}});
    auto r = mp4::retag_m4a(p, ov);
    fs::remove(p);
    CHECK(r.moov_pos == ftyp.size() && r.moov_len == moov_len);
    Bytes out(file.begin(), file.begin() + ptrdiff_t(r.moov_pos));
    append(out, r.moov);
    out.insert(out.end(), file.begin() + ptrdiff_t(r.moov_pos + r.moov_len), file.end());
    // the chunk offset still points at the audio
    std::string s(r.moov.begin(), r.moov.end());
    auto at = s.find("stco");
    CHECK(at != std::string::npos);
    uint32_t off = be32(&r.moov[at + 12]);
    CHECK(std::string_view(reinterpret_cast<const char*>(&out[off]), 5) == "AUDIO");
    CHECK(s.find("A much longer new title") != std::string::npos && s.find("Old") == std::string::npos);
    CHECK_MSG(s.find("JPEG") != std::string::npos, "cover kept");
    CHECK(s.find("com.apple.iTunes") != std::string::npos && s.find("MusicBrainz Album Id") != std::string::npos);
    Bytes trkn = {0, 0, 0, 3, 0, 12, 0, 0};
    CHECK_MSG(std::search(r.moov.begin(), r.moov.end(), trkn.begin(), trkn.end()) != r.moov.end(), "trkn 3/12");
}
