// SPDX-License-Identifier: GPL-3.0-or-later
// Library: sidecar keys, tag edits stored in sidecars, listing rules.
#include <unistd.h>

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
