// SPDX-License-Identifier: GPL-3.0-or-later
// Tag model and JSON sidecars.
#include <unistd.h>

#include <thread>

#include "id3.hpp"
#include "library.hpp"
#include "sidecar.hpp"
#include "tags.hpp"
#include "test.hpp"
#include "writeback.hpp"

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

TEST(sidecar_removals_survive_layering) {
    // in-source sidecar sets fields; the tags-dir sidecar removes some of them per track
    fs::path dir = fs::temp_directory_path() / std::format("wm-layer-{}", ::getpid());
    fs::create_directories(dir / "src");
    fs::create_directories(dir / "tags");
    atomic_write(dir / "src" / SIDECAR_NAME, R"({"album": {"MUSICBRAINZ_ALBUMID": "a", "DISCTOTAL": "2", "COMMENT": "x"},
        "track": {"1": {"ALBUMARTISTSORT": "s"}}})");
    atomic_write(dir / "tags" / SIDECAR_NAME, R"({"album": {"COMMENT": ""},
        "track": {"1": {"MUSICBRAINZ_ALBUMID": "", "ALBUMARTISTSORT": "", "DISCTOTAL": ""}},
        "file": {"f.flac": {"MusicBrainz Album Id": ""}}})");
    auto sc = Sidecar::load(dir / "src", dir / "tags");
    CHECK(sc);
    auto removed = [](const Tags* t, const char* k) { return t && t->get_all(k) && t->get_all(k)->empty(); };
    CHECK_MSG(removed(&sc->album, "COMMENT"), "album removal kept");
    CHECK_MSG(removed(sc->track(std::nullopt, 1), "MUSICBRAINZ_ALBUMID"), "track removal kept");
    CHECK_MSG(removed(sc->track(std::nullopt, 1), "ALBUMARTISTSORT"), "removal overrides the in-source track value");
    CHECK_MSG(removed(sc->file("f.flac"), "MusicBrainz Album Id"), "file removal kept, spelling kept");
    // applied like a split track: source tags, then album, then track
    auto tg = Tags::from_pairs({{"MUSICBRAINZ_ALBUMID", "from-image"}, {"COMMENT", "rip"}, {"TITLE", "t"}});
    tg.overlay(sc->album);
    tg.overlay(*sc->track(std::nullopt, 1));
    CHECK(tg.get("MUSICBRAINZ_ALBUMID") == nullptr && tg.get("ALBUMARTISTSORT") == nullptr);
    CHECK(tg.get("DISCTOTAL") == nullptr && tg.get("COMMENT") == nullptr);
    CHECK(*tg.get("TITLE") == "t");
    for (auto& [k, v] : tg.m) CHECK_MSG(!v.empty(), "empty field {} would be emitted", k);
    fs::remove_all(dir);
}

TEST(sidecar_removes_tags_of_regular_files) {
    // a FLAC file's own tags are removed by album and file entries of the tags-dir sidecar
    fs::path dir = fs::temp_directory_path() / std::format("wm-rm-{}", ::getpid());
    fs::path src = dir / "lib" / "Album";
    fs::create_directories(src);
    fs::create_directories(dir / "tags" / "Lib" / "Album");
    fs::path f = src / "01 - a.flac";
    auto st = run({"ffmpeg", "-v", "error", "-f", "lavfi", "-i", "anullsrc=r=44100:cl=stereo", "-t", "0.5", "-c:a", "flac", f.string()});
    CHECK_MSG(st.ok(), "ffmpeg: {}", st.err);
    st = run({"metaflac", "--set-tag=TITLE=keep", "--set-tag=MUSICBRAINZ_ALBUMID=src-id", "--set-tag=ALBUMARTISTSORT=src-sort",
              "--set-tag=ORIGINALDATE=1970", f.string()});
    CHECK_MSG(st.ok(), "metaflac: {}", st.err);
    atomic_write(dir / "tags" / "Lib" / "Album" / SIDECAR_NAME, R"({"album": {"ORIGINALDATE": ""},
        "file": {"01 - a.flac": {"MUSICBRAINZ_ALBUMID": "", "albumartistsort": ""}}})");
    Config cfg;
    cfg.roots = {{"Lib", dir / "lib"}};
    cfg.tags_dir = dir / "tags";
    cfg.cache_dir = dir / "cache";
    cfg.workers = 0;
    auto lib = Library::create(std::move(cfg));
    auto listing = lib->list_dir(src);
    auto e = listing->find("01 - a.flac");
    CHECK(e && e->file);
    auto vf = e->file;
    auto tags = read_file_tags("flac", [&](uint64_t off, size_t len) { return vf->read_at(off, len); }, vf->size());
    CHECK(*tags.get("TITLE") == "keep");
    CHECK_MSG(tags.get("MUSICBRAINZ_ALBUMID") == nullptr, "file removal applied to the source's tag");
    CHECK_MSG(tags.get("ALBUMARTISTSORT") == nullptr, "removal under another spelling applied");
    CHECK_MSG(tags.get("ORIGINALDATE") == nullptr, "album removal applied");
    fs::remove_all(dir);
}

