// SPDX-License-Identifier: GPL-3.0-or-later
#include "id3.hpp"

#include <algorithm>
#include <cstring>
#include <map>

namespace wm::id3 {

namespace {

/// Where a tag name goes in ID3v2.4.
struct Target {
    enum Kind { Text, Txxx, Comment, Lyrics, Track, Disc } kind;
    std::string s;  // frame id, TXXX description or lyrics language
    bool operator==(const Target&) const = default;
};

const std::vector<std::string_view> TRACK_NAMES = {"TRACKNUMBER", "TRACK", "TRCK"};
const std::vector<std::string_view> TRACK_TOTALS = {"TRACKTOTAL", "TOTALTRACKS"};
const std::vector<std::string_view> DISC_NAMES = {"DISCNUMBER", "DISC", "TPOS"};
const std::vector<std::string_view> DISC_TOTALS = {"DISCTOTAL", "TOTALDISCS"};

bool in(const std::vector<std::string_view>& v, std::string_view k) { return std::find(v.begin(), v.end(), k) != v.end(); }

/// Tag name -> ID3v2.4 frame. Accepts Vorbis names (TITLE, ALBUMARTIST), the names
/// ffprobe reports for ID3 sources (album_artist, publisher, album-sort, lyrics-eng)
/// and raw text-frame ids (TSRC). Case-insensitive.
Target target_for(const std::string& key) {
    std::string k = upper(key);
    if (in(TRACK_NAMES, k) || in(TRACK_TOTALS, k)) return {Target::Track, ""};
    if (in(DISC_NAMES, k) || in(DISC_TOTALS, k)) return {Target::Disc, ""};
    if (k == "COMMENT") return {Target::Comment, ""};
    if (k == "LYRICS" || k == "UNSYNCEDLYRICS" || starts_with(k, "LYRICS-")) {
        // ffprobe: "lyrics-eng" / "lyrics-<description>-eng"
        auto dash = k.rfind('-');
        std::string lang = dash != std::string::npos && k.size() - dash - 1 == 3 ? k.substr(dash + 1) : "XXX";
        return {Target::Lyrics, lower(lang)};
    }
    static const std::vector<std::pair<std::vector<std::string_view>, const char*>> map = {
        {{"TITLE"}, "TIT2"},
        {{"ARTIST"}, "TPE1"},
        {{"ALBUMARTIST", "ALBUM_ARTIST", "ALBUM ARTIST"}, "TPE2"},
        {{"CONDUCTOR"}, "TPE3"},
        {{"REMIXER", "MIXARTIST"}, "TPE4"},
        {{"ALBUM"}, "TALB"},
        {{"DATE", "YEAR"}, "TDRC"},
        {{"ORIGINALDATE", "ORIGINAL_DATE"}, "TDOR"},
        {{"RELEASEDATE"}, "TDRL"},
        {{"GENRE"}, "TCON"},
        {{"COMPOSER"}, "TCOM"},
        {{"LYRICIST"}, "TEXT"},
        {{"LABEL", "PUBLISHER", "ORGANIZATION"}, "TPUB"},
        {{"COPYRIGHT"}, "TCOP"},
        {{"ISRC"}, "TSRC"},
        {{"WORK", "GROUPING"}, "TIT1"},
        {{"SUBTITLE"}, "TIT3"},
        {{"BPM"}, "TBPM"},
        {{"MOOD"}, "TMOO"},
        {{"ENCODEDBY", "ENCODED_BY"}, "TENC"},
        {{"ENCODER", "ENCODERSETTINGS"}, "TSSE"},
        {{"COMPILATION"}, "TCMP"},
        {{"ALBUMSORT", "ALBUM_SORT", "ALBUM-SORT"}, "TSOA"},
        {{"ARTISTSORT", "ARTIST_SORT", "ARTIST-SORT"}, "TSOP"},
        {{"TITLESORT", "TITLE_SORT", "TITLE-SORT"}, "TSOT"},
        {{"ALBUMARTISTSORT", "ALBUM_ARTIST_SORT", "ALBUM_ARTIST-SORT", "ALBUM-ARTIST-SORT"}, "TSO2"},
        {{"COMPOSERSORT", "COMPOSER_SORT", "COMPOSER-SORT"}, "TSOC"},
        {{"MOVEMENTNAME"}, "MVNM"},
        {{"MEDIA"}, "TMED"},
        {{"LANGUAGE"}, "TLAN"},
        {{"DISCSUBTITLE"}, "TSST"},
        {{"CREATION_TIME"}, "TDEN"},
    };
    for (auto& [names, id] : map)
        if (in(names, k)) return {Target::Text, id};
    // a raw text-frame id such as TSRC or TMED
    bool raw = k.size() == 4 && k[0] == 'T' && k != "TXXX" && std::all_of(k.begin(), k.end(), [](char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); });
    return raw ? Target{Target::Text, k} : Target{Target::Txxx, key};
}

std::array<uint8_t, 4> synchsafe(uint32_t n) { return {uint8_t(n >> 21 & 0x7F), uint8_t(n >> 14 & 0x7F), uint8_t(n >> 7 & 0x7F), uint8_t(n & 0x7F)}; }

uint32_t unsynchsafe(const uint8_t* b) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v = v << 7 | (b[i] & 0x7F);
    return v;
}

