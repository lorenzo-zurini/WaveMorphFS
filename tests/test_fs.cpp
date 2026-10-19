// SPDX-License-Identifier: GPL-3.0-or-later
// The mount tree (placements), tag write-back and the FUSE layer.
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <thread>

#include "fs.hpp"
#include "library.hpp"
#include "test.hpp"
#include "writeback.hpp"

using namespace wm;

namespace {

/// A library in a fresh temporary directory: sources under lib/ (root "Lib"),
/// sidecars under tags/Lib/, removed at the end.
struct Fixture {
    fs::path dir, src, tags;
    std::shared_ptr<Library> lib;

    explicit Fixture(const char* name) {
        dir = fs::temp_directory_path() / std::format("wm-{}-{}", name, ::getpid());
        fs::remove_all(dir);
        src = dir / "lib";
        tags = dir / "tags" / "Lib";
        fs::create_directories(src);
        fs::create_directories(tags);
    }
    ~Fixture() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    Library& open() {
        Config cfg;
        cfg.roots = {{"Lib", src}};
        cfg.tags_dir = dir / "tags";
        cfg.cache_dir = dir / "cache";
        cfg.workers = 0;
        lib = Library::create(std::move(cfg));
        lib->refresh_placements(true);
        return *lib;
    }
    void file(const std::string& rel, std::string_view text = "x") {
        fs::create_directories((src / rel).parent_path());
        atomic_write(src / rel, text);
    }
    void sidecar(const std::string& rel, std::string_view json) {
        fs::create_directories(tags / rel);
        atomic_write(tags / rel / SIDECAR_NAME, json);
    }
    std::vector<std::string> names(const std::string& vpath) {
        std::vector<std::string> out;
        for (auto& ve : lib->list_vdir(vpath)->entries) out.push_back(ve.e.name);
        return out;
    }
};

using Names = std::vector<std::string>;

/// A short FLAC file with the given TITLE.
void make_flac(const fs::path& p, const std::string& title) {
    fs::create_directories(p.parent_path());
    auto st = run({"ffmpeg", "-v", "error", "-f", "lavfi", "-i", "sine=f=440:d=1", "-c:a", "flac", p.string()});
    CHECK_MSG(st.ok(), "ffmpeg: {}", st.err);
    st = run({"metaflac", "--set-tag=TITLE=" + title, p.string()});
    CHECK_MSG(st.ok(), "metaflac: {}", st.err);
}

Tags tags_of(const VFile& vf, const std::string& ext) {
    return read_file_tags(ext, [&](uint64_t o, size_t n) { return vf.read_at(o, n); }, vf.size());
}

}  // namespace

TEST(placement_emptied_folder_hidden_next_to_similar_names) {
    // "A B" sorts between "A" and "A/...": the emptied "A" must still be found to have moves below
    Fixture f("emptied");
    f.file("A/sub1/x.txt");
    f.file("A B/y.txt");
    f.sidecar("A/sub1", R"({"_target": "Lib/Moved"})");
    f.sidecar("A B", R"({"_target": "Lib/Other"})");
    f.open();
    CHECK((f.names("Lib") == Names{"Moved", "Other"}));
}

TEST(placement_top_level_targets_that_place_nothing_hidden) {
    Fixture f("ghost");
    f.file("A/x.txt");
    // a folder that does not exist, a file key that matches nothing, and the root's own target
    f.sidecar("Gone", R"({"_target": "Ghost/Typo"})");
    f.sidecar("A", R"({"file": {"missing.txt": {"_target": "Ghost2/X"}}})");
    f.sidecar("", R"({"_target": "Elsewhere/Lib"})");
    f.open();
    CHECK_MSG((f.names("") == Names{"Lib"}), "got {} entries", f.names("").size());
    CHECK((f.names("Lib") == Names{"A"}));
}

TEST(placement_first_scan_dates_directories_by_their_sidecars) {
    // directories shaped by targets keep their mtime across restarts, so music
    // servers do not rescan every moved folder after each start
    Fixture f("stamp");
    f.file("A/x.txt");
    f.sidecar("A", R"({"_target": "Lib/Sorted/A"})");
    int64_t written = 1'700'000'000LL * 1'000'000'000;
    timespec t[2] = {{1'700'000'000, 0}, {1'700'000'000, 0}};
    CHECK(::utimensat(AT_FDCWD, (f.tags / "A" / SIDECAR_NAME).c_str(), t, 0) == 0);
    auto& lib = f.open();
    CHECK(lib.list_vdir("Lib/Sorted")->mtime == written);
    CHECK(lib.list_vdir("")->mtime == written);
    // later changes are dated when they are seen
    f.sidecar("A", R"({"_target": "Lib/Sorted/B"})");
    int64_t before = now_ns();
    lib.refresh_placements(true);
    CHECK(lib.list_vdir("Lib/Sorted")->mtime >= before);
}

