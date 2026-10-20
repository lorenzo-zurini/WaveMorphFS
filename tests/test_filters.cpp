// SPDX-License-Identifier: GPL-3.0-or-later
// "_keep" / "_drop": tags removed from (and allowed again in) what is served,
// at every level from the whole library down to one file.
#include <thread>

#include "id3.hpp"
#include "library.hpp"
#include "sidecar.hpp"
#include "test.hpp"
#include "writeback.hpp"

using namespace wm;

namespace {

/// Sources under lib/ (root "Lib"), sidecars under tags/.
struct Lib {
    wmtest::TempDir tmp{"wm-filters"};
    fs::path src = tmp / "lib", tags = tmp / "tags";
    std::shared_ptr<Library> lib;

    Lib() { fs::create_directories(src); }
    void sidecar(const fs::path& rel, std::string_view json) {
        fs::create_directories(tags / rel);
        atomic_write(tags / rel / SIDECAR_NAME, json);
    }
    Library& open() {
        Config cfg;
        cfg.roots = {{"Lib", src}};
        cfg.tags_dir = tags;
        cfg.cache_dir = tmp / "cache";
        cfg.workers = 0;
        cfg.recheck = std::chrono::milliseconds(100);
        lib = Library::create(std::move(cfg));
        return *lib;
    }
};

/// A short FLAC with the given tags, aged so that it counts as settled.
void flac_with(const fs::path& p, const std::vector<std::string>& tags) {
    fs::create_directories(p.parent_path());
    auto st = run({"ffmpeg", "-v", "error", "-f", "lavfi", "-i", "sine=f=440:d=2", "-c:a", "flac", p.string()});
    CHECK_MSG(st.ok(), "ffmpeg: {}", st.err);
    std::vector<std::string> argv = {"metaflac", "--remove-all-tags"};
    for (auto& t : tags) argv.push_back("--set-tag=" + t);
    argv.push_back(p.string());
    CHECK(run(argv).ok());
    fs::last_write_time(p, fs::file_time_type::clock::now() - std::chrono::hours(1));
}

Tags served(Library& lib, const fs::path& dir, const std::string& name, const std::string& ext) {
    auto l = lib.list_dir(dir);
    auto e = l->find(name);
    CHECK_MSG(e != nullptr, "{} is listed", name);
    if (!e) return {};
    return read_file_tags(ext, [&](uint64_t o, size_t n) { return e->file->read_at(o, n); }, e->file->size());
}

bool has(const Tags& t, const char* k) { return t.get(k) != nullptr; }

}  // namespace

TEST(tag_patterns_match_like_sidecar_names) {
    CHECK(tag_pattern_matches("artists", "ARTISTS"));
    CHECK_MSG(tag_pattern_matches("album_artist", "ALBUMARTIST"), "synonyms are one field");
    CHECK(!tag_pattern_matches("ARTIST", "ARTISTS"));
    CHECK_MSG(tag_pattern_matches("MUSICBRAINZ_*", "MusicBrainz Album Id"), "wildcards see canonical names too");
    CHECK(tag_pattern_matches("musicbrainz_*", "MUSICBRAINZ_RELEASEGROUPID"));
    CHECK(tag_pattern_matches("*SORT", "albumartist_sort"));
    CHECK(tag_pattern_matches("*_CREDIT", "ARTIST_CREDIT"));
    CHECK(tag_pattern_matches("*", "anything"));
    CHECK(!tag_pattern_matches("REPLAYGAIN_*", "TITLE"));
}

TEST(filters_inherit_and_more_specific_levels_decide) {
    Lib l;
    flac_with(l.src / "Comp" / "Album" / "img.flac",
              {"ALBUM=Img", "ARTISTS=a; b", "MUSICBRAINZ_ALBUMID=src-id", "GENRE=Classical", "ASIN=B000"});
    atomic_write(l.src / "Comp" / "Album" / "img.cue",
                 std::string_view("FILE \"img.flac\" WAVE\n  TRACK 01 AUDIO\n    TITLE \"One\"\n    INDEX 01 00:00:00\n"
                                  "  TRACK 02 AUDIO\n    TITLE \"Two\"\n    INDEX 01 00:01:00\n"));
    fs::last_write_time(l.src / "Comp" / "Album" / "img.cue", fs::file_time_type::clock::now() - std::chrono::hours(1));
    l.sidecar("", R"({"_drop": "ASIN"})");                                     // the whole library
    l.sidecar("Lib", R"({"_drop": ["ARTISTS", "MUSICBRAINZ_*"]})");            // the root
    l.sidecar("Lib/Comp/Album", R"({
        "album": {"MusicBrainz Album Id": "chosen"},
        "track": {"1": {"_drop": "GENRE"}, "2": {"ARTISTS": "chosen too"}}
    })");
    auto& lib = l.open();
    fs::path dir = l.src / "Comp" / "Album";
    Tags one = served(lib, dir, "01 - One.flac", "flac"), two = served(lib, dir, "02 - Two.flac", "flac");
    CHECK_MSG(!has(one, "ASIN") && !has(two, "ASIN"), "dropped library-wide");
    CHECK_MSG(!has(one, "ARTISTS"), "dropped for the root");
    CHECK_MSG(two.get("ARTISTS") && *two.get("ARTISTS") == "chosen too", "a value set for a track beats an inherited drop");
    CHECK_MSG(one.get("MUSICBRAINZ_ALBUMID") && *one.get("MUSICBRAINZ_ALBUMID") == "chosen", "a value set in the folder's sidecar beats an inherited drop");
    CHECK_MSG(!has(one, "GENRE") && has(two, "GENRE"), "dropped for one track only");
    CHECK(one.get("TITLE") && *one.get("TITLE") == "One");
}

