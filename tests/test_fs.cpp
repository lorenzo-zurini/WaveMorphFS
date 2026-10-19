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
