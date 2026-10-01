// SPDX-License-Identifier: GPL-3.0-or-later
// Tag model and JSON sidecars.
#include "sidecar.hpp"
#include "tags.hpp"
#include "test.hpp"

using namespace wm;

TEST(tags_overlay_replaces_whole_key) {
    auto a = Tags::from_pairs({{"ARTIST", "A"}, {"ARTIST", "B"}, {"DATE", "1990"}});
    a.overlay(Tags::from_pairs({{"artist", "C"}}));
    CHECK(*a.get_all("ARTIST") == std::vector<std::string>{"C"});
    CHECK(*a.get("date") == "1990");
}

TEST(tags_keep_spelling_but_match_case_insensitively) {
    auto t = Tags::from_pairs({{"MusicBrainz Album Id", "x"}, {"album_artist", "y"}});
    CHECK(*t.get("MUSICBRAINZ ALBUM ID") == "x");
    auto pairs = t.to_pairs();
    auto has = [&](const char* n) { return std::any_of(pairs.begin(), pairs.end(), [&](auto& p) { return p.first == n; }); };
    CHECK(has("MusicBrainz Album Id") && has("album_artist"));
    t.overlay(Tags::from_pairs({{"ALBUM_ARTIST", "z"}}));
    CHECK(*t.get_all("album_artist") == std::vector<std::string>{"z"});
}

TEST(tags_synonyms_replace_but_keep_spelling) {
    // SACD text says ALBUMARTIST; the sidecar (harvested with ffprobe names) says album_artist
    auto base = Tags::from_pairs({{"ALBUMARTIST", "SIR ADRIAN BOULT"}, {"YEAR", "1978"}, {"DATE", "1978-01-01"}});
    base.overlay(Tags::from_pairs({{"album_artist", "Gustav Holst"}, {"MusicBrainz Album Id", "x"}}));
    auto pairs = base.to_pairs();
    CHECK(std::count_if(pairs.begin(), pairs.end(), [](auto& p) { return same_field(p.first, "ALBUMARTIST"); }) == 1);
    CHECK(*base.get("ALBUMARTIST") == "Gustav Holst");
    CHECK(base.m.contains("album_artist") && base.m.find("album_artist")->first == "album_artist");
    CHECK(*base.get("MUSICBRAINZ_ALBUMID") == "x");
    // DATE and YEAR are different fields: both stay
    CHECK(*base.get("YEAR") == "1978" && *base.get("DATE") == "1978-01-01");
    // a sidecar carrying both synonyms keeps both of its own entries
    auto t = Tags::from_pairs({{"TRACKNUMBER", "1"}});
    t.overlay(Tags::from_pairs({{"track", "01"}, {"TRCK", "1/9"}}));
    CHECK(t.m.size() == 2 && !t.m.contains("TRACKNUMBER"));
    // set() replaces synonyms; track-specific filtering sees them
    auto ape = Tags::from_pairs({{"Track", "3"}, {"Album Artist", "A"}});
    CHECK(ape.without_track_specific().m.size() == 1);
    ape.set("TRACKNUMBER", "4");
    CHECK(ape.m.size() == 2 && *ape.get("Track") == "4");
}

TEST(tags_sanitize) {
    CHECK(sanitize_name("AC/DC: Back in Black.") == "AC∕DC: Back in Black");
    std::string longname(300, 'a');
    longname += "é";
    CHECK(sanitize_name(longname).size() <= 200);
    std::string multibyte;
    for (int i = 0; i < 150; i++) multibyte += "é";
    auto s = sanitize_name(multibyte);
    CHECK(s.size() <= 200 && s.size() % 2 == 0);
}

TEST(sidecar_parse_and_lookup) {
    auto sc = Sidecar::parse(R"({
        // comments are allowed
        "_comment": "ignored",
        "album": {"ALBUM": "X", "artist": ["A", "B"], "COMMENT": "", "YEAR": 1999},
        "track": {"3": {"TITLE": "Three"}, "2-05": {"TITLE": "Disc two five"}},
        "file": {"a b.flac": {"TITLE": "File"}}
    })");
    CHECK((*sc.album.get_all("ARTIST") == std::vector<std::string>{"A", "B"}));
    CHECK_MSG(sc.album.get_all("COMMENT") && sc.album.get_all("COMMENT")->empty(), "explicit removal kept as empty");
    CHECK(*sc.album.get("YEAR") == "1999");
    CHECK(*sc.track(std::nullopt, 3)->get("TITLE") == "Three");
    CHECK(*sc.track(2, 5)->get("TITLE") == "Disc two five");
    CHECK(*sc.file("a b.flac")->get("TITLE") == "File");
    auto base = Tags::from_pairs({{"COMMENT", "rip info"}});
    base.overlay(sc.album);
    CHECK_MSG(base.get("COMMENT") == nullptr, "removal applied");
}

TEST(sidecar_pinned_track_names) {
    auto sc = Sidecar::parse(R"({"track": {"3": {"_name": "03 - Old.flac", "TITLE": "New"}, "2-05": {"_name": "x.dsf"}}})");
    CHECK(*sc.track_name(std::nullopt, 3) == "03 - Old.flac");
    CHECK(*sc.track_name(2, 5) == "x.dsf");
    CHECK(sc.track_name(std::nullopt, 4) == nullptr);
    CHECK_MSG(sc.track(std::nullopt, 3)->get("_name") == nullptr, "_name is not a tag");
    CHECK(*sc.track(std::nullopt, 3)->get("TITLE") == "New");
}