Bytes deunsync(std::span<const uint8_t> b) {
    Bytes out;
    out.reserve(b.size());
    for (size_t i = 0; i < b.size(); i++) {
        out.push_back(b[i]);
        if (b[i] == 0xFF && i + 1 < b.size() && b[i + 1] == 0) i++;
    }
    return out;
}

/// Split at the encoding's string terminator.
std::pair<std::span<const uint8_t>, std::span<const uint8_t>> split_terminated(uint8_t enc, std::span<const uint8_t> b) {
    if (enc == 1 || enc == 2) {
        for (size_t i = 0; i + 1 < b.size(); i += 2)
            if (b[i] == 0 && b[i + 1] == 0) return {b.subspan(0, i), b.subspan(i + 2)};
        return {b, {}};
    }
    auto it = std::find(b.begin(), b.end(), 0);
    if (it == b.end()) return {b, {}};
    size_t i = size_t(it - b.begin());
    return {b.subspan(0, i), b.subspan(i + 1)};
}

std::string decode_text(uint8_t enc, std::span<const uint8_t> b) {
    std::string o;
    if (enc == 0) {
        for (auto c : b) put_utf8(o, c);
        return o;
    }
    if (enc == 1 || enc == 2) {
        bool be = enc == 2;
        if (b.size() >= 2 && ((b[0] == 0xFF && b[1] == 0xFE) || (b[0] == 0xFE && b[1] == 0xFF))) {
            be = b[0] == 0xFE;
            b = b.subspan(2);
        }
        for (size_t i = 0; i + 1 < b.size(); i += 2) {
            char32_t u = be ? char32_t(b[i] << 8 | b[i + 1]) : char32_t(b[i + 1] << 8 | b[i]);
            if (u >= 0xD800 && u < 0xDC00 && i + 3 < b.size()) {
                char32_t lo = be ? char32_t(b[i + 2] << 8 | b[i + 3]) : char32_t(b[i + 3] << 8 | b[i + 2]);
                if (lo >= 0xDC00 && lo < 0xE000) {
                    u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                    i += 2;
                }
            }
            put_utf8(o, (u >= 0xD800 && u < 0xE000) ? 0xFFFD : u);
        }
        return o;
    }
    return lossy_utf8(b);
}

/// Frame body without a data length indicator; nullopt if compressed/encrypted.
std::optional<std::span<const uint8_t>> body_of(const Frame& f) {
    if (f.flags[1] & 0x0C) return std::nullopt;
    if (f.flags[1] & 0x01) {
        if (f.data.size() < 4) return std::nullopt;
        return std::span(f.data).subspan(4);
    }
    return std::span<const uint8_t>(f.data);
}

/// Description of a TXXX/COMM/USLT frame.
std::optional<std::string> description(const Frame& f) {
    auto b = body_of(f);
    if (!b || b->empty()) return std::nullopt;
    uint8_t enc = (*b)[0];
    size_t skip = f.sid() == "TXXX" ? 1 : 4;
    if (b->size() < skip) return std::nullopt;
    return decode_text(enc, split_terminated(enc, b->subspan(skip)).first);
}

Frame make_frame(const char* id, Bytes data) {
    Frame f;
    std::memcpy(f.id, id, 4);
    f.data = std::move(data);
    return f;
}

