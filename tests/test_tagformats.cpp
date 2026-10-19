// SPDX-License-Identifier: GPL-3.0-or-later
// Tag formats and their parsers on malformed input: UTF-8 handling, ID3, MP4,
// APEv2, CUE sheets and charset detection, covers, retagged files.
#include <sys/stat.h>
#include <unistd.h>

#include "test.hpp"
#include "util.hpp"

using namespace wm;
using namespace std::string_view_literals;

namespace {

fs::path scratch(const char* name) {
    fs::path d = fs::temp_directory_path() / std::format("wm-tagformats-{}-{}", ::getpid(), name);
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

}  // namespace

TEST(utf8_ill_formed_sequences_become_replacement_characters) {
    auto lossy = [](std::string_view s) { return lossy_utf8(bytes_of(s)); };
    CHECK(lossy("caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x8E\xB5") == "caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x8E\xB5");
    CHECK(lossy("\xEF\xBF\xBD") == "\xEF\xBF\xBD");               // a real U+FFFD stays
    CHECK(lossy("a\xC0\x80" "b") == "a��" "b");          // overlong NUL
    CHECK(lossy("\xE0\x80\xAF") == "���");           // overlong '/'
    CHECK(lossy("\xED\xA0\x80") == "���");           // surrogate D800
    CHECK(lossy("\xF4\x90\x80\x80") == "����"); // beyond U+10FFFF
    CHECK(lossy("\xF8\x88\x80\x80\x80") == "�����");
    CHECK(lossy("x\xC3") == "x�");                             // cut short
    size_t len = 0;
    CHECK(utf8_at("\xE3\x80\x80", 0, len) == 0x3000 && len == 3);
    CHECK(trim("\xE3\x80\x80 x \xC2\xA0") == "x");
}

TEST(atomic_write_replaces_and_keeps_permissions) {
    auto d = scratch("atomic");
    fs::path p = d / "wavemorph.json";
    atomic_write(p, "one"sv);
    CHECK(*try_read_text(p) == "one");
    ::chmod(p.c_str(), 0664);
    atomic_write(p, "two"sv);
    struct stat st;
    CHECK(::stat(p.c_str(), &st) == 0 && (st.st_mode & 07777) == 0664);
    CHECK(*try_read_text(p) == "two");
    CHECK(std::distance(fs::directory_iterator(d), fs::directory_iterator()) == 1);  // no temp file left
    CHECK_THROWS(atomic_write(d / "missing-dir" / "x", "y"sv));
    fs::remove_all(d);
}
