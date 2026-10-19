// SPDX-License-Identifier: GPL-3.0-or-later
// Tag formats and their parsers on malformed input: UTF-8 handling, ID3, MP4,
// APEv2, CUE sheets and charset detection, covers, retagged files.
#include <sys/stat.h>
#include <unistd.h>

#include "apetag.hpp"
#include "charset.hpp"
#include "cue.hpp"
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

Bytes ilst_item(std::string_view atom, uint32_t type, const Bytes& payload) {
    Bytes d;
    put_be32(d, type);
    put_be32(d, 0);
    append(d, payload);
    return box(atom, {box("data", {d})});
}

Bytes freeform(std::string_view name, std::string_view v) {
    return box("----", {box("mean", {Bytes(4, 0), B("com.apple.iTunes")}), box("name", {Bytes(4, 0), B(name)}),
                        box("data", {Bytes{0, 0, 0, 1, 0, 0, 0, 0}, B(v)})});
}

/// A minimal M4A: ftyp, a moov holding just the item list, mdat.
Bytes m4a(std::initializer_list<Bytes> items) {
    Bytes meta = box("meta", {Bytes(4, 0), box("hdlr", {Bytes(25, 0)}), box("ilst", items)});
    return cat({box("ftyp", {B("M4A "), Bytes(4, 0)}), box("moov", {box("udta", {meta})}), box("mdat", {B("AUDIO")})});
}

/// The tags of `file` retagged with `overlay`, read back.
Tags m4a_retagged(const fs::path& dir, const Bytes& file, const Tags& overlay) {
    fs::path p = dir / "t.m4a";
    atomic_write(p, file);
    auto r = mp4::retag_m4a(p, overlay);
    Bytes out(file.begin(), file.begin() + ptrdiff_t(r.moov_pos));
    append(out, r.moov);
    out.insert(out.end(), file.begin() + ptrdiff_t(r.moov_pos + r.moov_len), file.end());
    return mp4::read_tags(reader_of(out), out.size());
}

/// The bytes of segments that are all in memory.
Bytes flat(const std::vector<Seg>& segs) {
    Bytes o;
    for (auto& s : segs) append(o, std::get<Bytes>(s));
    return o;
}

/// An APEv2 item.
Bytes ape_item(std::string_view key, std::string_view value, uint32_t flags = 0) {
    Bytes b;
    put_le32(b, uint32_t(value.size()));
    put_le32(b, flags);
    append(b, key);
    b.push_back(0);
    append(b, value);
    return b;
}

