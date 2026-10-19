// SPDX-License-Identifier: GPL-3.0-or-later
// Library: sidecar keys, tag edits stored in sidecars, listing rules.
#include <unistd.h>

#include <thread>

#include <nlohmann/json.hpp>

#include "library.hpp"
#include "sidecar.hpp"
#include "test.hpp"

using namespace wm;
using json = nlohmann::ordered_json;

namespace {

/// A scratch library: <dir>/lib (root "Lib") with folder Album, and <dir>/tags.
struct Scratch {
    fs::path dir, src, tags;
    explicit Scratch(const char* name) {
        dir = fs::temp_directory_path() / std::format("wm-{}-{}", name, ::getpid());
        fs::remove_all(dir);
        src = dir / "lib" / "Album";
        tags = dir / "tags" / "Lib" / "Album";
        fs::create_directories(src);
        fs::create_directories(tags);
    }
    ~Scratch() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    std::shared_ptr<Library> library(bool mc = false) const {
        Config cfg;
        cfg.roots = {{"Lib", dir / "lib"}};
        cfg.tags_dir = dir / "tags";
        cfg.cache_dir = dir / "cache";
        cfg.workers = 0;
        cfg.recheck = std::chrono::milliseconds(100);  // listings re-checked quickly
        cfg.sacd_multichannel = mc;
        return Library::create(std::move(cfg));
    }
    json tags_sidecar() const { return json::parse(*try_read_text(tags / SIDECAR_NAME)); }
};

Tags tags_of(std::initializer_list<std::pair<std::string, std::string>> pairs) { return Tags::from_pairs(pairs); }

}  // namespace

TEST(sidecar_track_key_spellings_are_one_track) {
    auto sc = Sidecar::parse(R"({"track": {
        "05": {"TITLE": "Old", "COMPOSER": "C"}, "5": {"_name": "x.flac", "ARTIST": "New"},
        "2-5": {"TITLE": "hand"}, "2-05": {"TITLE": "edited"},
        "2.07": {"TITLE": "dot", "_hide": true}, "007": {"_cover": "/c.jpg"}}})");
    auto t = sc.track(std::nullopt, 5);
    CHECK(t && t->get("TITLE") && *t->get("TITLE") == "Old");
    CHECK_MSG(t->get("COMPOSER") && t->get("ARTIST") && *t->get("ARTIST") == "New", "\"05\" and \"5\" merged");
    CHECK(sc.track_name(std::nullopt, 5) && *sc.track_name(std::nullopt, 5) == "x.flac");
    CHECK_MSG(*sc.track(2, 5)->get("TITLE") == "edited", "the later spelling wins");
    CHECK_MSG(sc.track(2, 7) && sc.hides_track(2, 7), "\"2.07\" is disc 2, track 7");
    std::string k7 = track_key(std::nullopt, 7);
    CHECK(sc.cover_for("n", &k7) && *sc.cover_for("n", &k7) == "/c.jpg");
    CHECK(track_key(2, 5) == "2-05" && track_key(std::nullopt, 12) == "12");
}

TEST(edit_goes_to_the_existing_spelling_of_a_track_key) {
    // an edit of track 5 must land in the table the user wrote as "05", not in a
    // new "5" table that would shadow it
    Scratch s("edit-alias");
    atomic_write(s.tags / SIDECAR_NAME, R"({"track": {"05": {"TITLE": "Old", "COMPOSER": "C"}, "2-5": {"TITLE": "two"}}})");
    auto lib = s.library();
    lib->apply_edit(s.src, "05 - Old.flac", "track", "5", tags_of({{"ARTIST", "New"}}));
    lib->apply_edit(s.src, "2-05 - two.flac", "track", "2-05", tags_of({{"TITLE", "Two"}}));
    auto doc = s.tags_sidecar();
    CHECK_MSG(!doc["track"].contains("5") && !doc["track"].contains("2-05"), "no second table: {}", doc.dump());
    CHECK(doc["track"]["05"]["ARTIST"] == "New" && doc["track"]["05"]["TITLE"] == "Old" && doc["track"]["05"]["_name"] == "05 - Old.flac");
    CHECK(doc["track"]["2-5"]["TITLE"] == "Two");
    auto sc = Sidecar::load(s.src, s.tags);
    CHECK(sc && *sc->track(std::nullopt, 5)->get("COMPOSER") == "C" && *sc->track(std::nullopt, 5)->get("ARTIST") == "New");
    CHECK(*sc->track(2, 5)->get("TITLE") == "Two");
}