TEST(sidecar_names_kept_verbatim) {
    auto sc = Sidecar::parse(R"({"file": {"01 - Song.m4a": {"MusicBrainz Album Id": "abc", "album_artist": "Someone", "TXXX:Custom/Tag": "v"}}})");
    auto f = sc.file("01 - Song.m4a");
    CHECK(f);
    auto pairs = f->to_pairs();
    auto has = [&](const char* n) { return std::any_of(pairs.begin(), pairs.end(), [&](auto& p) { return p.first == n; }); };
    CHECK(has("MusicBrainz Album Id") && has("album_artist") && has("TXXX:Custom/Tag"));
    CHECK(*f->get("MUSICBRAINZ ALBUM ID") == "abc");
}

TEST(sidecar_render_roundtrip) {
    auto album = Tags::from_pairs({{"ALBUM", "X"}, {"MusicBrainz Album Id", "id"}});
    std::vector<std::pair<std::string, Tags>> files = {{"01 - a.flac", Tags::from_pairs({{"TITLE", "A"}, {"ARTIST", "P"}, {"ARTIST", "Q"}})}};
    auto sc = Sidecar::parse(Sidecar::render(album, {}, files));
    CHECK(sc.album == album);
    CHECK(*sc.file("01 - a.flac") == files[0].second);
}

#include <unistd.h>

#include "writeback.hpp"

TEST(writeback_tag_changes) {
    auto before = Tags::from_pairs({{"TITLE", "Old"}, {"album_artist", "A"}, {"DATE", "1999"}, {"COMMENT", "x"}});
    // an editor that respells names, changes the title, drops the comment, adds a tag
    auto after = Tags::from_pairs({{"TITLE", "New"}, {"ALBUMARTIST", "A"}, {"DATE", "1999"}, {"My Tag", "y"}});
    auto c = tag_changes(before, after);
    CHECK(c.m.size() == 3);
    CHECK(*c.get("TITLE") == "New");
    CHECK(*c.get("My Tag") == "y");
    CHECK(c.get_all("COMMENT") && c.get_all("COMMENT")->empty());
    CHECK(tag_changes(before, before).empty());
    // what beets/mediafile writes for an unchanged track: synonyms, placeholders, YEAR
    auto src = Tags::from_pairs({{"TITLE", "T"}, {"TRACKNUMBER", "3"}, {"TRACKTOTAL", "12"}, {"ALBUMARTIST", "A"}, {"DATE", "2006"}});
    auto beets = Tags::from_pairs({{"TITLE", "T"}, {"TRACKNUMBER", "3"}, {"TRACK", "3"}, {"TRACKTOTAL", "12"}, {"TOTALTRACKS", "12"},
                                   {"TRACKC", "12"}, {"ALBUMARTIST", "A"}, {"ALBUM ARTIST", "A"}, {"DATE", "2006"}, {"YEAR", "2006"},
                                   {"BPM", "0"}, {"COMPILATION", "0"}, {"DISC", "0"}, {"ORIGINALDATE", "0000"}});
    src.set("DISCNUMBER", "04");
    beets.set("DISCNUMBER", "4");
    auto n = tag_changes(src, beets);
    CHECK_MSG(n.empty(), "{} spurious changes, first {}", n.m.size(), n.m.empty() ? "" : n.m.begin()->first);
}

namespace {
struct MemFile : VFile {
    Bytes data;
    uint64_t size() const override { return data.size(); }
    Bytes read_at(uint64_t off, size_t len) const override {
        if (off >= data.size()) return {};
        return Bytes(data.begin() + ptrdiff_t(off), data.begin() + ptrdiff_t(std::min<uint64_t>(data.size(), off + len)));
    }
    std::string describe() const override { return "mem"; }
};
}  // namespace

TEST(writeback_session_overlay) {
    auto f = std::make_shared<MemFile>();
    for (int i = 0; i < 100; i++) f->data.push_back(uint8_t(i));
    WriteSession ws(f, fs::temp_directory_path() / std::format("wm-edit-test-{}", ::getpid()));
    CHECK(!ws.dirty() && ws.size() == 100);
    Bytes w = {200, 201, 202};
    ws.write(10, w);
    ws.write(12, Bytes{203, 204});   // overlapping write extends the range
    ws.write(150, Bytes{1});         // beyond the end: gap reads as zeros
    CHECK(ws.dirty() && ws.size() == 151);
    Bytes r = ws.read(8, 8);
    CHECK((r == Bytes{8, 9, 200, 201, 203, 204, 14, 15}));
    CHECK(ws.read(99, 3) == (Bytes{99, 0, 0}));
    ws.truncate(11);
    ws.write(13, Bytes{7});           // after shrinking, original bytes no longer show through
    CHECK((ws.read(9, 5) == Bytes{9, 200, 0, 0, 7}));
    fs::remove_all(fs::temp_directory_path() / std::format("wm-edit-test-{}", ::getpid()));
}