/// An APEv2 tag of `items` (footer only).
Bytes ape_tag(std::initializer_list<Bytes> items) {
    Bytes body = cat(items), footer = B("APETAGEX");
    put_le32(footer, 2000);
    put_le32(footer, uint32_t(body.size() + 32));
    put_le32(footer, uint32_t(items.size()));
    put_le32(footer, 0);
    footer.resize(32);
    return cat({body, footer});
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

TEST(tag_fields_one_table_for_all_formats) {
    CHECK(canonical_field("aART") == "ALBUMARTIST" && canonical_field("TPE2") == "ALBUMARTIST" && canonical_field("album artist") == "ALBUMARTIST");
    CHECK(canonical_field("trkn") == "TRACKNUMBER" && canonical_field("DISK") == "DISCNUMBER" && canonical_field("Custom Tag") == "CUSTOM TAG");
    CHECK(same_field("MIXARTIST", "remixer") && same_field("UNSYNCEDLYRICS", "LYRICS") && same_field("encodersettings", "encoder"));
    CHECK(same_field("Custom", "CUSTOM") && !same_field("Custom", "Custom2"));
    CHECK_MSG(!same_field("YEAR", "DATE") && !same_field("WORK", "GROUPING"), "fields sharing a frame stay fields of their own");
    CHECK(id3_owner("TDRC")->name == "DATE" && id3_owner("TIT1")->name == "GROUPING" && field_spec("year")->id3 == "TDRC");
    CHECK(mp4_owner("\u00A9wrk")->name == "WORK" && mp4_owner("\u00A9day")->name == "DATE" && !id3_owner("TXXX"));
    CHECK(is_track_specific("TOTALTRACKS") && is_track_specific("unsyncedlyrics") && is_track_specific("Part") && !is_track_specific("ALBUM"));
    CHECK(is_number_field("trkn") && is_number_field("TotalDiscs") && !is_number_field("TITLE"));
}

TEST(tag_numbers_fill_only_missing_fields) {
    auto t = Tags::from_pairs({{"TRACKTOTAL", "12"}});
    add_number(t, "TRACKNUMBER", "TRACKTOTAL", "3/12");
    CHECK(value(t, "TRACKNUMBER") == "3" && t.get_all("TRACKTOTAL")->size() == 1);
    add_number(t, "DISCNUMBER", "DISCTOTAL", " /2");
    CHECK(!t.get("DISCNUMBER") && value(t, "DISCTOTAL") == "2");
    std::string total = "12";
    CHECK(join_number("3", &total) == "3/12" && join_number("3/9", &total) == "3/9" && join_number("3", nullptr) == "3");
}

TEST(id3_synonym_spellings_replace_the_frames_they_name) {
    auto [src, len] = id3::parse_tag(id3::build(Tags::from_pairs({{"MusicBrainz Album Id", "old"}, {"ACOUSTID ID", "a"}})));
    id3::apply(src.frames, Tags::from_pairs({{"MUSICBRAINZ_ALBUMID", "new"}}));
    auto t = id3::read_tags(id3::serialize(src.frames));
    CHECK_MSG(!t.m.contains("MusicBrainz Album Id") && *t.m.at("MUSICBRAINZ_ALBUMID").data() == "new", "one MusicBrainz album id, spelled as given");
    CHECK(value(t, "ACOUSTID_ID") == "a");
}

TEST(id3_names_from_other_formats_go_to_their_frames) {
    auto tag = id3::build(Tags::from_pairs({{"DISK", "2"}, {"trkn", "3/12"}, {"aART", "X"}, {"SORT_ALBUM", "S"}, {"\u00A9nam", "T"}}));
    auto ids = frame_ids(id3::parse_tag(tag).first.frames);
    std::sort(ids.begin(), ids.end());
    CHECK_MSG((ids == std::vector<std::string>{"TIT2", "TPE2", "TPOS", "TRCK", "TSOA"}), "no TXXX, no TRKN frame");
    auto t = id3::read_tags(tag);
    CHECK(value(t, "DISCNUMBER") == "2" && value(t, "TRACKNUMBER") == "3" && value(t, "TRACKTOTAL") == "12");
    CHECK(value(t, "ALBUMARTIST") == "X" && value(t, "ALBUMSORT") == "S" && value(t, "TITLE") == "T");
}

TEST(id3_the_owner_of_a_shared_frame_wins) {
    auto t = id3::read_tags(id3::build(Tags::from_pairs({{"YEAR", "1999"}, {"DATE", "2001-02-03"}, {"WORK", "w"}, {"GROUPING", "g"}})));
    CHECK(value(t, "DATE") == "2001-02-03" && t.m.size() == 2 && value(t, "GROUPING") == "g");
    CHECK_MSG(value(id3::read_tags(id3::build(Tags::from_pairs({{"WORK", "w"}}))), "GROUPING") == "w", "WORK alone goes to TIT1");
    // a tag editor's change of TIT1/TPE4 is saved under the frame's field and must show
    for (auto [old_name, edited] : {std::pair{"WORK", "GROUPING"}, std::pair{"MIXARTIST", "REMIXER"}}) {
        auto sidecar = Tags::from_pairs({{old_name, "old"}});
        sidecar.merge(Tags::from_pairs({{edited, "new"}}));
        CHECK_MSG(value(id3::read_tags(id3::build(sidecar)), edited) == "new", "{} edit shown", edited);
    }
}

TEST(id3_lyrics_in_several_languages) {
    auto [src, len] = id3::parse_tag(id3::build(Tags::from_pairs({{"LYRICS-deu", "d"}, {"lyrics-eng", "e"}})));
    auto uslt = [&] { return std::count_if(src.frames.begin(), src.frames.end(), [](auto& f) { return f.sid() == "USLT"; }); };
    CHECK(uslt() == 2);
    id3::apply(src.frames, Tags::from_pairs({{"LYRICS-eng", "e2"}}));
    CHECK_MSG(uslt() == 2, "the German lyrics stay");
    id3::apply(src.frames, Tags::from_pairs({{"LYRICS", "all"}}));
    CHECK_MSG(uslt() == 1 && value(id3::read_tags(id3::serialize(src.frames)), "LYRICS") == "all", "plain LYRICS replaces every language");
}

TEST(mp4_names_from_other_formats_and_synonyms) {
    auto d = scratch("mp4-names");
    Bytes file = m4a({ilst_item("\xA9nam", 1, B("Old")), freeform("MusicBrainz Album Id", "old"), ilst_item("trkn", 0, Bytes{0, 0, 0, 4, 0, 10, 0, 0})});
    auto t = m4a_retagged(d, file, Tags::from_pairs({{"TPE2", "AA"}, {"TRCK", "3/12"}, {"MUSICBRAINZ_ALBUMID", "new"}, {"TIT2", "New"}}));
    CHECK(value(t, "ALBUMARTIST") == "AA" && value(t, "TITLE") == "New" && value(t, "TRACKNUMBER") == "3" && value(t, "TRACKTOTAL") == "12");
    CHECK_MSG(!t.m.contains("MusicBrainz Album Id") && value(t, "MUSICBRAINZ_ALBUMID") == "new", "one album id, spelled as given");
    CHECK_MSG(!t.m.contains("TPE2") && !t.m.contains("TRCK") && !t.m.contains("TIT2"), "no freeform items for known fields");
    fs::remove_all(d);
}

TEST(mp4_values_that_do_not_fit_keep_the_item) {
    auto d = scratch("mp4-values");
    Bytes file = m4a({ilst_item("trkn", 0, Bytes{0, 0, 0, 4, 0, 10, 0, 0}), ilst_item("cpil", 21, Bytes{1}), ilst_item("tmpo", 21, Bytes{0, 120})});
    auto t = m4a_retagged(d, file, Tags::from_pairs({{"TRACKNUMBER", "A1"}, {"COMPILATION", "yes"}, {"BPM", "fast"}}));
    CHECK(value(t, "TRACKNUMBER") == "4" && value(t, "TRACKTOTAL") == "10" && value(t, "COMPILATION") == "1" && value(t, "BPM") == "120");
    t = m4a_retagged(d, file, Tags::from_pairs({{"BPM", "120.6"}, {"COMPILATION", "False"}, {"TRACKTOTAL", "12"}}));
    CHECK(value(t, "BPM") == "121" && value(t, "COMPILATION") == "0" && value(t, "TRACKNUMBER") == "4" && value(t, "TRACKTOTAL") == "12");
    fs::remove_all(d);
}

TEST(ape_pictures_are_located_not_read) {
    std::string picture(8 << 20, 'p');
    Bytes file = cat({B("audio"), ape_tag({ape_item("Title", "T"), ape_item("Cover Art (Front)", std::string("c.jpg\0", 6) + picture, 2)})});
    uint64_t asked = 0;
    auto tag = ape::read([&](uint64_t off, size_t len) {
        asked += len;
        return off >= file.size() ? Bytes{} : Bytes(file.begin() + ptrdiff_t(off), file.begin() + ptrdiff_t(std::min<uint64_t>(file.size(), off + len)));
    }, file.size());
    CHECK(tag && value(tag->tags, "TITLE") == "T" && tag->front_cover && tag->front_cover->len == picture.size() + 6);
    CHECK_MSG(asked < 4096, "read {} bytes", asked);
    // a footer claiming more than the file holds
    Bytes bad = ape_tag({ape_item("Title", "T")});
    bad[bad.size() - 32 + 15] = 0x7F;
    CHECK(!ape::read(reader_of(bad), bad.size()));
}

TEST(ape_year_and_date_share_the_year_item) {
    auto read_back = [](const Tags& t) { auto b = flat(ape::build(t, nullptr, std::nullopt)); return ape::read(reader_of(b), b.size())->tags; };
    auto t = read_back(Tags::from_pairs({{"DATE", "2001-02-03"}}));
    CHECK(t.m.contains("Year") && value(t, "YEAR") == "2001-02-03");
    t = read_back(Tags::from_pairs({{"YEAR", "1971"}, {"DATE", "1980"}}));
    CHECK_MSG(t.m.size() == 1 && value(t, "YEAR") == "1971", "one date, the Year item's own field");
    // a sidecar YEAR replaces the source's Year (an edit is read back as Year)
    auto src = read_back(Tags::from_pairs({{"Year", "1971"}, {"Album Artist", "A"}}));
    src.overlay(Tags::from_pairs({{"YEAR", "1980"}}));
    t = read_back(src);
    CHECK(value(t, "YEAR") == "1980" && t.m.size() == 2 && value(t, "ALBUMARTIST") == "A");
}

TEST(ape_track_numbers_are_read_once) {
    Bytes tag = ape_tag({ape_item("Track", "3/12"), ape_item("TRACKTOTAL", "12"), ape_item("Disc", "/2")});
    auto t = ape::read(reader_of(tag), tag.size())->tags;
    CHECK(value(t, "TRACKNUMBER") == "3" && t.get_all("TRACKTOTAL")->size() == 1);
    CHECK(!t.get("DISCNUMBER") && value(t, "DISCTOTAL") == "2");
    auto b = flat(ape::build(Tags::from_pairs({{"TRACKNUMBER", "3"}, {"TOTALTRACKS", "12"}}), nullptr, std::nullopt));
    CHECK(std::string(b.begin(), b.end()).find(std::string("Track\0" "3/12", 10)) != std::string::npos);
}

TEST(charset_utf16_stays_aligned_after_a_bad_unit) {
    Bytes le = {0xFF, 0xFE, 'A', 0, 0x00, 0xD8, 'B', 0, 'C', 0};  // a lone surrogate between A and B
    CHECK(decode_text(le).text == "A\uFFFDBC");
    Bytes be = {0xFE, 0xFF, 0, 'A', 0xDC, 0x00, 0, 'B'};
    CHECK(decode_text(be).text == "A\uFFFDB");
    CHECK(*decode_as(Bytes{'x', 0, 0x00, 0xDC, 'y', 0}, "UTF-16LE") == "x\uFFFDy");
}

TEST(charset_y_diaeresis_is_french) {
    // Ÿ (0x9F in windows-1252) counted as Ź, so windows-1250's ź won
    auto d = decode_text(B("L'HA\x9F-LES-ROSES"));
    CHECK_MSG(d.encoding == "windows-1252" && d.text == "L'HA\u0178-LES-ROSES", "{} {}", d.encoding, d.text);
}

TEST(cue_sheets_with_mac_line_ends_and_data_tracks) {
    std::string cue = "FILE \"a.flac\" WAVE\r  TRACK 01 MODE1/2352\r    INDEX 01 00:00:00\r"
                      "  TRACK 02 AUDIO\r    REM __NONAUDIO x\r    TITLE \"Two\"\r    INDEX 01 00:10:00\r"
                      "  TRACK 03 AUDIO\r    TITLE \"Three\"\r    INDEX 01 01:00:00\r";
    auto sheet = CueSheet::parse(cue);
    CHECK(sheet.files.size() == 1 && sheet.tracks.size() == 2 && sheet.tracks[0].number == 2 && sheet.tracks[0].fields.at("TITLE") == "Two");
    CHECK(sheet.tracks[1].index01 == 75 * 60);
}

TEST(cue_parser_survives_any_prefix_and_byte_noise) {
    std::string cue = "REM DATE 1991\nPERFORMER \"P\"\nTITLE \"T\"\nFILE \"image.wav\" WAVE\n  TRACK 01 AUDIO\n    TITLE \"One\"\n"
                      "    INDEX 01 00:00:00\n  TRACK 02 AUDIO\n    INDEX 00 01:00:00\n    INDEX 01 01:02:03\n";
    uint32_t x = 12345;
    for (size_t n = 0; n <= cue.size(); n++) {
        std::string variant = cue.substr(0, n);
        if (n % 7 == 3) {
            x = x * 1103515245 + 12345;
            if (!variant.empty()) variant[x % variant.size()] = char(x >> 16);
        }
        try {
            auto sheet = CueSheet::parse_bytes(bytes_of(variant));
            CHECK(!sheet.tracks.empty());
        } catch (const Error&) {
        }
    }
}