std::string joined(const std::vector<std::string>& vs, std::string_view sep) {
    std::string o;
    for (size_t i = 0; i < vs.size(); i++) {
        if (i) o += sep;
        o += vs[i];
    }
    return o;
}

Frame text_frame(const std::string& id, const std::vector<std::string>& values) {
    Bytes body = {3};  // UTF-8
    append(body, joined(values, std::string_view("\0", 1)));
    return make_frame(id.c_str(), std::move(body));
}

Frame txxx_frame(const std::string& desc, const std::vector<std::string>& values) {
    Bytes body = {3};
    append(body, desc);
    body.push_back(0);
    append(body, joined(values, std::string_view("\0", 1)));
    return make_frame("TXXX", std::move(body));
}

/// COMM / USLT: encoding, language, empty description, text.
Frame lang_frame(const char* id, const std::string& lang, const std::string& text) {
    Bytes body = {3};
    for (size_t i = 0; i < 3; i++) body.push_back(i < lang.size() ? uint8_t(lang[i]) : 'X');
    body.push_back(0);
    append(body, text);
    return make_frame(id, std::move(body));
}

std::optional<std::string> frame_text(const std::vector<Frame>& frames, std::string_view id) {
    for (auto& f : frames)
        if (f.sid() == id && !f.data.empty()) {
            std::string t = decode_text(f.data[0], std::span(f.data).subspan(1));
            while (!t.empty() && t.back() == '\0') t.pop_back();
            return trim(t);
        }
    return std::nullopt;
}

/// Replace the ID3v2.3-only date frames with their 2.4 counterparts.
std::vector<Frame> upgrade_v23(std::vector<Frame> frames) {
    auto year = frame_text(frames, "TYER");
    auto ddmm = frame_text(frames, "TDAT");
    auto tory = frame_text(frames, "TORY");
    std::erase_if(frames, [](const Frame& f) {
        auto i = f.sid();
        return i == "TYER" || i == "TDAT" || i == "TIME" || i == "TRDA" || i == "TORY" || i == "TSIZ";
    });
    auto has = [&](std::string_view id) { return std::any_of(frames.begin(), frames.end(), [&](auto& f) { return f.sid() == id; }); };
    if (year && !year->empty() && !has("TDRC")) {
        std::string date = *year;
        if (ddmm && ddmm->size() == 4 && std::all_of(ddmm->begin(), ddmm->end(), [](char c) { return c >= '0' && c <= '9'; }))
            date += "-" + ddmm->substr(2, 2) + "-" + ddmm->substr(0, 2);
        frames.push_back(text_frame("TDRC", {date}));
    }
    if (tory && !tory->empty() && !has("TDOR")) frames.push_back(text_frame("TDOR", {*tory}));
    return frames;
}

}  // namespace

std::pair<SourceTag, size_t> parse_tag(std::span<const uint8_t> b) {
    WM_ENSURE(b.size() >= 10, "truncated ID3 tag");
    uint8_t major = b[3], hflags = b[5];
    size_t size = unsynchsafe(&b[6]);
    size_t total = 10 + size + (major == 4 && (hflags & 0x10) ? 10 : 0);
    WM_ENSURE(major == 3 || major == 4, "ID3v2.{} tag", major);
    WM_ENSURE(b.size() >= 10 + size, "truncated ID3 tag");
    Bytes body(b.begin() + 10, b.begin() + 10 + ptrdiff_t(size));
    if (major == 3 && (hflags & 0x80)) body = deunsync(body);
    size_t pos = 0;
    if ((hflags & 0x40) && body.size() >= 4) pos = major == 3 ? 4 + be32(&body[0]) : unsynchsafe(&body[0]);
    SourceTag tag;
    while (pos + 10 <= body.size() && body[pos] != 0) {
        Frame f;
        std::memcpy(f.id, &body[pos], 4);
        size_t fs = major == 4 ? unsynchsafe(&body[pos + 4]) : be32(&body[pos + 4]);
        f.flags[0] = body[pos + 8];
        f.flags[1] = body[pos + 9];
        WM_ENSURE(pos + 10 + fs <= body.size(), "frame overruns tag");
        f.data.assign(body.begin() + ptrdiff_t(pos + 10), body.begin() + ptrdiff_t(pos + 10 + fs));
        pos += 10 + fs;
        if (!std::all_of(f.id, f.id + 4, [](char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); })) break;
        if (major == 3) {
            // ID3v2.3: compressed/encrypted/grouped frames have a different layout in 2.4
            if (f.flags[1] & 0xE0) continue;
            f.flags[0] = f.flags[1] = 0;
        }
        tag.frames.push_back(std::move(f));
    }
    if (major == 3) tag.frames = upgrade_v23(std::move(tag.frames));
    tag.flags = major == 4 ? (hflags & 0x80) : 0;
    return {std::move(tag), total};
}