TEST(sidecar_retags_regular_dsf) {
    // a DSF file's own ID3 tag is rebuilt with the sidecar applied; audio bytes stay
    fs::path dir = fs::temp_directory_path() / std::format("wm-dsf-{}", ::getpid());
    fs::path src = dir / "lib" / "Album";
    fs::create_directories(src);
    fs::create_directories(dir / "tags" / "Lib" / "Album");
    Bytes audio(4096 * 2 * 3);
    for (size_t i = 0; i < audio.size(); i++) audio[i] = uint8_t(i * 13 + 1);
    Bytes id3 = id3::build(Tags::from_pairs({{"TITLE", "old"}, {"ARTIST", "a"}, {"MUSICBRAINZ_ALBUMID", "x"}}));
    uint64_t audio_end = 28 + 52 + 12 + audio.size();
    Bytes f;
    append(f, std::string_view("DSD "));
    put_le64(f, 28);
    put_le64(f, audio_end + id3.size());
    put_le64(f, audio_end);
    append(f, std::string_view("fmt "));
    put_le64(f, 52);
    f.resize(f.size() + 40, 0);  // fmt fields (not interpreted when retagging)
    append(f, std::string_view("data"));
    put_le64(f, 12 + audio.size());
    append(f, audio);
    append(f, id3);
    atomic_write(src / "t.dsf", f);
    atomic_write(dir / "tags" / "Lib" / "Album" / SIDECAR_NAME, R"({"file": {"t.dsf": {"TITLE": "new", "MUSICBRAINZ_ALBUMID": ""}}})");
    Config cfg;
    cfg.roots = {{"Lib", dir / "lib"}};
    cfg.tags_dir = dir / "tags";
    cfg.cache_dir = dir / "cache";
    cfg.workers = 0;
    auto lib = Library::create(std::move(cfg));
    auto e = lib->list_dir(src)->find("t.dsf");
    CHECK(e && e->file && e->tag_section == "file" && e->tag_ext == "dsf");
    auto vf = e->file;
    Bytes out = vf->read_at(0, size_t(vf->size()));
    CHECK(out.size() == vf->size());
    CHECK_MSG(le64(&out[12]) == out.size(), "DSD chunk carries the new file size");
    CHECK_MSG(le64(&out[20]) == audio_end, "metadata pointer after the data chunk");
    CHECK_MSG(std::equal(out.begin() + 28, out.begin() + ptrdiff_t(audio_end), f.begin() + 28), "audio bytes unchanged");
    auto tags = read_file_tags("dsf", [&](uint64_t off, size_t len) { return vf->read_at(off, len); }, vf->size());
    CHECK(*tags.get("TITLE") == "new" && *tags.get("ARTIST") == "a");
    CHECK_MSG(tags.get("MUSICBRAINZ_ALBUMID") == nullptr, "removal applied");
    fs::remove_all(dir);
}

TEST(duplicate_cues_and_hidden_files) {
    fs::path dir = fs::temp_directory_path() / std::format("wm-dup-{}", ::getpid());
    fs::path src = dir / "lib" / "Album";
    fs::create_directories(src);
    fs::create_directories(dir / "tags" / "Lib" / "Album");
    auto st = run({"ffmpeg", "-v", "error", "-f", "lavfi", "-i", "sine=f=440:d=6", "-ac", "2", "-ar", "44100", "-sample_fmt", "s16", "-c:a", "flac",
                   (src / "Album.flac").string()});
    CHECK_MSG(st.ok(), "ffmpeg: {}", st.err);
    auto cue = [](const char* file) {
        return std::format("FILE \"{}\" WAVE\n  TRACK 01 AUDIO\n    TITLE \"One\"\n    INDEX 01 00:00:00\n"
                           "  TRACK 02 AUDIO\n    TITLE \"Two\"\n    INDEX 01 00:02:00\n  TRACK 03 AUDIO\n    TITLE \"Three\"\n    INDEX 01 00:04:00\n",
                           file);
    };
    // two sheets for the same image: the one naming the real file wins
    atomic_write(src / "Album.flac.cue", cue("Album.flac"));
    atomic_write(src / "Album.wav.cue", cue("Album.wav"));
    auto old = fs::file_time_type::clock::now() - std::chrono::hours(1);  // not "still being written"
    for (auto& e : fs::directory_iterator(src)) fs::last_write_time(e.path(), old);
    Config cfg;
    cfg.roots = {{"Lib", dir / "lib"}};
    cfg.tags_dir = dir / "tags";
    cfg.cache_dir = dir / "cache";
    cfg.workers = 0;
    auto lib = Library::create(std::move(cfg));
    auto names = [&] {
        std::vector<std::string> v;
        for (auto& e : lib->list_dir(src)->entries) v.push_back(e.name);
        return v;
    };
    CHECK_MSG((names() == std::vector<std::string>{"01 - One.flac", "02 - Two.flac", "03 - Three.flac"}), "listed once, as a single disc");
    auto e = lib->list_dir(src)->find("02 - Two.flac");
    CHECK(e && e->tag_key == "2");
    auto set_sidecar = [&](const char* text) {
        atomic_write(dir / "tags" / "Lib" / "Album" / SIDECAR_NAME, text);
        std::this_thread::sleep_for(std::chrono::milliseconds(1600));  // listings are re-checked after 1.5 s
    };
    // a hidden track, and a hidden mount entry
    set_sidecar(R"({"track": {"1": {"_hide": true}}, "_hide": ["03 - Three.flac"]})");
    CHECK((names() == std::vector<std::string>{"02 - Two.flac"}));
    // hiding the winning sheet falls back to the other one
    set_sidecar(R"({"_hide": "Album.flac.cue"})");
    CHECK((names() == std::vector<std::string>{"01 - One.flac", "02 - Two.flac", "03 - Three.flac"}));
    // hiding the image hides everything made from it
    set_sidecar(R"({"_hide": ["Album.flac"]})");
    CHECK(names().empty());
    fs::remove_all(dir);
}