TEST(edit_keeps_in_source_spellings_and_beats_its_file_table) {
    // the in-source sidecar (read, never written) spells fields its own way and
    // has a file table that is applied after the track table
    Scratch s("edit-insrc");
    atomic_write(s.src / SIDECAR_NAME, R"({"album": {"album_artist": "A"}, "track": {"1": {"Composer": "C"}},
        "file": {"01 - a.flac": {"Title": "From file table"}}})");
    auto lib = s.library();
    lib->apply_edit(s.src, "01 - a.flac", "track", "1",
                    tags_of({{"ALBUMARTIST", "B"}, {"COMPOSER", "D"}, {"TITLE", "Edited"}, {"GENRE", "G"}}));
    auto doc = s.tags_sidecar();
    auto& t = doc["track"]["1"];
    CHECK_MSG(t["album_artist"] == "B" && !t.contains("ALBUMARTIST"), "album spelling of the in-source sidecar: {}", t.dump());
    CHECK_MSG(t["Composer"] == "D" && !t.contains("COMPOSER"), "track spelling of the in-source sidecar");
    CHECK(t["GENRE"] == "G");
    CHECK_MSG(doc["file"]["01 - a.flac"]["Title"] == "Edited", "in-source file table overridden: {}", doc.dump());
    CHECK_MSG(!doc["file"]["01 - a.flac"].contains("GENRE"), "only fields the in-source file table sets");
    // what the listing will apply: track table, then the merged file tables
    auto sc = Sidecar::load(s.src, s.tags);
    Tags tg = tags_of({{"TITLE", "cue title"}});
    tg.overlay(sc->album);
    tg.overlay(*sc->track(std::nullopt, 1));
    tg.overlay(*sc->file("01 - a.flac"));
    CHECK(*tg.get("TITLE") == "Edited" && *tg.get("ALBUMARTIST") == "B" && *tg.get("COMPOSER") == "D");
    CHECK_MSG(fs::file_size(s.src / SIDECAR_NAME) > 0 && try_read_text(s.src / SIDECAR_NAME)->find("Edited") == std::string::npos,
              "the source folder is not written");
}

TEST(edit_drops_its_own_file_table_field) {
    // without an in-source file table, a field in this sidecar's file table would
    // hide the track edit: it is removed there
    Scratch s("edit-own-file");
    atomic_write(s.tags / SIDECAR_NAME, R"({"file": {"01 - a.flac": {"TITLE": "stale", "MOOD": "m"}}})");
    s.library()->apply_edit(s.src, "01 - a.flac", "track", "1", tags_of({{"TITLE", "new"}}));
    auto doc = s.tags_sidecar();
    CHECK(doc["track"]["1"]["TITLE"] == "new");
    CHECK(!doc["file"]["01 - a.flac"].contains("TITLE") && doc["file"]["01 - a.flac"]["MOOD"] == "m");
}

TEST(md5s_are_stored_for_the_version_they_were_read_from) {
    // a file replaced while its MD5s were computed must not get the old ones
    Scratch s("md5-key");
    fs::path f = s.src / "img.flac";
    atomic_write(f, std::string_view("version one"));
    auto old_key = SrcKey::of(f);
    Cache cache(s.dir / "cache");
    Md5Map m;
    m[{0, 10}] = {1};
    atomic_write(f, std::string_view("version two, longer"));
    cache.store_md5s(f, old_key, m);
    CHECK_MSG(cache.load_md5s(f).empty(), "nothing for the current version");
    CHECK(fs::exists(cache.md5_path(f, old_key)));
}