TEST(filters_whitelist_regular_files) {
    Lib l;
    flac_with(l.src / "A" / "song.flac", {"TITLE=Song", "ARTIST=Someone", "ENCODER=x", "REPLAYGAIN_TRACK_GAIN=-3 dB"});
    auto st = run({"ffmpeg", "-v", "error", "-f", "lavfi", "-i", "sine=d=1", "-c:a", "libmp3lame", "-metadata", "title=Tune", "-metadata", "ARTISTS=a; b",
                   "-metadata", "artist=A", (l.src / "A" / "tune.mp3").string()});
    CHECK_MSG(st.ok(), "ffmpeg: {}", st.err);
    fs::last_write_time(l.src / "A" / "tune.mp3", fs::file_time_type::clock::now() - std::chrono::hours(1));
    // a whitelist for the whole library: only these names are served
    l.sidecar("", R"({"_keep": ["TITLE", "ARTIST", "TRACKNUMBER"]})");
    auto& lib = l.open();
    Tags f = served(lib, l.src / "A", "song.flac", "flac");
    CHECK(has(f, "TITLE") && has(f, "ARTIST") && !has(f, "ENCODER") && !has(f, "REPLAYGAIN_TRACK_GAIN"));
    Tags m = served(lib, l.src / "A", "tune.mp3", "mp3");
    CHECK_MSG(has(m, "TITLE") && has(m, "ARTIST") && !has(m, "ARTISTS"), "the TXXX frame goes too");
}

TEST(filters_files_they_leave_alone_are_served_unchanged) {
    Lib l;
    flac_with(l.src / "A" / "song.flac", {"TITLE=Song"});
    l.sidecar("", R"({"_drop": "ASIN"})");
    auto& lib = l.open();
    auto e = lib.list_dir(l.src / "A")->find("song.flac");
    CHECK(e && e->file->describe().starts_with("passthrough:"));
}

TEST(filters_dropped_tags_are_not_stored_from_edits) {
    Lib l;
    flac_with(l.src / "A" / "song.flac", {"TITLE=Song"});
    l.sidecar("Lib", R"({"_drop": ["ARTISTS", "*_SORT"]})");
    auto& lib = l.open();
    fs::path dir = l.src / "A";
    fs::path sidecar = l.tags / "Lib" / "A" / SIDECAR_NAME;
    // only dropped tags: nothing is written
    lib.apply_edit(dir, "song.flac", std::string(EDIT_FILE), "song.flac", Tags::from_pairs({{"ARTISTS", "x"}, {"ARTIST_SORT", "y"}}));
    CHECK_MSG(!fs::exists(sidecar), "the sidecar stays clean");
    // mixed: the rest is stored
    lib.apply_edit(dir, "song.flac", std::string(EDIT_FILE), "song.flac", Tags::from_pairs({{"ARTISTS", "x"}, {"GENRE", "Jazz"}}));
    auto text = try_read_text(sidecar);
    CHECK(text && text->find("GENRE") != std::string::npos && text->find("ARTISTS") == std::string::npos);
}

TEST(filters_changing_above_relists_and_moves_mtimes) {
    Lib l;
    flac_with(l.src / "A" / "song.flac", {"TITLE=Song", "ASIN=B000"});
    l.sidecar("Lib", R"({"_drop": "GENRE"})");
    auto& lib = l.open();
    fs::path dir = l.src / "A";
    auto before = lib.list_dir(dir);
    CHECK(has(served(lib, dir, "song.flac", "flac"), "ASIN"));
    int64_t mt = before->find("song.flac")->mtime;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    l.sidecar("Lib", R"({"_drop": ["GENRE", "ASIN"]})");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));  // past cfg.recheck
    CHECK_MSG(!has(served(lib, dir, "song.flac", "flac"), "ASIN"), "a root sidecar's change reaches the folder below");
    CHECK_MSG(lib.list_dir(dir)->find("song.flac")->mtime > mt, "and its files' mtimes move forward");
}