void apply(std::vector<Frame>& frames, const Tags& overlay) {
    auto get = [&](const std::vector<std::string_view>& names) -> const std::string* {
        for (auto n : names)
            if (auto v = overlay.get(n)) return v;
        return nullptr;
    };
    auto named = [&](const std::vector<std::string_view>& names) {
        return std::any_of(names.begin(), names.end(), [&](auto n) { return overlay.get_all(n) != nullptr; });
    };
    // track / disc combine two names into one frame
    for (auto [id, nums, totals] : {std::tuple{"TRCK", &TRACK_NAMES, &TRACK_TOTALS}, std::tuple{"TPOS", &DISC_NAMES, &DISC_TOTALS}}) {
        if (!named(*nums) && !named(*totals)) continue;
        std::optional<std::string> n;
        if (auto v = get(*nums)) n = *v;
        else if (!named(*nums))
            if (auto old = frame_text(frames, id)) n = split(*old, '/')[0];
        std::erase_if(frames, [&](auto& f) { return f.sid() == id; });
        if (n) {
            const std::string* t = get(*totals);
            std::string v = t && n->find('/') == std::string::npos ? *n + "/" + *t : *n;
            frames.push_back(text_frame(id, {v}));
        }
    }
    std::vector<Target> used;
    for (auto& [k, vs] : overlay.m) {
        Target t = target_for(k);
        if (t.kind == Target::Track || t.kind == Target::Disc) continue;
        // two spellings mapping to the same frame: the first one wins
        if (std::find(used.begin(), used.end(), t) != used.end()) continue;
        std::erase_if(frames, [&](const Frame& f) {
            switch (t.kind) {
            case Target::Text: return f.sid() == t.s || (t.s == "TDRC" && (f.sid() == "TYER" || f.sid() == "TDAT" || f.sid() == "TIME" || f.sid() == "TRDA"));
            case Target::Txxx: {
                if (f.sid() != "TXXX") return false;
                auto d = description(f);
                return d && iequals(*d, t.s);
            }
            case Target::Comment: {
                if (f.sid() != "COMM") return false;
                auto d = description(f);
                return d && d->empty();
            }
            case Target::Lyrics: return f.sid() == "USLT";
            default: return false;
            }
        });
        if (!vs.empty()) {
            switch (t.kind) {
            case Target::Text: frames.push_back(text_frame(t.s, vs)); break;
            case Target::Txxx: frames.push_back(txxx_frame(t.s, vs)); break;
            case Target::Comment: frames.push_back(lang_frame("COMM", "eng", joined(vs, "\n"))); break;
            case Target::Lyrics: frames.push_back(lang_frame("USLT", t.s, joined(vs, "\n"))); break;
            default: break;
            }
        }
        used.push_back(t);
    }
}

Bytes serialize(uint8_t flags, const std::vector<Frame>& frames) {
    Bytes body;
    for (auto& f : frames) {
        body.insert(body.end(), f.id, f.id + 4);
        append(body, synchsafe(uint32_t(f.data.size())));
        body.push_back(f.flags[0]);
        body.push_back(f.flags[1]);
        append(body, f.data);
    }
    body.insert(body.end(), 4096, 0);  // padding, so tag editors can save in place
    Bytes out = {'I', 'D', '3', 4, 0, flags};
    append(out, synchsafe(uint32_t(body.size())));
    append(out, body);
    return out;
}