TEST(placement_unreadable_sidecar_read_again) {
    Fixture f("unreadable");
    f.file("A/x.txt");
    f.sidecar("A", R"({"_target": "Lib/Moved"})");
    fs::path sc = f.tags / "A" / SIDECAR_NAME;
    fs::permissions(sc, fs::perms::none);
    if (try_read_text(sc)) {
        std::printf("      (skipped: running as a user that reads anything)\n");
        return;
    }
    f.open();
    CHECK((f.names("Lib") == Names{"A"}));
    // readable again with the same mtime: the targets must not have been forgotten
    fs::permissions(sc, fs::perms::owner_read | fs::perms::owner_write);
    f.lib->refresh_placements(true);
    CHECK((f.names("Lib") == Names{"Moved"}));
}

TEST(placement_numbered_clash_keeps_source_name) {
    // tag edits of a file numbered in the mount are stored under its own name
    Fixture f("clash");
    f.file("A/b.txt");
    f.file("B/b.txt");
    f.sidecar("B", R"({"file": {"b.txt": {"_target": "Lib/A"}}})");
    f.open();
    CHECK((f.names("Lib/A") == Names{"b (2).txt", "b.txt"}));
    auto moved = f.lib->lookup("Lib/A", "b (2).txt");
    CHECK(moved && moved->src_dir == f.src / "B" && moved->src_name == "b.txt");
}

TEST(edit_visible_at_once_after_invalidate) {
    // an editor reads the file back right after saving: it must see its edit
    Fixture f("invalidate");
    make_flac(f.src / "A" / "a.flac", "Old");
    f.sidecar("A", R"({"album": {"ALBUM": "X"}})");
    auto& lib = f.open();
    auto title = [&] {
        auto ve = lib.lookup("Lib/A", "a.flac");
        CHECK(ve && ve->e.file);
        auto t = tags_of(*ve->e.file, "flac");
        return t.get("TITLE") ? *t.get("TITLE") : "";
    };
    CHECK(title() == "Old");
    lib.apply_edit(f.src / "A", "a.flac", "file", "a.flac", Tags::from_pairs({{"TITLE", "New"}}));
    lib.invalidate(f.src / "A");
    CHECK(title() == "New");
}

TEST(writeback_rejects_incomplete_files) {
    // a half-written file must not read as one whose tags were all removed
    auto reader = [](const Bytes& b) {
        return [&b](uint64_t o, size_t n) {
            if (o >= b.size()) return Bytes{};
            return Bytes(b.begin() + ptrdiff_t(o), b.begin() + ptrdiff_t(std::min<uint64_t>(b.size(), o + n)));
        };
    };
    Bytes empty;
    for (auto ext : {"flac", "dsf", "mp3", "m4a", "wv"}) CHECK_THROWS(read_file_tags(ext, reader(empty), 0));
    Bytes flac = {'f', 'L', 'a', 'C', 0x84, 0, 0, 40, 0, 0};  // last block (VORBIS_COMMENT) longer than the file
    CHECK_THROWS(read_file_tags("flac", reader(flac), flac.size()));
    Bytes dsf(28, 0);
    std::memcpy(dsf.data(), "DSD ", 4);
    dsf[20] = 200;  // metadata pointer past the end
    CHECK_THROWS(read_file_tags("dsf", reader(dsf), dsf.size()));
    dsf[20] = 0;  // no metadata: every tag removed
    CHECK(read_file_tags("dsf", reader(dsf), dsf.size()).empty());
    Bytes mp3 = {0xFF, 0xFB, 0x90, 0x00};  // an MP3 without a tag: every tag removed
    CHECK(read_file_tags("mp3", reader(mp3), mp3.size()).empty());
    Bytes mp4 = {0, 0, 0, 16, 'f', 't', 'y', 'p', 'M', '4', 'A', ' ', 0, 0, 0, 0, 0, 0, 1, 0, 'm', 'd', 'a', 't'};  // mdat cut short, no moov
    CHECK_THROWS(read_file_tags("m4a", reader(mp4), mp4.size()));
}

