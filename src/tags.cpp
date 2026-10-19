// SPDX-License-Identifier: GPL-3.0-or-later
#include "tags.hpp"

#include <algorithm>

#include "util.hpp"

namespace wm {

bool KeyLess::operator()(std::string_view a, std::string_view b) const {
    size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; i++) {
        unsigned char x = a[i], y = b[i];
        if (x >= 'a' && x <= 'z') x -= 32;
        if (y >= 'a' && y <= 'z') y -= 32;
        if (x != y) return x < y;
    }
    return a.size() < b.size();
}

namespace {

struct Field {
    FieldSpec spec;
    std::vector<std::string_view> synonyms;  // other spellings (compared case-insensitively)
};

// One table for the synonyms and for where ID3 and MP4 keep each field, so that
// the tag writers, the tag readers and the overlay rules agree on what a name
// means. The owners of shared places come first (see FieldSpec). Built on first
// use, like the index over it, so lookups are safe during static initialisation.
const std::vector<Field>& fields() {
    static const std::vector<Field> f = {
        {{"TITLE", "TIT2", "\u00A9nam"}, {}},
        {{"ARTIST", "TPE1", "\u00A9ART"}, {}},
        {{"ALBUMARTIST", "TPE2", "aART"}, {"ALBUM_ARTIST", "ALBUM ARTIST"}},
        {{"ALBUM", "TALB", "\u00A9alb"}, {}},
        {{"DATE", "TDRC", "\u00A9day"}, {}},
        {{"ORIGINALDATE", "TDOR", ""}, {"ORIGINAL_DATE"}},
        {{"RELEASEDATE", "TDRL", ""}, {}},
        {{"GENRE", "TCON", "\u00A9gen"}, {}},
        {{"COMPOSER", "TCOM", "\u00A9wrt"}, {}},
        {{"CONDUCTOR", "TPE3", ""}, {}},
        {{"REMIXER", "TPE4", ""}, {"MIXARTIST"}},
        {{"LYRICIST", "TEXT", ""}, {}},
        {{"LABEL", "TPUB", ""}, {"PUBLISHER", "ORGANIZATION"}},
        {{"COPYRIGHT", "TCOP", "cprt"}, {}},
        {{"ISRC", "TSRC", ""}, {}},
        {{"GROUPING", "TIT1", "\u00A9grp"}, {}},
        {{"SUBTITLE", "TIT3", ""}, {}},
        {{"BPM", "TBPM", "tmpo"}, {}},
        {{"MOOD", "TMOO", ""}, {}},
        {{"ENCODEDBY", "TENC", ""}, {"ENCODED_BY"}},
        // ffprobe reports TSSE as "encoder", Picard writes it as ENCODERSETTINGS
        {{"ENCODER", "TSSE", "\u00A9too"}, {"ENCODERSETTINGS"}},
        {{"COMPILATION", "TCMP", "cpil"}, {}},
        {{"MEDIA", "TMED", ""}, {}},
        {{"LANGUAGE", "TLAN", ""}, {}},
        {{"DISCSUBTITLE", "TSST", ""}, {"DISC_SUBTITLE"}},
        {{"CREATION_TIME", "TDEN", ""}, {}},
        {{"MOVEMENTNAME", "MVNM", "\u00A9mvn"}, {}},
        {{"TITLESORT", "TSOT", "sonm"}, {"TITLE_SORT", "TITLE-SORT", "SORT_NAME"}},
        {{"ALBUMSORT", "TSOA", "soal"}, {"ALBUM_SORT", "ALBUM-SORT", "SORT_ALBUM"}},
        {{"ARTISTSORT", "TSOP", "soar"}, {"ARTIST_SORT", "ARTIST-SORT", "SORT_ARTIST"}},
        {{"ALBUMARTISTSORT", "TSO2", "soaa"}, {"ALBUM_ARTIST_SORT", "ALBUM_ARTIST-SORT", "ALBUM-ARTIST-SORT", "SORT_ALBUM_ARTIST"}},
        {{"COMPOSERSORT", "TSOC", "socm"}, {"COMPOSER_SORT", "COMPOSER-SORT", "SORT_COMPOSER"}},
        {{"COMMENT", "COMM", "\u00A9cmt"}, {}},
        {{"LYRICS", "USLT", "\u00A9lyr"}, {"UNSYNCEDLYRICS"}},
        {{"DESCRIPTION", "", "desc"}, {}},
        {{"SYNOPSIS", "", "ldes"}, {}},
        {{"SHOW", "", "tvsh"}, {}},
        {{"NETWORK", "", "tvnn"}, {}},
        {{"EPISODE_ID", "", "tven"}, {}},
        {{"PURCHASE_DATE", "", "purd"}, {}},
        {{"GAPLESS_PLAYBACK", "", "pgap"}, {}},
        {{"MEDIA_TYPE", "", "stik"}, {}},
        {{"RATING", "", "rtng"}, {}},
        // number and total share TRCK/TPOS and trkn/disk; the formats join them
        {{"TRACKNUMBER", "TRCK", "trkn"}, {"TRACK"}},
        {{"TRACKTOTAL", "", ""}, {"TOTALTRACKS", "TRACKC"}},
        {{"DISCNUMBER", "TPOS", "disk"}, {"DISC"}},
        {{"DISCTOTAL", "", ""}, {"TOTALDISCS", "DISCC"}},
        {{"MUSICBRAINZ_ALBUMID", "", ""}, {"MUSICBRAINZ ALBUM ID"}},
        {{"MUSICBRAINZ_ARTISTID", "", ""}, {"MUSICBRAINZ ARTIST ID"}},
        {{"MUSICBRAINZ_ALBUMARTISTID", "", ""}, {"MUSICBRAINZ ALBUM ARTIST ID"}},
        {{"MUSICBRAINZ_TRACKID", "", ""}, {"MUSICBRAINZ TRACK ID"}},
        {{"MUSICBRAINZ_RELEASETRACKID", "", ""}, {"MUSICBRAINZ RELEASE TRACK ID"}},
        {{"MUSICBRAINZ_RELEASEGROUPID", "", ""}, {"MUSICBRAINZ RELEASE GROUP ID"}},
        {{"MUSICBRAINZ_WORKID", "", ""}, {"MUSICBRAINZ WORK ID"}},
        {{"MUSICBRAINZ_DISCID", "", ""}, {"MUSICBRAINZ DISC ID"}},
        {{"RELEASECOUNTRY", "", ""}, {"MUSICBRAINZ ALBUM RELEASE COUNTRY"}},
        {{"RELEASESTATUS", "", ""}, {"MUSICBRAINZ ALBUM STATUS"}},
        {{"RELEASETYPE", "", ""}, {"MUSICBRAINZ ALBUM TYPE"}},
        {{"ACOUSTID_ID", "", ""}, {"ACOUSTID ID"}},
        {{"ACOUSTID_FINGERPRINT", "", ""}, {"ACOUSTID FINGERPRINT"}},
        // fields that share a place they do not own: written there only without the owner
        {{"YEAR", "TDRC", "\u00A9day"}, {}},
        {{"WORK", "TIT1", "\u00A9wrk"}, {}},
    };
    return f;
}

struct FieldIndex {
    std::map<std::string_view, const FieldSpec*, KeyLess> by_name;
    std::map<std::string_view, const FieldSpec*> id3, mp4;  // place -> owner
};

const FieldIndex& field_index() {
    static const FieldIndex ix = [] {
        FieldIndex x;
        for (auto& f : fields()) {
            x.by_name.emplace(f.spec.name, &f.spec);
            for (auto s : f.synonyms) x.by_name.emplace(s, &f.spec);
            // the owner of a place answers to the place's id as well (TPE2, aART)
            for (auto [place, owners] : {std::pair{f.spec.id3, &x.id3}, std::pair{f.spec.mp4, &x.mp4}})
                if (!place.empty() && owners->emplace(place, &f.spec).second) x.by_name.emplace(place, &f.spec);
        }
        return x;
    }();
    return ix;
}

// Names compared on their canonical form (TRACKTOTAL covers TOTALTRACKS).
const std::vector<std::string_view> TRACK_SPECIFIC = {
    "TITLE",          "TRACKNUMBER",           "TRACKTOTAL",          "ISRC",                  "CUESHEET",
    "LYRICS",         "MUSICBRAINZ_TRACKID",   "MUSICBRAINZ_RELEASETRACKID", "MUSICBRAINZ_WORKID", "ACOUSTID_ID",
    "ACOUSTID_FINGERPRINT", "REPLAYGAIN_TRACK_GAIN", "REPLAYGAIN_TRACK_PEAK", "LENGTH", "PART",
    "MOVEMENTNAME",   "MOVEMENT",              "WORK",
};

// remove `key` under any spelling or synonym
void erase_field(std::map<std::string, std::vector<std::string>, KeyLess>& m, std::string_view key) {
    std::erase_if(m, [&](auto& kv) { return same_field(kv.first, key); });
}

bool blank(const std::string& v) { return trim(v).empty(); }

}  // namespace