Tags read_tags(std::span<const uint8_t> tag) {
    static const std::map<std::string, std::string> names = {
        {"TIT2", "TITLE"}, {"TPE1", "ARTIST"}, {"TPE2", "ALBUMARTIST"}, {"TPE3", "CONDUCTOR"}, {"TPE4", "REMIXER"},
        {"TALB", "ALBUM"}, {"TDRC", "DATE"}, {"TDOR", "ORIGINALDATE"}, {"TDRL", "RELEASEDATE"}, {"TCON", "GENRE"},
        {"TCOM", "COMPOSER"}, {"TEXT", "LYRICIST"}, {"TPUB", "LABEL"}, {"TCOP", "COPYRIGHT"}, {"TSRC", "ISRC"},
        {"TIT1", "GROUPING"}, {"TIT3", "SUBTITLE"}, {"TBPM", "BPM"}, {"TMOO", "MOOD"}, {"TENC", "ENCODEDBY"},
        {"TSSE", "ENCODER"}, {"TCMP", "COMPILATION"}, {"TSOA", "ALBUMSORT"}, {"TSOP", "ARTISTSORT"}, {"TSOT", "TITLESORT"},
        {"TSO2", "ALBUMARTISTSORT"}, {"TSOC", "COMPOSERSORT"}, {"MVNM", "MOVEMENTNAME"}, {"TMED", "MEDIA"},
        {"TLAN", "LANGUAGE"}, {"TSST", "DISCSUBTITLE"}, {"TDEN", "CREATION_TIME"},
    };
    Tags t;
    if (tag.size() < 10 || std::memcmp(tag.data(), "ID3", 3) != 0) return t;
    auto frames = parse_tag(tag).first.frames;
    auto values = [](std::span<const uint8_t> b) {
        std::vector<std::string> out;
        if (b.empty()) return out;
        uint8_t enc = b[0];
        auto rest = b.subspan(1);
        while (!rest.empty()) {
            auto [v, next] = split_terminated(enc, rest);
            std::string s = decode_text(enc, v);
            if (!s.empty()) out.push_back(s);
            if (next.size() == rest.size()) break;
            rest = next;
        }
        return out;
    };
    for (auto& f : frames) {
        auto body = body_of(f);
        if (!body || body->empty()) continue;
        std::string id(f.sid());
        if (id == "TXXX") {
            uint8_t enc = (*body)[0];
            auto [d, rest] = split_terminated(enc, body->subspan(1));
            std::vector<uint8_t> tmp = {enc};
            tmp.insert(tmp.end(), rest.begin(), rest.end());
            for (auto& v : values(tmp)) t.add(decode_text(enc, d), v);
        } else if (id == "COMM" || id == "USLT") {
            uint8_t enc = (*body)[0];
            if (body->size() < 4) continue;
            auto [d, text] = split_terminated(enc, body->subspan(4));
            if (id == "COMM" && !decode_text(enc, d).empty()) continue;  // described comments are not "the" comment
            std::string v = decode_text(enc, text);
            while (!v.empty() && v.back() == '\0') v.pop_back();  // optional terminator
            t.add(id == "COMM" ? "COMMENT" : "LYRICS", v);
        } else if (id == "TRCK" || id == "TPOS") {
            auto vs = values(*body);
            if (vs.empty()) continue;
            auto p = split(vs[0], '/');
            t.add(id == "TRCK" ? "TRACKNUMBER" : "DISCNUMBER", trim(p[0]));
            if (p.size() > 1) t.add(id == "TRCK" ? "TRACKTOTAL" : "DISCTOTAL", trim(p[1]));
        } else if (id[0] == 'T' || id == "MVNM") {
            auto it = names.find(id);
            for (auto& v : values(*body)) t.add(it != names.end() ? it->second : id, v);
        }
    }
    return t;
}

Bytes build(const Tags& tags) {
    std::vector<Frame> frames;
    apply(frames, tags);
    return serialize(0, frames);
}

std::pair<Bytes, uint64_t> retag_mp3(const fs::path& p, const Tags& overlay) {
    File f(p);
    uint8_t head[10] = {};
    size_t n = f.read_at(head, 10, 0);
    SourceTag src;
    uint64_t start = 0;
    if (n == 10 && std::memcmp(head, "ID3", 3) == 0) {
        size_t total = 10 + unsynchsafe(&head[6]);
        Bytes b(total);
        f.read_exact_at(b.data(), total, 0);
        auto [tag, len] = parse_tag(b);
        src = std::move(tag);
        start = len;
    }
    apply(src.frames, overlay);
    return {serialize(src.flags, src.frames), start};
}

}  // namespace wm::id3