TEST(writeback_write_while_storing_stays_dirty) {
    struct Zeros : VFile {
        uint64_t size() const override { return 10; }
        Bytes read_at(uint64_t off, size_t len) const override { return Bytes(size_t(std::min<uint64_t>(len, off < 10 ? 10 - off : 0)), 0); }
        std::string describe() const override { return "zeros"; }
    };
    fs::path scratch = fs::temp_directory_path() / std::format("wm-dirty-{}", ::getpid());
    {
        WriteSession ws(std::make_shared<Zeros>(), scratch);
        ws.write(0, Bytes{1});
        uint64_t seen = ws.changes();  // an edit is being stored from what this saw...
        ws.write(1, Bytes{2});         // ...when another write lands
        ws.mark_stored(seen);
        CHECK(ws.dirty());
        ws.mark_stored(ws.changes());
        CHECK(!ws.dirty());
    }
    fs::remove_all(scratch);
}

// ---------------------------------------------------------------- a real mount

namespace {

/// The library mounted in a background thread; skipped (nullopt from start())
/// where FUSE is not available (containers without /dev/fuse, docker build).
class Mount {
public:
    static std::unique_ptr<Mount> start(std::shared_ptr<Library> lib, const fs::path& at) {
        if (!fs::exists("/dev/fuse") || !run({"fusermount3", "-V"}).ok()) return nullptr;
        fs::create_directories(at);
        auto m = std::unique_ptr<Mount>(new Mount(at));
        m->thread_ = std::thread([m = m.get(), lib = std::move(lib)] { m->rc_ = mount_fs(lib, {m->at_.string(), 4, false}); });
        struct stat outer, inner;
        CHECK(::stat(at.parent_path().c_str(), &outer) == 0);
        for (int i = 0; i < 100 && m->rc_ < 0; i++) {
            if (::stat(at.c_str(), &inner) == 0 && inner.st_dev != outer.st_dev) return m;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return nullptr;  // mounting failed (no permission to mount here)
    }
    /// Stop as a service manager would; returns mount_fs()'s exit code.
    int stop_with_sigterm() {
        ::kill(::getpid(), SIGTERM);  // the mount's handlers are installed: it answered requests
        thread_.join();
        return rc_;
    }
    ~Mount() {
        if (thread_.joinable()) {
            run({"fusermount3", "-uz", at_.string()});
            thread_.join();
        }
    }

private:
    explicit Mount(fs::path at) : at_(std::move(at)) {}
    fs::path at_;
    std::thread thread_;
    std::atomic<int> rc_ = -1;
};

}  // namespace

TEST(mount_edits_sessions_and_stop) {
    Fixture f("mount");
    make_flac(f.src / "A" / "a.flac", "Old");
    f.sidecar("A", R"({"album": {"ALBUM": "X"}})");  // retagged: generated files carry tag padding
    f.open();
    fs::path mnt = f.dir / "mnt";
    auto m = Mount::start(f.lib, mnt);
    if (!m) {
        std::printf("      (skipped: FUSE is not available here)\n");
        return;
    }
    std::string path = (mnt / "Lib" / "A" / "a.flac").string();
    auto mounted_tags = [&] { return tags_of(Passthrough(path, fs::file_size(path)), "flac"); };
    CHECK(*mounted_tags().get("TITLE") == "Old");

    // an editor saving in place sees its edit when it reads the file back
    auto st = run({"metaflac", "--set-tag=GENRE=Edited", path});
    CHECK_MSG(st.ok(), "metaflac: {}", st.err);
    auto t0 = Clock::now();
    while (!mounted_tags().get("GENRE") && Clock::now() - t0 < std::chrono::seconds(3)) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    auto took = std::chrono::duration<double>(Clock::now() - t0).count();
    CHECK_MSG(mounted_tags().get("GENRE") && took < 1.2, "edit visible after {:.2f}s", took);

    // an editor that truncates the file and dies: nothing is stored, the close reports it
    std::string sidecar = *try_read_text(f.tags / "A" / SIDECAR_NAME);
    int fd = ::open(path.c_str(), O_WRONLY | O_TRUNC);
    CHECK(fd >= 0);
    CHECK(::close(fd) != 0 && errno == EIO);
    CHECK(*try_read_text(f.tags / "A" / SIDECAR_NAME) == sidecar);

    // stat by name while writers come and go (a session freed under stat once)
    std::atomic<bool> stop = false;
    std::thread statter([&] {
        struct stat s;
        while (!stop) ::stat(path.c_str(), &s);
    });
    for (int i = 0; i < 200; i++) {
        int w = ::open(path.c_str(), O_RDWR);
        CHECK(w >= 0);
        ::close(w);
    }
    stop = true;
    statter.join();

    CHECK_MSG(m->stop_with_sigterm() == 0, "SIGTERM is a normal stop");
}
