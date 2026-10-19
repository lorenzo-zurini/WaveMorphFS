// SPDX-License-Identifier: GPL-3.0-or-later
// Tag formats and their parsers on malformed input: UTF-8 handling, ID3, MP4,
// APEv2, CUE sheets and charset detection, covers, retagged files.
#include <sys/stat.h>
#include <unistd.h>

#include "id3.hpp"
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

Bytes B(std::string_view s) { return Bytes(s.begin(), s.end()); }

/// Body of an ID3 text frame: an encoding byte, then the text.
Bytes latin1(std::string_view s) { return cat({Bytes{0}, B(s)}); }
Bytes utf8(std::string_view s) { return cat({Bytes{3}, B(s)}); }

struct RawFrame {
    std::string id;
    Bytes data;
    uint8_t status = 0, format = 0;  // flag bytes (v2.3/2.4)
};

/// An ID3v2.<major> tag of `frames` followed by `tail` (padding); v2.4 frame sizes
/// are synchsafe unless `plain_sizes`, as some writers got wrong.
Bytes id3_tag(uint8_t major, std::initializer_list<RawFrame> frames, bool plain_sizes = false, Bytes tail = Bytes(16, 0)) {
    Bytes body;
    for (auto& f : frames) {
        append(body, f.id);
        uint32_t n = uint32_t(f.data.size());
        if (major == 2) put_be24(body, n);
        else if (major == 3 || plain_sizes) put_be32(body, n);
        else
            for (int sh : {21, 14, 7, 0}) body.push_back(uint8_t(n >> sh & 0x7F));
        if (major > 2) {
            body.push_back(f.status);
            body.push_back(f.format);
        }
        append(body, f.data);
    }
    append(body, tail);
    Bytes tag = {'I', 'D', '3', major, 0, 0};
    for (int sh : {21, 14, 7, 0}) tag.push_back(uint8_t(body.size() >> sh & 0x7F));
    append(tag, body);
    return tag;
}

std::vector<std::string> frame_ids(const std::vector<id3::Frame>& frames) {
    std::vector<std::string> ids;
    for (auto& f : frames) ids.emplace_back(f.sid());
    return ids;
}