const FieldSpec* field_spec(std::string_view key) {
    auto& m = field_index().by_name;
    auto it = m.find(key);
    return it == m.end() ? nullptr : it->second;
}

const FieldSpec* id3_owner(std::string_view id) {
    auto& m = field_index().id3;
    auto it = m.find(id);
    return it == m.end() ? nullptr : it->second;
}

const FieldSpec* mp4_owner(std::string_view atom) {
    auto& m = field_index().mp4;
    auto it = m.find(atom);
    return it == m.end() ? nullptr : it->second;
}

std::string canonical_field(std::string_view key) {
    auto f = field_spec(key);
    return f ? std::string(f->name) : upper(key);
}

bool same_field(std::string_view a, std::string_view b) {
    // every canonical name is in the table too, so an unknown name can only be
    // the same field as another spelling of itself
    auto fa = field_spec(a), fb = field_spec(b);
    return fa || fb ? fa == fb : iequals(a, b);
}

bool is_track_specific(std::string_view key) {
    auto f = field_spec(key);
    std::string_view name = f ? f->name : key;
    return std::any_of(TRACK_SPECIFIC.begin(), TRACK_SPECIFIC.end(), [&](auto n) { return iequals(name, n); });
}

bool is_number_field(std::string_view key) {
    auto f = field_spec(key);
    return f && (f->name == "TRACKNUMBER" || f->name == "TRACKTOTAL" || f->name == "DISCNUMBER" || f->name == "DISCTOTAL");
}