namespace {

void ok(const ProcResult& r) { CHECK_MSG(r.ok(), "{}", r.err); }

/// Make `p` look settled (written long ago).
void age(const fs::path& p) { fs::last_write_time(p, fs::file_time_type::clock::now() - std::chrono::hours(1)); }

void sine(const fs::path& out, const char* codec, double seconds, const char* sample_fmt = "s16") {
    ok(run({"ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i", std::format("sine=f=440:d={}", seconds), "-ac", "2", "-ar", "44100", "-sample_fmt",
            sample_fmt, "-c:a", codec, out.string()}));
    age(out);
}

std::string two_track_cue(const std::string& file, const char* discnumber) {
    return std::format("REM DISCNUMBER {}\nFILE \"{}\" WAVE\n  TRACK 01 AUDIO\n    TITLE \"One\"\n    INDEX 01 00:00:00\n"
                       "  TRACK 02 AUDIO\n    TITLE \"Two\"\n    INDEX 01 00:01:00\n",
                       discnumber, file);
}

std::vector<std::string> names_of(const Listing& l) {
    std::vector<std::string> v;
    for (auto& e : l.entries) v.push_back(e.name);
    return v;
}

void wait_recheck() { std::this_thread::sleep_for(std::chrono::milliseconds(200)); }  // past cfg.recheck

}  // namespace

TEST(variable_blocksize_flac_images_are_decoded) {
    // a split track is a variable-blocksize FLAC: as an image it is decoded and
    // re-encoded, not split by copying frames
    Scratch s("varblock");
    sine(s.src / "a.flac", "flac", 4);
    atomic_write(s.src / "a.cue", two_track_cue("a.flac", "1"));
    age(s.src / "a.cue");
    auto lib = s.library();
    auto split = lib->list_dir(s.src);
    auto t = split->find("01 - One.flac");
    CHECK(t && t->file->describe().starts_with("flac-image:"));
    fs::path v = s.dir / "lib" / "Var";
    fs::create_directories(v);
    atomic_write(v / "v.flac", t->file->read_at(0, size_t(t->file->size())));
    age(v / "v.flac");
    // track 1 of the original is one second long
    atomic_write(v / "v.cue", std::string_view("FILE \"v.flac\" WAVE\n  TRACK 01 AUDIO\n    TITLE \"One\"\n    INDEX 01 00:00:00\n"
                                               "  TRACK 02 AUDIO\n    TITLE \"Two\"\n    INDEX 01 00:00:40\n"));
    age(v / "v.cue");
    auto l = lib->list_dir(v);
    CHECK_MSG((names_of(*l) == std::vector<std::string>{"01 - One.flac", "02 - Two.flac"}), "{}", names_of(*l)[0]);
    CHECK(l->entries[0].file->describe().starts_with("flac-encoded:"));
    fs::path out = s.dir / "out.flac";
    atomic_write(out, l->entries[1].file->read_at(0, size_t(l->entries[1].file->size())));
    CHECK(run({"flac", "-t", "-s", out.string()}).ok());
}

TEST(repeated_disc_numbers_fall_back_to_name_order) {
    Scratch s("discno");
    for (auto n : {"CD1", "CD2"}) {
        sine(s.src / std::format("{}.flac", n), "flac", 3);
        atomic_write(s.src / std::format("{}.cue", n), two_track_cue(std::format("{}.flac", n), "1"));  // both claim disc 1
        age(s.src / std::format("{}.cue", n));
    }
    auto lib = s.library();
    auto l = lib->list_dir(s.src);
    CHECK_MSG((names_of(*l) == std::vector<std::string>{"1-01 - One.flac", "1-02 - Two.flac", "2-01 - One.flac", "2-02 - Two.flac"}), "{}",
              l->entries.empty() ? "" : l->entries.back().name);
    CHECK(l->find("2-02 - Two.flac")->tag_key == "2-02");
}

TEST(generated_names_never_clash) {
    // x.wv and x.tta both become x.flac; x (2).flac exists as a real file
    Scratch s("clash");
    sine(s.src / "x.wv", "wavpack", 1, "s16p");
    sine(s.src / "x.tta", "tta", 1);
    atomic_write(s.src / "x (2).flac", std::string_view("not audio"));
    age(s.src / "x (2).flac");
    auto lib = s.library();
    auto l = lib->list_dir(s.src);
    CHECK_MSG((names_of(*l) == std::vector<std::string>{"x (2).flac", "x (3).flac", "x.flac"}), "{} entries", l->entries.size());
}

TEST(hiding_a_source_hides_what_is_made_from_it) {
    Scratch s("hide-src");
    sine(s.src / "x.wv", "wavpack", 1, "s16p");
    atomic_write(s.tags / SIDECAR_NAME, R"({"_hide": ["x.wv"]})");
    CHECK(s.library()->list_dir(s.src)->entries.empty());
}

