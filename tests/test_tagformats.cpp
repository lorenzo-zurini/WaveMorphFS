// SPDX-License-Identifier: GPL-3.0-or-later
// Tag formats and their parsers on malformed input: UTF-8 handling, ID3, MP4,
// APEv2, CUE sheets and charset detection, covers, retagged files.
#include <sys/stat.h>
#include <unistd.h>

#include "mp4.hpp"
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

Bytes cat(std::initializer_list<Bytes> parts) {
    Bytes o;
    for (auto& p : parts) append(o, p);
    return o;
}

/// An MP4 box (32-bit size) around `parts`.
Bytes box(std::string_view type, std::initializer_list<Bytes> parts = {}) {
    Bytes payload = cat(parts), b;
    put_be32(b, uint32_t(payload.size() + 8));
    append(b, type);
    append(b, payload);
    return b;
}

/// Just the header of a box with a 64-bit size.
Bytes box64(std::string_view type, uint64_t size) {
    Bytes b;
    put_be32(b, 1);
    append(b, type);
    put_be64(b, size);
    return b;
}

/// A reader over `f` that fails instead of looping forever.
ByteReader reader_of(const Bytes& f) {
    auto calls = std::make_shared<int>(0);
    return [&f, calls](uint64_t off, size_t len) -> Bytes {
        WM_ENSURE(++*calls < 1000, "read {} times", *calls);
        if (off >= f.size()) return {};
        return Bytes(f.begin() + ptrdiff_t(off), f.begin() + ptrdiff_t(std::min<uint64_t>(f.size(), off + len)));
    };
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

TEST(mp4_box_sizes_beyond_their_parent_are_refused) {
    auto d = scratch("mp4-sizes");
    auto refused = [&](const char* name, Bytes file) {
        CHECK_MSG(mp4::read_tags(reader_of(file), file.size()).empty(), "{}: no tags", name);
        atomic_write(d / name, file);
        CHECK_THROWS(mp4::retag_m4a(d / name, Tags::from_pairs({{"TITLE", "x"}})));
    };
    // a 64-bit size that wraps the position around to the start of the file
    Bytes wrap = cat({box("free", {Bytes(8, 0)}), box64("junk", ~uint64_t(0) - 15)});
    wrap.resize(64);
    refused("wrap.m4a", wrap);
    // the same inside moov
    refused("inner.m4a", box("moov", {box("free"), box64("junk", ~uint64_t(0) - 7)}));
    // a moov claiming more than the file holds (no allocation of it)
    Bytes huge = box64("moov", uint64_t(1) << 40);
    huge.resize(64);
    refused("huge.m4a", huge);
    // boxes nested far deeper than any real file
    Bytes deep = box("ilst");
    for (int i = 0; i < 40; i++) deep = box("udta", {deep});
    refused("deep.m4a", box("moov", {deep}));
    fs::remove_all(d);
}