std::string join_number(const std::string& number, const std::string* total) {
    return total && number.find('/') == std::string::npos ? number + "/" + *total : number;
}

void add_number(Tags& t, const std::string& num_field, const std::string& total_field, std::string_view value) {
    auto parts = split(value, '/');
    std::string n = trim(parts[0]), total = parts.size() > 1 ? trim(parts[1]) : "";
    if (!n.empty() && !t.get_all(num_field)) t.add(num_field, n);
    if (!total.empty() && !t.get_all(total_field)) t.add(total_field, total);
}

Tags Tags::from_pairs(const std::vector<std::pair<std::string, std::string>>& pairs) {
    Tags t;
    for (auto& [k, v] : pairs) t.add(k, v);
    return t;
}

void Tags::add(const std::string& key, std::string value) {
    if (blank(value)) return;
    m[key].push_back(std::move(value));
}

void Tags::set(const std::string& key, std::string value) {
    erase_field(m, key);
    if (!blank(value)) m.emplace(key, std::vector<std::string>{std::move(value)});
}

void Tags::set_many(const std::string& key, std::vector<std::string> values) {
    std::erase_if(values, blank);
    erase_field(m, key);
    if (!values.empty()) m.emplace(key, std::move(values));
}

const std::vector<std::string>* Tags::get_all(std::string_view key) const {
    if (auto it = m.find(key); it != m.end()) return &it->second;
    for (auto& [k, v] : m)
        if (same_field(k, key)) return &v;
    return nullptr;
}

const std::string* Tags::get(std::string_view key) const {
    auto v = get_all(key);
    return v && !v->empty() ? &v->front() : nullptr;
}

void Tags::replace_fields_of(const Tags& other, bool keep_removals) {
    std::erase_if(m, [&](auto& kv) { return std::any_of(other.m.begin(), other.m.end(), [&](auto& o) { return same_field(o.first, kv.first); }); });
    for (auto& [k, v] : other.m)
        if (keep_removals || !v.empty()) m.emplace(k, v);
}