TEST(files_still_being_written_are_listed_again_once_settled) {
    Scratch s("settle");
    atomic_write(s.src / "new.mp3", std::string_view("partial"));
    auto lib = s.library();
    auto l = lib->list_dir(s.src);
    CHECK_MSG(l->retry_at && *l->retry_at > Clock::now() + std::chrono::seconds(50), "a fresh file schedules a rebuild");
    age(s.src / "new.mp3");
    fs::path o = s.dir / "lib" / "Old";
    fs::create_directories(o);
    atomic_write(o / "old.mp3", std::string_view("done"));
    age(o / "old.mp3");
    CHECK(!lib->list_dir(o)->retry_at);
}

TEST(broken_sidecar_keeps_the_previous_listing) {
    // a typo while editing must not rename or unhide anything
    Scratch s("broken");
    for (auto n : {"a.txt", "b.txt"}) {
        atomic_write(s.src / n, std::string_view("x"));
        age(s.src / n);
    }
    atomic_write(s.tags / SIDECAR_NAME, R"({"_hide": ["b.txt"]})");
    auto lib = s.library();
    CHECK((names_of(*lib->list_dir(s.src)) == std::vector<std::string>{"a.txt"}));
    atomic_write(s.tags / SIDECAR_NAME, R"({"_hide": ["b.txt"],)");
    wait_recheck();
    CHECK_MSG((names_of(*lib->list_dir(s.src)) == std::vector<std::string>{"a.txt"}), "still hidden");
    atomic_write(s.tags / SIDECAR_NAME, R"({"_hide": []})");
    wait_recheck();
    CHECK((names_of(*lib->list_dir(s.src)) == std::vector<std::string>{"a.txt", "b.txt"}));
}

TEST(removing_a_sidecar_does_not_turn_mtimes_back) {
    // servers re-read files whose mtime moved forward; the tags go back to the
    // source's when the sidecar is deleted, so the mtime must move forward too
    Scratch s("rm-sidecar");
    sine(s.src / "t.flac", "flac", 1);
    atomic_write(s.tags / SIDECAR_NAME, R"({"album": {"ALBUM": "From sidecar"}})");
    auto lib = s.library();
    int64_t before = lib->list_dir(s.src)->find("t.flac")->mtime;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    fs::remove(s.tags / SIDECAR_NAME);
    wait_recheck();
    auto e = lib->list_dir(s.src)->find("t.flac");
    CHECK(e->file->describe().starts_with("passthrough:"));
    CHECK_MSG(e->mtime > before, "{} <= {}", e->mtime, before);
}

TEST(variable_blocksize_images_keep_their_front_cover) {
    Scratch s("varcover");
    sine(s.src / "a.flac", "flac", 4);
    ok(run({"ffmpeg", "-v", "error", "-f", "lavfi", "-i", "color=c=red:s=16x16", "-frames:v", "1", (s.dir / "c.png").string()}));
    ok(run({"metaflac", "--import-picture-from=3||||" + (s.dir / "c.png").string(), (s.src / "a.flac").string()}));
    age(s.src / "a.flac");
    atomic_write(s.src / "a.cue", two_track_cue("a.flac", "1"));
    age(s.src / "a.cue");
    auto lib = s.library();
    auto split = lib->list_dir(s.src);
    auto t = split->find("01 - One.flac");  // variable-blocksize, with the cover
    CHECK(t);
    fs::path v = s.dir / "lib" / "Var";
    fs::create_directories(v);
    atomic_write(v / "v.flac", t->file->read_at(0, size_t(t->file->size())));
    age(v / "v.flac");
    atomic_write(v / "v.cue", std::string_view("FILE \"v.flac\" WAVE\n  TRACK 01 AUDIO\n    INDEX 01 00:00:00\n  TRACK 02 AUDIO\n    INDEX 01 00:00:40\n"));
    age(v / "v.cue");
    auto l = lib->list_dir(v);
    CHECK(l->entries.size() == 2 && l->entries[0].file->describe().starts_with("flac-encoded:"));
    fs::path out = s.dir / "out.flac";
    atomic_write(out, l->entries[0].file->read_at(0, size_t(l->entries[0].file->size())));
    CHECK(run({"flac", "-t", "-s", out.string()}).ok());
    ok(run({"metaflac", "--export-picture-to=" + (s.dir / "back.png").string(), out.string()}));
    CHECK_MSG(try_read_file(s.dir / "back.png") == try_read_file(s.dir / "c.png"), "the image's front cover");
}
