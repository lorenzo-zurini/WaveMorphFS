// SPDX-License-Identifier: GPL-3.0-or-later
#include "id3.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <tuple>

namespace wm::id3 {

namespace {

/// Padding after the frames, so tag editors can save in place.
constexpr uint64_t PADDING = 4096;
/// Sizes in ID3v2 headers are 28-bit synchsafe integers.
constexpr uint64_t MAX_SIZE = 1u << 28;

// frame format flags (second flag byte) of ID3v2.4 and ID3v2.3
constexpr uint8_t V24_GROUP = 0x40, V24_COMPRESSED = 0x08, V24_ENCRYPTED = 0x04, V24_UNSYNC = 0x02, V24_DATA_LENGTH = 0x01;
constexpr uint8_t V23_COMPRESSED = 0x80, V23_ENCRYPTED = 0x40, V23_GROUP = 0x20;

/// Where a tag name goes in ID3v2.4.
struct Target {
    enum Kind { Text, Txxx, Comment, Lyrics } kind;
    std::string s;      // frame id, TXXX description or lyrics language ("" = any)
    bool owner = true;  // the field owns the frame (see FieldSpec)

    bool same_place(const Target& o) const { return kind == o.kind && (kind == Txxx ? same_field(s, o.s) : iequals(s, o.s)); }
};

/// Tag name -> ID3v2.4 frame. Accepts every spelling field_spec() knows (Vorbis
/// TITLE, ffprobe album_artist, frame ids such as TSRC), ffprobe's "lyrics-eng"
/// and raw text-frame ids. Track and disc numbers are not handled here.
Target target_for(const std::string& key) {
    std::string k = upper(key);
    if (starts_with(k, "LYRICS-")) {
        // ffprobe: "lyrics-eng" / "lyrics-<description>-eng"
        auto dash = k.rfind('-');
        return {Target::Lyrics, k.size() - dash - 1 == 3 ? lower(k.substr(dash + 1)) : ""};
    }
    if (auto f = field_spec(key); f && !f->id3.empty()) {
        bool owner = id3_owner(f->id3) == f;
        if (f->id3 == "COMM") return {Target::Comment, "", owner};
        if (f->id3 == "USLT") return {Target::Lyrics, "", owner};
        return {Target::Text, std::string(f->id3), owner};
    }
    // a raw text-frame id such as TKEY
    bool raw = k.size() == 4 && k[0] == 'T' && k != "TXXX" && std::all_of(k.begin(), k.end(), [](char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); });
    return raw ? Target{Target::Text, k} : Target{Target::Txxx, key};
}

std::array<uint8_t, 4> synchsafe(uint64_t n) {
    WM_ENSURE(n < MAX_SIZE, "ID3 size {} does not fit 28 bits", n);
    return {uint8_t(n >> 21 & 0x7F), uint8_t(n >> 14 & 0x7F), uint8_t(n >> 7 & 0x7F), uint8_t(n & 0x7F)};
}

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

bool valid_id(const uint8_t* p, size_t n) {
    return std::all_of(p, p + n, [](uint8_t c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); });
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

/// Frame body without the bytes its flags add (group id, data length
/// indicator); nullopt if compressed or encrypted.
std::optional<std::span<const uint8_t>> body_of(const Frame& f) {
    if (f.flags[1] & (V24_COMPRESSED | V24_ENCRYPTED)) return std::nullopt;
    size_t skip = (f.flags[1] & V24_GROUP ? 1 : 0) + (f.flags[1] & V24_DATA_LENGTH ? 4 : 0);
    if (f.data.size() < skip) return std::nullopt;
    return std::span(f.data).subspan(skip);
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

/// Language of a COMM/USLT frame.
std::string language(const Frame& f) {
    auto b = body_of(f);
    return b && b->size() >= 4 ? std::string(str_of(b->subspan(1, 3))) : "";
}

/// The values of a text frame body (encoding byte, then terminated strings).
std::vector<std::string> text_values(std::span<const uint8_t> b) {
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
}

Frame make_frame(std::string_view id, Bytes data) {
    Frame f;
    std::memcpy(f.id, id.data(), 4);
    f.data = std::move(data);
    return f;
}

Frame text_frame(const std::string& id, const std::vector<std::string>& values) {
    Bytes body = {3};  // UTF-8
    append(body, join(values, std::string_view("\0", 1)));
    return make_frame(id, std::move(body));
}

Frame txxx_frame(const std::string& desc, const std::vector<std::string>& values) {
    Bytes body = {3};
    append(body, desc);
    body.push_back(0);
    append(body, join(values, std::string_view("\0", 1)));
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

/// First value of the text frame `id`.
std::optional<std::string> frame_text(const std::vector<Frame>& frames, std::string_view id) {
    for (auto& f : frames)
        if (f.sid() == id)
            if (auto b = body_of(f)) {
                auto vs = text_values(*b);
                return vs.empty() ? "" : trim(vs[0]);
            }
    return std::nullopt;
}

/// Replace the ID3v2.3-only frames that have ID3v2.4 counterparts.
std::vector<Frame> upgrade_v23(std::vector<Frame> frames) {
    auto year = frame_text(frames, "TYER");
    auto ddmm = frame_text(frames, "TDAT");
    auto tory = frame_text(frames, "TORY");
    std::erase_if(frames, [](const Frame& f) {
        auto i = f.sid();
        return i == "TYER" || i == "TDAT" || i == "TIME" || i == "TRDA" || i == "TORY" || i == "TSIZ";
    });
    for (auto& f : frames)
        if (f.sid() == "IPLS") std::memcpy(f.id, "TIPL", 4);  // same layout: pairs of strings
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

/// ID3v2.2 frame id -> ID3v2.3 id (dates are upgraded to v2.4 with the v2.3 ones).
/// iTunes wrote v2.2 tags for years; RVA/EQU (other layouts in later versions)
/// and the obsolete link/encryption frames have no counterpart and are dropped.
const std::map<std::string_view, std::string_view> V22_IDS = {
    {"BUF", "RBUF"}, {"CNT", "PCNT"}, {"COM", "COMM"}, {"CRA", "AENC"}, {"ETC", "ETCO"}, {"GEO", "GEOB"}, {"IPL", "IPLS"},
    {"MCI", "MCDI"}, {"MLL", "MLLT"}, {"POP", "POPM"}, {"REV", "RVRB"}, {"SLT", "SYLT"}, {"STC", "SYTC"}, {"TAL", "TALB"},
    {"TBP", "TBPM"}, {"TCM", "TCOM"}, {"TCO", "TCON"}, {"TCP", "TCMP"}, {"TCR", "TCOP"}, {"TDA", "TDAT"}, {"TDY", "TDLY"},
    {"TEN", "TENC"}, {"TFT", "TFLT"}, {"TIM", "TIME"}, {"TKE", "TKEY"}, {"TLA", "TLAN"}, {"TLE", "TLEN"}, {"TMT", "TMED"},
    {"TOA", "TOPE"}, {"TOF", "TOFN"}, {"TOL", "TOLY"}, {"TOR", "TORY"}, {"TOT", "TOAL"}, {"TP1", "TPE1"}, {"TP2", "TPE2"},
    {"TP3", "TPE3"}, {"TP4", "TPE4"}, {"TPA", "TPOS"}, {"TPB", "TPUB"}, {"TRC", "TSRC"}, {"TRD", "TRDA"}, {"TRK", "TRCK"},
    {"TS2", "TSO2"}, {"TSA", "TSOA"}, {"TSC", "TSOC"}, {"TSI", "TSIZ"}, {"TSP", "TSOP"}, {"TSS", "TSSE"}, {"TST", "TSOT"},
    {"TT1", "TIT1"}, {"TT2", "TIT2"}, {"TT3", "TIT3"}, {"TXT", "TEXT"}, {"TXX", "TXXX"}, {"TYE", "TYER"}, {"UFI", "UFID"},
    {"ULT", "USLT"}, {"WAF", "WOAF"}, {"WAR", "WOAR"}, {"WAS", "WOAS"}, {"WCM", "WCOM"}, {"WCP", "WCOP"}, {"WPB", "WPUB"},
    {"WXX", "WXXX"},
};

/// An ID3v2.2 frame as ID3v2.3; nullopt if it has no later counterpart.
std::optional<Frame> from_v22(std::string_view id, std::span<const uint8_t> data) {
    if (id == "PIC") {
        // PIC has a three-letter image format where APIC has a MIME type
        if (data.size() < 4) return std::nullopt;
        std::string fmt = lower(str_of(data.subspan(1, 3)));
        Bytes b = {data[0]};
        append(b, fmt == "jpg" ? std::string("image/jpeg") : "image/" + fmt);
        b.push_back(0);
        append(b, data.subspan(4));
        return make_frame("APIC", std::move(b));
    }
    auto it = V22_IDS.find(id);
    if (it == V22_IDS.end()) return std::nullopt;
    return make_frame(it->second, Bytes(data.begin(), data.end()));
}

/// ID3v2.3 frame flags and the bytes they add, moved to their ID3v2.4 places:
/// v2.3 puts the decompressed size, encryption method and group id after the
/// header in that order; v2.4 has group id, encryption method, then the
/// decompressed size as a synchsafe data length indicator. nullopt if the frame
/// is too short for what its flags announce.
std::optional<Frame> from_v23(Frame f) {
    uint8_t st = f.flags[0], fmt = f.flags[1];
    size_t p = 0;
    std::optional<uint32_t> size;
    std::optional<uint8_t> method, group;
    if (fmt & V23_COMPRESSED) {
        if (f.data.size() < 4) return std::nullopt;
        size = be32(f.data.data());
        if (*size >= MAX_SIZE) return std::nullopt;  // not expressible in v2.4 (nor plausible)
        p = 4;
    }
    if (fmt & V23_ENCRYPTED) {
        if (f.data.size() <= p) return std::nullopt;
        method = f.data[p++];
    }
    if (fmt & V23_GROUP) {
        if (f.data.size() <= p) return std::nullopt;
        group = f.data[p++];
    }
    // status flags: tag alter, file alter and read-only preservation, one bit lower in v2.4
    f.flags[0] = uint8_t((st & 0xE0) >> 1);
    f.flags[1] = uint8_t((group ? V24_GROUP : 0) | (method ? V24_ENCRYPTED : 0) | (size ? V24_COMPRESSED | V24_DATA_LENGTH : 0));
    std::array<uint8_t, 6> added{};
    size_t n = 0;
    if (group) added[n++] = *group;
    if (method) added[n++] = *method;
    if (size)
        for (auto b : synchsafe(*size)) added[n++] = b;
    f.data.erase(f.data.begin(), f.data.begin() + ptrdiff_t(p));
    f.data.insert(f.data.begin(), added.begin(), added.begin() + ptrdiff_t(n));
    return f;
}

/// Whether the frames from `pos` on, read with synchsafe or with plain 32-bit
/// sizes, end exactly at the padding or the end of the tag.
bool frames_fit(std::span<const uint8_t> body, size_t pos, bool synchsafe_sizes) {
    while (pos + 10 <= body.size() && body[pos] != 0) {
        if (!valid_id(&body[pos], 4)) return false;
        uint64_t fs = synchsafe_sizes ? unsynchsafe(&body[pos + 4]) : be32(&body[pos + 4]);
        if (fs > body.size() - pos - 10) return false;
        pos += 10 + size_t(fs);
    }
    return true;
}

}  // namespace

std::optional<uint64_t> tag_length(std::span<const uint8_t> head) {
    if (head.size() < 10 || std::memcmp(head.data(), "ID3", 3) != 0) return std::nullopt;
    bool footer = head[3] == 4 && (head[5] & 0x10);
    return 10 + uint64_t(unsynchsafe(&head[6])) + (footer ? 10 : 0);
}

std::pair<SourceTag, size_t> parse_tag(std::span<const uint8_t> b) {
    WM_ENSURE(b.size() >= 10, "truncated ID3 tag");
    uint8_t major = b[3], hflags = b[5];
    size_t size = unsynchsafe(&b[6]);
    size_t total = 10 + size + (major == 4 && (hflags & 0x10) ? 10 : 0);
    WM_ENSURE(major >= 2 && major <= 4, "ID3v2.{} tag", major);
    WM_ENSURE(!(major == 2 && (hflags & 0x40)), "compressed ID3v2.2 tag");
    WM_ENSURE(b.size() >= 10 + size, "truncated ID3 tag");
    Bytes body(b.begin() + 10, b.begin() + 10 + ptrdiff_t(size));
    if (major < 4 && (hflags & 0x80)) body = deunsync(body);
    size_t pos = 0;
    if (major > 2 && (hflags & 0x40) && body.size() >= 4) pos = major == 3 ? 4 + size_t(be32(&body[0])) : unsynchsafe(&body[0]);
    // v2.4 frame sizes are synchsafe, but iTunes and others wrote plain ones:
    // take those when only they lead from frame to frame (as mutagen does)
    bool plain_sizes = major == 3 || (major == 4 && !frames_fit(body, pos, true) && frames_fit(body, pos, false));
    size_t id_len = major == 2 ? 3 : 4, head_len = major == 2 ? 6 : 10;
    SourceTag tag;
    while (pos + head_len <= body.size() && body[pos] != 0) {
        // anything that is not a frame (non-zero padding, junk) ends the frames
        if (!valid_id(&body[pos], id_len)) break;
        uint64_t fs = major == 2 ? uint64_t(body[pos + 3]) << 16 | body[pos + 4] << 8 | body[pos + 5]
                      : plain_sizes ? be32(&body[pos + 4]) : unsynchsafe(&body[pos + 4]);
        if (fs > body.size() - pos - head_len) {
            debug("ID3: frame {} overruns the tag; ignoring the rest", str_of(std::span(body).subspan(pos, id_len)));
            break;
        }
        size_t at = pos;
        pos += head_len + size_t(fs);
        std::string_view id = str_of(std::span(body).subspan(at, id_len));
        auto data = std::span(body).subspan(at + head_len, size_t(fs));
        if (major == 2) {
            if (auto f = from_v22(id, data)) tag.frames.push_back(std::move(*f));
            continue;
        }
        Frame f = make_frame(id, Bytes(data.begin(), data.end()));
        f.flags[0] = body[at + 8];
        f.flags[1] = body[at + 9];
        if (major == 3) {
            auto g = from_v23(std::move(f));
            if (!g) continue;
            f = std::move(*g);
        } else if ((hflags & 0x80) || (f.flags[1] & V24_UNSYNC)) {
            // ID3v2.4 unsynchronises per frame; the header flag means all of them
            // (writers set it without flagging the frames). Frames are kept plain,
            // so the tag is written without unsynchronisation and new frames and
            // pictures can sit next to them. The bytes the flags add come first.
            size_t added = (f.flags[1] & V24_GROUP ? 1 : 0) + (f.flags[1] & V24_ENCRYPTED ? 1 : 0) + (f.flags[1] & V24_DATA_LENGTH ? 4 : 0);
            if (f.data.size() >= added) {
                Bytes plain(f.data.begin(), f.data.begin() + ptrdiff_t(added));
                append(plain, deunsync(std::span(f.data).subspan(added)));
                f.data = std::move(plain);
            }
            f.flags[1] &= uint8_t(~V24_UNSYNC);
        }
        tag.frames.push_back(std::move(f));
    }
    if (major < 4) tag.frames = upgrade_v23(std::move(tag.frames));
    return {std::move(tag), total};
}

void apply(std::vector<Frame>& frames, const Tags& overlay) {
    // track / disc: number and total share one "n/total" frame
    for (auto [id, num, total] : {std::tuple{"TRCK", "TRACKNUMBER", "TRACKTOTAL"}, std::tuple{"TPOS", "DISCNUMBER", "DISCTOTAL"}}) {
        bool num_named = overlay.get_all(num) != nullptr;
        if (!num_named && !overlay.get_all(total)) continue;
        std::optional<std::string> n;
        if (auto v = overlay.get(num)) n = *v;
        else if (!num_named)
            if (auto old = frame_text(frames, id)) n = trim(split(*old, '/')[0]);
        std::erase_if(frames, [&](auto& f) { return f.sid() == id; });
        if (n && !n->empty()) frames.push_back(text_frame(id, {join_number(*n, overlay.get(total))}));
    }
    // One frame per place. When several fields of the overlay share a place the
    // owner wins, so that a value read back from the frame and saved under the
    // owner's name (TDRC -> DATE) replaces the other field's (YEAR).
    std::vector<std::pair<Target, const std::vector<std::string>*>> picks;
    for (auto& [k, vs] : overlay.m) {
        if (is_number_field(k)) continue;
        Target t = target_for(k);
        auto it = std::find_if(picks.begin(), picks.end(), [&](auto& p) { return p.first.same_place(t); });
        if (it == picks.end()) picks.emplace_back(std::move(t), &vs);
        else if (t.owner && !it->first.owner) *it = {std::move(t), &vs};
    }
    // remove everything first: a plain LYRICS replaces every USLT frame, but not
    // the LYRICS-eng frame added next to it
    for (auto& [t, vs] : picks)
        std::erase_if(frames, [&](const Frame& f) {
            switch (t.kind) {
            case Target::Text: return f.sid() == t.s || (t.s == "TDRC" && (f.sid() == "TYER" || f.sid() == "TDAT" || f.sid() == "TIME" || f.sid() == "TRDA"));
            case Target::Txxx: {
                if (f.sid() != "TXXX") return false;
                auto d = description(f);
                return d && same_field(*d, t.s);
            }
            case Target::Comment: {
                if (f.sid() != "COMM") return false;
                auto d = description(f);
                return d && d->empty();
            }
            case Target::Lyrics: return f.sid() == "USLT" && (t.s.empty() || iequals(language(f), t.s));
            }
            return false;
        });
    for (auto& [t, vs] : picks) {
        if (vs->empty()) continue;
        switch (t.kind) {
        case Target::Text: frames.push_back(text_frame(t.s, *vs)); break;
        case Target::Txxx: frames.push_back(txxx_frame(t.s, *vs)); break;
        case Target::Comment: frames.push_back(lang_frame("COMM", "eng", join(*vs, "\n"))); break;
        case Target::Lyrics: frames.push_back(lang_frame("USLT", t.s.empty() ? "xxx" : t.s, join(*vs, "\n"))); break;
        }
    }
}

Tags read_tags(std::span<const uint8_t> tag) {
    Tags t;
    if (tag.size() < 10 || std::memcmp(tag.data(), "ID3", 3) != 0) return t;
    std::vector<std::pair<std::string, std::string>> numbers;  // TRCK/TPOS, added last (see add_number)
    for (auto& f : parse_tag(tag).first.frames) {
        auto body = body_of(f);
        if (!body || body->empty()) continue;
        std::string id(f.sid());
        uint8_t enc = (*body)[0];
        if (id == "TXXX") {
            auto [d, rest] = split_terminated(enc, body->subspan(1));
            std::string name = decode_text(enc, d);
            if (name.empty()) continue;  // a value without a name is no tag
            Bytes values = {enc};
            append(values, rest);
            for (auto& v : text_values(values)) t.add(name, v);
        } else if (id == "COMM" || id == "USLT") {
            if (body->size() < 4) continue;
            auto [d, text] = split_terminated(enc, body->subspan(4));
            if (id == "COMM" && !decode_text(enc, d).empty()) continue;  // described comments are not "the" comment
            std::string v = decode_text(enc, text);
            while (!v.empty() && v.back() == '\0') v.pop_back();  // optional terminator
            t.add(std::string(id3_owner(id)->name), v);
        } else if (id == "TRCK" || id == "TPOS") {
            if (auto vs = text_values(*body); !vs.empty()) numbers.emplace_back(id, vs[0]);
        } else if (auto o = id3_owner(id)) {
            for (auto& v : text_values(*body)) t.add(std::string(o->name), v);
        } else if (id[0] == 'T') {
            for (auto& v : text_values(*body)) t.add(id, v);
        }
    }
    for (auto& [id, v] : numbers) add_number(t, id == "TRCK" ? "TRACKNUMBER" : "DISCNUMBER", id == "TRCK" ? "TRACKTOTAL" : "DISCTOTAL", v);
    return t;
}

namespace {

void put_frame_header(Bytes& out, std::string_view id, const uint8_t flags[2], uint64_t size) {
    append(out, id);
    append(out, synchsafe(size));
    out.push_back(flags[0]);
    out.push_back(flags[1]);
}

/// Tag header and frames, then (when `apic` has bytes) the header of an APIC
/// frame of `apic` bytes; the tag size counts them and the padding.
Bytes tag_head(const std::vector<Frame>& frames, uint64_t apic) {
    Bytes body;
    for (auto& f : frames) {
        put_frame_header(body, f.sid(), f.flags, f.data.size());
        append(body, f.data);
    }
    static const uint8_t no_flags[2] = {0, 0};
    if (apic) put_frame_header(body, "APIC", no_flags, apic);
    Bytes out = {'I', 'D', '3', 4, 0, 0};
    append(out, synchsafe(body.size() + apic + PADDING));
    append(out, body);
    return out;
}

/// serialize(), with the pictures replaced by `cover` (referenced, not copied).
std::vector<Seg> serialize_segments(std::vector<Frame> frames, const Cover* cover) {
    std::vector<Seg> apic;
    uint64_t apic_len = 0;
    if (cover) {
        std::erase_if(frames, [](const Frame& f) { return f.sid() == "APIC"; });
        apic = id3_apic(*cover);
        for (auto& s : apic) apic_len += seg_size(s);
    }
    std::vector<Seg> segs = {tag_head(frames, apic_len)};
    segs.insert(segs.end(), apic.begin(), apic.end());
    segs.push_back(Zeros{PADDING});
    return segs;
}

}  // namespace

Bytes serialize(const std::vector<Frame>& frames) {
    Bytes out = tag_head(frames, 0);
    out.resize(out.size() + PADDING);
    return out;
}

std::vector<Seg> build_segments(const Tags& tags, const Cover* cover) {
    std::vector<Frame> frames;
    apply(frames, tags);
    return serialize_segments(std::move(frames), cover);
}

Bytes build(const Tags& tags) {
    std::vector<Frame> frames;
    apply(frames, tags);
    return serialize(frames);
}

std::pair<std::vector<Seg>, uint64_t> retag_at(const File& f, uint64_t pos, const Tags& overlay, const Cover* cover) {
    uint8_t head[10] = {};
    SourceTag src;
    uint64_t len = 0;
    if (f.read_at(head, 10, pos) == 10)
        if (auto total = tag_length(head)) {
            auto [tag, l] = parse_tag(f.read_vec(pos, size_t(*total)));
            src = std::move(tag);
            len = l;
        }
    apply(src.frames, overlay);
    return {serialize_segments(std::move(src.frames), cover), len};
}

}  // namespace wm::id3