TEST(filters_whitelists_add_up_and_drops_still_apply) {
    Lib l;
    flac_with(l.src / "A" / "song.flac", {"TITLE=Song", "ARTIST=Someone", "COMPOSER=Bach", "GENRE=Baroque", "ENCODER=x"});
    flac_with(l.src / "B" / "other.flac", {"TITLE=Other", "ARTIST=Someone", "COMPOSER=Bach", "GENRE=Baroque"});
    l.sidecar("Lib", R"({"_keep": ["TITLE", "ARTIST", "COMPOSER"]})");
    l.sidecar("Lib/A", R"({"_keep": "GENRE", "file": {"song.flac": {"_drop": "COMPOSER"}}})");
    auto& lib = l.open();
    Tags a = served(lib, l.src / "A", "song.flac", "flac"), b = served(lib, l.src / "B", "other.flac", "flac");
    CHECK_MSG(has(a, "GENRE") && !has(b, "GENRE"), "a folder adds to the whitelist it inherits");
    CHECK_MSG(!has(a, "ENCODER"), "names no whitelist lists are dropped");
    CHECK_MSG(!has(a, "COMPOSER") && has(b, "COMPOSER"), "a more specific drop beats an inherited keep");
    CHECK(has(a, "TITLE") && has(b, "TITLE"));
}

namespace {

/// An ID3v2.4 tag of the given frames (id, body), synchsafe sizes.
Bytes id3_tag(const std::vector<std::pair<std::string, Bytes>>& frames) {
    auto ss = [](Bytes& b, uint32_t n) {
        for (int sh : {21, 14, 7, 0}) b.push_back(uint8_t(n >> sh & 0x7F));
    };
    Bytes body;
    for (auto& [id, data] : frames) {
        append(body, id);
        ss(body, uint32_t(data.size()));
        body.push_back(0);
        body.push_back(0);
        append(body, data);
    }
    Bytes tag = {'I', 'D', '3', 4, 0, 0};
    ss(tag, uint32_t(body.size()));
    append(tag, body);
    return tag;
}

Bytes text(std::string_view s) { return Bytes(s.begin(), s.end()); }

}  // namespace

TEST(filters_drop_id3_frames_that_are_not_tags) {
    Lib l;
    // an MP3 with private, binary and URL frames next to its tags
    fs::path raw = l.tmp / "raw.mp3";
    CHECK(run({"ffmpeg", "-v", "error", "-f", "lavfi", "-i", "sine=d=1", "-c:a", "libmp3lame", "-write_id3v2", "0", raw.string()}).ok());
    Bytes mp3 = id3_tag({{"TIT2", text(std::string("\x03Tune", 5))},
                         {"PRIV", text(std::string("mailto:uits-info@umusic.com\0<uits/>", 35))},
                         {"PRIV", text(std::string("WM/MediaClassPrimaryID\0xx", 25))},
                         {"GEOB", text(std::string("\x00" "application/x\0f.bin\0Serato Markers\0data", 41))},
                         {"UFID", text(std::string("http://musicbrainz.org\0abc", 26))}});
    append(mp3, *try_read_file(raw));
    fs::create_directories(l.src / "A");
    atomic_write(l.src / "A" / "tune.mp3", mp3);
    fs::last_write_time(l.src / "A" / "tune.mp3", fs::file_time_type::clock::now() - std::chrono::hours(1));
    auto names = id3::other_frame_names(mp3);
    CHECK_MSG((names == std::vector<std::string>{"PRIV:mailto:uits-info@umusic.com", "PRIV:WM/MediaClassPrimaryID", "GEOB:Serato Markers",
                                                  "UFID:http://musicbrainz.org"}),
              "{}", names.size());
    l.sidecar("Lib/A", R"({"_drop": ["PRIV:mailto:uits-info@umusic.com", "GEOB"]})");
    auto& lib = l.open();
    auto e = lib.list_dir(l.src / "A")->find("tune.mp3");
    CHECK(e != nullptr);
    if (!e) return;
    Bytes served = e->file->read_at(0, size_t(e->file->size()));
    auto left = id3::other_frame_names(served);
    CHECK_MSG((left == std::vector<std::string>{"PRIV:WM/MediaClassPrimaryID", "UFID:http://musicbrainz.org"}),
              "by full name and by frame id; other frames stay ({} left)", left.size());
    CHECK(id3::read_tags(served).get("TITLE") && *id3::read_tags(served).get("TITLE") == "Tune");
    CHECK_MSG(tag_pattern_matches("PRIV*", "PRIV:mailto:x") && tag_pattern_matches("PRIV", "PRIV:mailto:x"), "a frame id names its frames");
    CHECK(!tag_pattern_matches("PRIV:other", "PRIV:mailto:x"));
}