void Tags::overlay(const Tags& other) { replace_fields_of(other, false); }

void Tags::merge(const Tags& other) { replace_fields_of(other, true); }

Tags Tags::without_track_specific() const {
    Tags t = *this;
    std::erase_if(t.m, [](auto& kv) { return is_track_specific(kv.first); });
    return t;
}

std::vector<std::pair<std::string, std::string>> Tags::to_pairs() const {
    std::vector<std::pair<std::string, std::string>> out;
    for (auto& [k, vs] : m)
        for (auto& v : vs) out.emplace_back(k, v);
    return out;
}

bool Tags::operator==(const Tags& o) const {
    if (m.size() != o.m.size()) return false;
    for (auto a = m.begin(), b = o.m.begin(); a != m.end(); ++a, ++b)
        if (!iequals(a->first, b->first) || a->second != b->second) return false;
    return true;
}

std::vector<std::pair<std::string, std::string>> vorbis_comments(const Tags& t) {
    auto pairs = t.to_pairs();
    std::erase_if(pairs, [](auto& kv) {
        bool ok = !kv.first.empty() && std::all_of(kv.first.begin(), kv.first.end(), [](char c) { return c >= 0x20 && c <= 0x7D && c != '='; });
        if (!ok) debug("Vorbis comment: tag name \"{}\" cannot be written", kv.first);
        return !ok;
    });
    return pairs;
}

Tags from_cue_disc(const std::map<std::string, std::string>& fields) {
    Tags t;
    for (auto& [k, v] : fields) {
        if (k == "TITLE") t.set("ALBUM", v);
        else if (k == "PERFORMER") {
            t.set("ALBUMARTIST", v);
            t.set("ARTIST", v);
        } else if (k == "SONGWRITER" || k == "COMPOSER") t.set("COMPOSER", v);
        else if (k == "CATALOG") t.set("BARCODE", v);
        else if (k == "DATE" || k == "YEAR") t.set("DATE", v);
        else if (k == "GENRE") t.set("GENRE", v);
        else if (k == "DISCID") t.set("DISCID", v);
        else if (k == "DISCNUMBER") t.set("DISCNUMBER", v);
        else if (k == "TOTALDISCS" || k == "DISCTOTAL") t.set("DISCTOTAL", v);
        else if (k == "LABEL" || k == "PUBLISHER") t.set("LABEL", v);
        else if (k == "CONDUCTOR" || k == "ORCHESTRA" || k == "ARRANGER") t.set(k, v);
        else if (starts_with(k, "REPLAYGAIN_ALBUM") || starts_with(k, "MUSICBRAINZ_")) t.set(k, v);
    }
    return t;
}

Tags from_cue_track(const std::map<std::string, std::string>& fields) {
    Tags t;
    for (auto& [k, v] : fields) {
        if (k == "TITLE") t.set("TITLE", v);
        else if (k == "PERFORMER") t.set("ARTIST", v);
        else if (k == "SONGWRITER" || k == "COMPOSER") t.set("COMPOSER", v);
        else if (k == "ISRC") t.set("ISRC", v);
        else if (k == "CONDUCTOR" || k == "ORCHESTRA" || k == "ARRANGER" || k == "LYRICIST") t.set(k, v);
        else if (starts_with(k, "REPLAYGAIN_TRACK")) t.set(k, v);
    }
    return t;
}

std::string sanitize_name(std::string_view s) {
    std::string out;
    for (char c : s) {
        if (c == '/') out += "∕";
        else if (uint8_t(c) < 0x20) out += ' ';
        else out += c;
    }
    out = trim(out);
    while (!out.empty() && out.back() == '.') out.pop_back();
    // keep names well under the 255-byte limit (room for prefix + extension)
    // (cut at a character boundary: back off continuation bytes)
    if (out.size() > 200) {
        size_t n = 200;
        while (n > 0 && (uint8_t(out[n]) & 0xC0) == 0x80) n--;
        out.resize(n);
    }
    return out;
}

}  // namespace wm
