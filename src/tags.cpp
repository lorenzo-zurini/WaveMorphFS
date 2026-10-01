// SPDX-License-Identifier: GPL-3.0-or-later
#include "tags.hpp"

#include <algorithm>
#include <set>

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

const std::vector<std::string_view> TRACK_SPECIFIC = {
    "TITLE",          "TRACKNUMBER",           "TRACKTOTAL",          "TOTALTRACKS",           "ISRC",
    "CUESHEET",       "LYRICS",                "UNSYNCEDLYRICS",      "MUSICBRAINZ_TRACKID",   "MUSICBRAINZ_RELEASETRACKID",
    "MUSICBRAINZ_WORKID", "ACOUSTID_ID",       "ACOUSTID_FINGERPRINT", "REPLAYGAIN_TRACK_GAIN", "REPLAYGAIN_TRACK_PEAK",
    "LENGTH",         "PART",                  "MOVEMENTNAME",        "MOVEMENT",              "WORK",
};

bool is_track_specific(std::string_view key) {
    std::string c = canonical_field(key);
    return std::any_of(TRACK_SPECIFIC.begin(), TRACK_SPECIFIC.end(), [&](auto n) { return c == n; });
}

// remove `key` under any spelling or synonym
static void erase_field(std::map<std::string, std::vector<std::string>, KeyLess>& m, std::string_view key) {
    std::string c = canonical_field(key);
    std::erase_if(m, [&](auto& kv) { return canonical_field(kv.first) == c; });
}

std::string canonical_field(std::string_view key) {
    static const std::map<std::string, std::string> aliases = [] {
        const std::vector<std::vector<std::string>> groups = {
            {"ALBUMARTIST", "ALBUM_ARTIST", "ALBUM ARTIST", "TPE2", "AART"},
            {"TRACKNUMBER", "TRACK", "TRCK", "TRKN"},
            {"DISCNUMBER", "DISC", "TPOS", "DISK"},
            {"TRACKTOTAL", "TOTALTRACKS", "TRACKC"},
            {"DISCTOTAL", "TOTALDISCS", "DISCC"},
            {"DISCSUBTITLE", "DISC_SUBTITLE", "TSST"},
            {"TITLE", "TIT2", "\u00A9NAM"},
            {"ARTIST", "TPE1", "\u00A9ART"},
            {"ALBUM", "TALB", "\u00A9ALB"},
            {"GENRE", "TCON", "\u00A9GEN"},
            {"COMPOSER", "TCOM", "\u00A9WRT"},
            {"LABEL", "PUBLISHER", "ORGANIZATION", "TPUB"},
            {"COPYRIGHT", "TCOP", "CPRT"},
            {"ISRC", "TSRC"},
            {"MEDIA", "TMED"},
            {"ENCODEDBY", "ENCODED_BY", "TENC"},
            {"ORIGINALDATE", "ORIGINAL_DATE", "TDOR"},
            {"MOVEMENTNAME", "MVNM", "\u00A9MVN"},
            {"ALBUMSORT", "ALBUM_SORT", "ALBUM-SORT", "SORT_ALBUM", "TSOA", "SOAL"},
            {"ARTISTSORT", "ARTIST_SORT", "ARTIST-SORT", "SORT_ARTIST", "TSOP", "SOAR"},
            {"TITLESORT", "TITLE_SORT", "TITLE-SORT", "SORT_NAME", "TSOT", "SONM"},
            {"ALBUMARTISTSORT", "ALBUM_ARTIST_SORT", "ALBUM_ARTIST-SORT", "ALBUM-ARTIST-SORT", "SORT_ALBUM_ARTIST", "TSO2", "SOAA"},
            {"COMPOSERSORT", "COMPOSER_SORT", "COMPOSER-SORT", "SORT_COMPOSER", "TSOC", "SOCM"},
            {"MUSICBRAINZ_ALBUMID", "MUSICBRAINZ ALBUM ID"},
            {"MUSICBRAINZ_ARTISTID", "MUSICBRAINZ ARTIST ID"},
            {"MUSICBRAINZ_ALBUMARTISTID", "MUSICBRAINZ ALBUM ARTIST ID"},
            {"MUSICBRAINZ_TRACKID", "MUSICBRAINZ TRACK ID"},
            {"MUSICBRAINZ_RELEASETRACKID", "MUSICBRAINZ RELEASE TRACK ID"},
            {"MUSICBRAINZ_RELEASEGROUPID", "MUSICBRAINZ RELEASE GROUP ID"},
            {"MUSICBRAINZ_WORKID", "MUSICBRAINZ WORK ID"},
            {"MUSICBRAINZ_DISCID", "MUSICBRAINZ DISC ID"},
            {"RELEASECOUNTRY", "MUSICBRAINZ ALBUM RELEASE COUNTRY"},
            {"RELEASESTATUS", "MUSICBRAINZ ALBUM STATUS"},
            {"RELEASETYPE", "MUSICBRAINZ ALBUM TYPE"},
            {"ACOUSTID_ID", "ACOUSTID ID"},
            {"ACOUSTID_FINGERPRINT", "ACOUSTID FINGERPRINT"},
        };
        std::map<std::string, std::string> m;
        for (auto& g : groups)
            for (auto& a : g) m[a] = g[0];
        return m;
    }();
    std::string u = upper(key);
    auto it = aliases.find(u);
    return it == aliases.end() ? u : it->second;
}

static bool blank(const std::string& v) { return trim(v).empty(); }

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
    std::string c = canonical_field(key);
    for (auto& [k, v] : m)
        if (canonical_field(k) == c) return &v;
    return nullptr;
}

const std::string* Tags::get(std::string_view key) const {
    auto v = get_all(key);
    return v && !v->empty() ? &v->front() : nullptr;
}

void Tags::overlay(const Tags& other) {
    std::set<std::string> fields;
    for (auto& kv : other.m) fields.insert(canonical_field(kv.first));
    std::erase_if(m, [&](auto& kv) { return fields.contains(canonical_field(kv.first)); });
    for (auto& [k, v] : other.m)
        if (!v.empty()) m.emplace(k, v);
}

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
    while (out.size() > 200) {
        out.pop_back();
        while (!out.empty() && (uint8_t(out.back()) & 0xC0) == 0x80) out.pop_back();
        // drop a dangling lead byte left by the loop above
        if (!out.empty() && uint8_t(out.back()) >= 0xC0) out.pop_back();
    }
    return out;
}

}  // namespace wm