const std::string& value(const Tags& t, std::string_view k) {
    static const std::string none = "<none>";
    auto v = t.get(k);
    return v ? *v : none;
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

TEST(id3_v22_tags_are_read_with_their_later_frame_ids) {
    Bytes pic = cat({Bytes{0}, B("JPG"), Bytes{3, 0}, B("JFIF")});
    Bytes tag = id3_tag(2, {{"TT2", latin1("Old Title")}, {"TP1", latin1("Artist")}, {"TYE", latin1("1987")}, {"TRK", latin1("5/9")},
                            {"PIC", pic}, {"RVA", Bytes{1, 2, 3}}});
    auto [src, len] = id3::parse_tag(tag);
    CHECK(len == tag.size());
    CHECK_MSG((frame_ids(src.frames) == std::vector<std::string>{"TIT2", "TPE1", "TRCK", "APIC", "TDRC"}), "frames {}", frame_ids(src.frames).size());
    CHECK(src.frames[3].data == cat({Bytes{0}, B("image/jpeg"), Bytes{0, 3, 0}, B("JFIF")}));
    auto t = id3::read_tags(tag);
    CHECK(value(t, "TITLE") == "Old Title" && value(t, "DATE") == "1987");
    CHECK(value(t, "TRACKNUMBER") == "5" && value(t, "TRACKTOTAL") == "9");
    // a retag writes it as v2.4
    auto out = id3::serialize(src.frames);
    CHECK(out[3] == 4 && value(id3::read_tags(out), "ARTIST") == "Artist");
}

TEST(id3_v24_plain_frame_sizes_are_read) {
    std::string title(200, 'x');  // 200 = 0xC8: not a valid synchsafe byte
    Bytes tag = id3_tag(4, {{"TIT2", utf8(title)}, {"TPE1", utf8("Artist")}}, true);
    auto t = id3::read_tags(tag);
    CHECK(value(t, "TITLE") == title && value(t, "ARTIST") == "Artist");
    // correct synchsafe sizes are still read as such
    Bytes ok = id3_tag(4, {{"TIT2", utf8(title)}, {"TPE1", utf8("Artist")}});
    CHECK(value(id3::read_tags(ok), "TITLE") == title);
}

TEST(id3_junk_after_the_frames_ends_them) {
    Bytes junk = id3_tag(4, {{"TIT2", utf8("Kept")}}, false, B("\xFF\xFEnot a frame at all"));
    auto [src, len] = id3::parse_tag(junk);
    CHECK(len == junk.size() && frame_ids(src.frames) == std::vector<std::string>{"TIT2"});
    // a frame running past the tag: the frames before it are kept
    Bytes over = id3_tag(4, {{"TIT2", utf8("Kept")}}, false, cat({B("TPE1"), Bytes{0x7F, 0x7F, 0x7F, 0x7F, 0, 0}, B("abc")}));
    CHECK(value(id3::read_tags(over), "TITLE") == "Kept");
}

TEST(id3_v23_frame_flags_move_to_their_v24_places) {
    Bytes tag = id3_tag(3, {{"TIT2", cat({Bytes{7}, latin1("Grouped")}), 0, 0x20},
                            {"APIC", cat({Bytes{0, 0, 3, 0xE8}, B("zlib data")}), 0, 0x80},
                            {"PRIV", B("x"), 0x80, 0}});
    auto fr = id3::parse_tag(tag).first.frames;
    CHECK_MSG(frame_ids(fr) == (std::vector<std::string>{"TIT2", "APIC", "PRIV"}), "no frame dropped");
    CHECK(fr[0].flags[1] == 0x40 && fr[0].data[0] == 7);
    CHECK(fr[1].flags[1] == 0x09 && fr[1].data == cat({Bytes{0, 0, 7, 0x68}, B("zlib data")}));  // 1000 synchsafe
    CHECK(fr[2].flags[0] == 0x40 && fr[2].flags[1] == 0);
    CHECK(value(id3::read_tags(tag), "TITLE") == "Grouped");
    // the IPLS list of v2.3 is TIPL in v2.4
    auto ipls = id3::parse_tag(id3_tag(3, {{"IPLS", latin1("producer\0Someone")}})).first.frames;
    CHECK(frame_ids(ipls) == std::vector<std::string>{"TIPL"});
}

TEST(id3_text_after_the_bytes_frame_flags_add_is_read) {
    // v2.4: group id and data length indicator before the text
    Bytes tag = id3_tag(4, {{"TIT2", cat({Bytes{9, 0, 0, 0, 8}, utf8("Title")}), 0, 0x41},
                            {"TRCK", cat({Bytes{0, 0, 0, 4}, utf8("4/9")}), 0, 0x01},
                            {"TXXX", Bytes{3, 0, 'v'}}});
    auto t = id3::read_tags(tag);
    CHECK(value(t, "TITLE") == "Title" && value(t, "TRACKNUMBER") == "4");
    CHECK_MSG(!t.m.contains(""), "a TXXX without a name is no tag");
    auto [src, len] = id3::parse_tag(tag);
    id3::apply(src.frames, Tags::from_pairs({{"TRACKTOTAL", "12"}}));
    CHECK_MSG(value(id3::read_tags(id3::serialize(src.frames)), "TRACKNUMBER") == "4", "number kept from a frame with a length indicator");
}

TEST(id3_tag_length_counts_the_footer) {
    Bytes v24 = {'I', 'D', '3', 4, 0, 0x10, 0, 0, 0, 100};
    CHECK(id3::tag_length(v24) == 120u);
    Bytes v23 = {'I', 'D', '3', 3, 0, 0x10, 0, 0, 1, 0};
    CHECK(id3::tag_length(v23) == 138u);
    CHECK(!id3::tag_length(B("TAGxxxxxxx")) && !id3::tag_length(B("ID3")));
}