TEST(targets_place_folders_and_files) {
    fs::path dir = fs::temp_directory_path() / std::format("wm-target-{}", ::getpid());
    fs::path lib_root = dir / "lib", tags = dir / "tags" / "Lib";
    for (auto d : {"Artist/Album/CD1", "Artist/Other", "Misc"}) fs::create_directories(lib_root / d);
    for (auto d : {"Artist/Album", "Misc"}) fs::create_directories(tags / d);
    auto touch = [](const fs::path& p) { atomic_write(p, std::string_view("x")); };
    touch(lib_root / "Artist/Album/cover.jpg");
    touch(lib_root / "Artist/Album/CD1/a.txt");
    touch(lib_root / "Artist/Other/b.txt");
    touch(lib_root / "Misc/song.txt");
    touch(lib_root / "Misc/keep.txt");
    // the album moves (with CD1); one file of Misc joins it, and so does a name clash
    atomic_write(tags / "Artist/Album" / SIDECAR_NAME, R"({"_target": "Lib//Sorted/Composer/ Work (1990) "})");
    atomic_write(tags / "Misc" / SIDECAR_NAME, R"({"file": {"song.txt": {"_target": "Lib/Sorted/Composer/ Work (1990) "},
                                                            "keep.txt": {"_target": "../escape"}}})");
    Config cfg;
    cfg.roots = {{"Lib", lib_root}};
    cfg.tags_dir = dir / "tags";
    cfg.cache_dir = dir / "cache";
    cfg.workers = 0;
    auto lib = Library::create(std::move(cfg));
    lib->refresh_placements(true);
    auto names = [&](const std::string& v) {
        std::vector<std::string> out;
        for (auto& ve : lib->list_vdir(v)->entries) out.push_back(ve.e.name);
        return out;
    };
    CHECK((names("") == std::vector<std::string>{"Lib"}));
    CHECK_MSG((names("Lib") == std::vector<std::string>{"Artist", "Misc", "Sorted"}), "virtual directory created");
    CHECK_MSG((names("Lib/Artist") == std::vector<std::string>{"Other"}), "moved folder gone from its old place");
    CHECK((names("Lib/Sorted/Composer") == std::vector<std::string>{" Work (1990) "}));
    CHECK_MSG((names("Lib/Sorted/Composer/ Work (1990) ") == std::vector<std::string>{"CD1", "cover.jpg", "song.txt"}), "folder, subfolder and file");
    CHECK((names("Lib/Sorted/Composer/ Work (1990) /CD1") == std::vector<std::string>{"a.txt"}));
    CHECK_MSG((names("Lib/Misc") == std::vector<std::string>{"keep.txt"}), "invalid target ignored");
    auto* moved = lib->list_vdir("Lib/Sorted/Composer/ Work (1990) ")->find("song.txt");
    CHECK_MSG(moved && moved->src_dir == lib_root / "Misc", "edits still go to the source folder");
    // a file placed next to one with the same name
    atomic_write(lib_root / "Misc/b.txt", std::string_view("y"));
    atomic_write(tags / "Misc" / SIDECAR_NAME, R"({"file": {"b.txt": {"_target": "Lib/Artist/Other"}}})");
    std::this_thread::sleep_for(std::chrono::milliseconds(1600));  // source listings are re-checked after 1.5 s
    lib->refresh_placements(true);
    CHECK_MSG((names("Lib/Artist/Other") == std::vector<std::string>{"b (2).txt", "b.txt"}), "name clash numbered");
    // the whole Artist folder emptied by moves is hidden
    fs::create_directories(tags / "Artist/Other");
    atomic_write(tags / "Artist/Other" / SIDECAR_NAME, R"({"_target": "Lib/Sorted/Other"})");
    atomic_write(tags / "Misc" / SIDECAR_NAME, "{}");
    lib->refresh_placements(true);
    CHECK_MSG((names("Lib") == std::vector<std::string>{"Misc", "Sorted"}), "emptied folders hidden");
    CHECK((names("Lib/Sorted") == std::vector<std::string>{"Composer", "Other"}));
    fs::remove_all(dir);
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
