// SPDX-License-Identifier: GPL-3.0-or-later
#include "mp4.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace wm::mp4 {

namespace {

using Type = std::array<uint8_t, 4>;

Type T(const char* s) { return {uint8_t(s[0]), uint8_t(s[1]), uint8_t(s[2]), uint8_t(s[3])}; }

struct Node {
    Type typ{};
    bool container = false;
    Bytes data;      // leaf payload
    Bytes prefix;    // full-box version/flags (meta)
    std::vector<Node> children;

    uint64_t len() const {
        uint64_t payload = container ? prefix.size() : data.size();
        if (container)
            for (auto& c : children) payload += c.len();
        return payload + 8 > UINT32_MAX ? payload + 16 : payload + 8;
    }

    void write(Bytes& out) const {
        uint64_t l = len();
        if (l > UINT32_MAX) {
            put_be32(out, 1);
            append(out, typ);
            put_be64(out, l);
        } else {
            put_be32(out, uint32_t(l));
            append(out, typ);
        }
        if (!container) {
            append(out, data);
            return;
        }
        append(out, prefix);
        for (auto& c : children) c.write(out);
    }

    Node* child(const Type& t) {
        for (auto& c : children)
            if (c.typ == t) return &c;
        return nullptr;
    }
};

Node leaf(const Type& t, Bytes d) {
    Node n;
    n.typ = t;
    n.data = std::move(d);
    return n;
}

Node box(const Type& t, std::vector<Node> kids, Bytes prefix = {}) {
    Node n;
    n.typ = t;
    n.container = true;
    n.prefix = std::move(prefix);
    n.children = std::move(kids);
    return n;
}

bool is_container(const Type& t) {
    for (auto c : {"moov", "trak", "mdia", "minf", "stbl", "udta", "meta"})
        if (t == T(c)) return true;
    return false;
}

/// Parse the boxes in `b` (no box crosses its end).
std::vector<Node> parse(std::span<const uint8_t> b) {
    std::vector<Node> out;
    size_t pos = 0;
    while (pos + 8 <= b.size()) {
        uint64_t size = be32(&b[pos]);
        Type typ;
        std::memcpy(typ.data(), &b[pos + 4], 4);
        size_t hdr = 8;
        if (size == 1) {
            WM_ENSURE(pos + 16 <= b.size(), "short box");
            size = be64(&b[pos + 8]);
            hdr = 16;
        } else if (size == 0) {
            size = b.size() - pos;
        }
        WM_ENSURE(size >= hdr && pos + size <= b.size(), "box overruns parent");
        auto payload = b.subspan(pos + hdr, size_t(size) - hdr);
        if (is_container(typ)) {
            // meta is a full box in ISO files but a plain container in some QuickTime files
            size_t pl = typ == T("meta") && payload.size() >= 8 && std::memcmp(&payload[4], "hdlr", 4) != 0 ? 4 : 0;
            out.push_back(box(typ, parse(payload.subspan(pl)), Bytes(payload.begin(), payload.begin() + ptrdiff_t(pl))));
        } else {
            out.push_back(leaf(typ, Bytes(payload.begin(), payload.end())));
        }
        pos += size_t(size);
    }
    return out;
}

enum class Kind { Text, Pair, U8, U16 };

/// What an ilst item is keyed by.
struct Item {
    enum { Atom, Freeform, Ignore } what;
    Type atom{};
    Kind kind = Kind::Text;
    std::string name;
    bool operator==(const Item& o) const {
        if (what != o.what) return false;
        if (what == Atom) return atom == o.atom;
        if (what == Freeform) return iequals(name, o.name);
        return true;
    }
};

/// "©nam" -> atom name (© is 0xA9)
Type atom(std::string_view s) {
    Type out{};
    size_t i = 0;
    for (size_t k = 0; k < s.size() && i < 4; k++) {
        if (uint8_t(s[k]) == 0xC2 && k + 1 < s.size() && uint8_t(s[k + 1]) == 0xA9) {
            out[i++] = 0xA9;
            k++;
        } else {
            out[i++] = uint8_t(s[k]);
        }
    }
    return out;
}

const std::vector<std::string_view> TRACK_NAMES = {"TRACKNUMBER", "TRACK", "TRKN"};
const std::vector<std::string_view> TRACK_TOTALS = {"TRACKTOTAL", "TOTALTRACKS"};
const std::vector<std::string_view> DISC_NAMES = {"DISCNUMBER", "DISC", "DISK"};
const std::vector<std::string_view> DISC_TOTALS = {"DISCTOTAL", "TOTALDISCS"};

bool in(const std::vector<std::string_view>& v, std::string_view k) { return std::find(v.begin(), v.end(), k) != v.end(); }

size_t utf8_chars(std::string_view s) {
    return size_t(std::count_if(s.begin(), s.end(), [](char c) { return (uint8_t(c) & 0xC0) != 0x80; }));
}

/// Tag name -> ilst item. Accepts Vorbis names (TITLE, ALBUMARTIST), the names
/// ffprobe reports for MP4 sources (album_artist, sort_album, ...) and raw atom
/// names (©nam, aART). Case-insensitive except for raw atom names.
Item item_for(const std::string& key) {
    std::string k = upper(key);
    auto a = [](const char* s) { return Item{Item::Atom, atom(s), Kind::Text, ""}; };
    if (in(TRACK_NAMES, k) || in(TRACK_TOTALS, k)) return {Item::Atom, T("trkn"), Kind::Pair, ""};
    if (in(DISC_NAMES, k) || in(DISC_TOTALS, k)) return {Item::Atom, T("disk"), Kind::Pair, ""};
    static const std::vector<std::pair<std::vector<std::string_view>, const char*>> text = {
        {{"TITLE"}, "©nam"},
        {{"ARTIST"}, "©ART"},
        {{"ALBUMARTIST", "ALBUM_ARTIST", "ALBUM ARTIST"}, "aART"},
        {{"ALBUM"}, "©alb"},
        {{"DATE", "YEAR"}, "©day"},
        {{"GENRE"}, "©gen"},
        {{"COMPOSER"}, "©wrt"},
        {{"COMMENT"}, "©cmt"},
        {{"DESCRIPTION"}, "desc"},
        {{"SYNOPSIS"}, "ldes"},
        {{"ENCODER"}, "©too"},
        {{"COPYRIGHT"}, "cprt"},
        {{"GROUPING"}, "©grp"},
        {{"LYRICS", "UNSYNCEDLYRICS"}, "©lyr"},
        {{"WORK"}, "©wrk"},
        {{"MOVEMENTNAME"}, "©mvn"},
        {{"TITLESORT", "TITLE_SORT", "SORT_NAME"}, "sonm"},
        {{"ALBUMSORT", "ALBUM_SORT", "SORT_ALBUM"}, "soal"},
        {{"ARTISTSORT", "ARTIST_SORT", "SORT_ARTIST"}, "soar"},
        {{"ALBUMARTISTSORT", "ALBUM_ARTIST_SORT", "SORT_ALBUM_ARTIST"}, "soaa"},
        {{"COMPOSERSORT", "COMPOSER_SORT", "SORT_COMPOSER"}, "socm"},
        {{"SHOW"}, "tvsh"},
        {{"NETWORK"}, "tvnn"},
        {{"EPISODE_ID"}, "tven"},
        {{"PURCHASE_DATE"}, "purd"},
    };
    if (k == "MAJOR_BRAND" || k == "MINOR_VERSION" || k == "COMPATIBLE_BRANDS" || k == "CREATION_TIME") return {Item::Ignore};
    for (auto& [names, at] : text)
        if (in(names, k)) return a(at);
    if (k == "COMPILATION") return {Item::Atom, T("cpil"), Kind::U8, ""};
    if (k == "GAPLESS_PLAYBACK") return {Item::Atom, T("pgap"), Kind::U8, ""};
    if (k == "MEDIA_TYPE") return {Item::Atom, T("stik"), Kind::U8, ""};
    if (k == "RATING") return {Item::Atom, T("rtng"), Kind::U8, ""};
    if (k == "BPM" || k == "TMPO") return {Item::Atom, T("tmpo"), Kind::U16, ""};
    if (utf8_chars(key) == 4 && starts_with(key, "©")) return a(key.c_str());
    for (auto raw : {"aART", "cprt", "desc", "ldes", "sonm", "soal", "soar", "soaa", "socm", "tvsh", "tvnn", "tven", "purd"})
        if (key == raw) return a(raw);
    return {Item::Freeform, {}, Kind::Text, key};
}

Node data_atom(uint32_t typ, std::span<const uint8_t> payload) {
    Bytes d;
    put_be32(d, typ);
    put_be32(d, 0);  // locale
    append(d, payload);
    return leaf(T("data"), std::move(d));
}

std::optional<std::string> freeform_name(const Node& n) {
    if (!n.container) return std::nullopt;
    for (auto& c : n.children)
        if (!c.container && c.typ == T("name") && c.data.size() >= 4) return lossy_utf8(std::span(c.data).subspan(4));
    return std::nullopt;
}

/// Number pair (trkn/disk) stored in an existing item.
std::optional<std::pair<uint16_t, uint16_t>> old_pair(const std::vector<Node>& items, const Type& t) {
    for (auto& n : items) {
        if (n.typ != t || !n.container) continue;
        for (auto& c : n.children)
            if (!c.container && c.typ == T("data") && c.data.size() >= 14) return std::pair{be16(&c.data[10]), be16(&c.data[12])};
    }
    return std::nullopt;
}

std::optional<uint16_t> parse_u16(std::string_view s) {
    auto v = parse_u64(trim(s));
    if (!v || *v > 65535) return std::nullopt;
    return uint16_t(*v);
}

std::pair<std::optional<uint16_t>, std::optional<uint16_t>> parse_pair(const std::string& s) {
    auto p = split(s, '/');
    return {parse_u16(p[0]), p.size() > 1 ? parse_u16(p[1]) : std::nullopt};
}

/// Apply `overlay` to an ilst's items.
void apply(std::vector<Node>& items, const Tags& overlay) {
    auto get = [&](const std::vector<std::string_view>& names) -> const std::string* {
        for (auto n : names)
            if (auto v = overlay.get(n)) return v;
        return nullptr;
    };
    auto named = [&](const std::vector<std::string_view>& names) {
        return std::any_of(names.begin(), names.end(), [&](auto n) { return overlay.get_all(n) != nullptr; });
    };
    for (auto [typ, nums, totals] : {std::tuple{T("trkn"), &TRACK_NAMES, &TRACK_TOTALS}, std::tuple{T("disk"), &DISC_NAMES, &DISC_TOTALS}}) {
        if (!named(*nums) && !named(*totals)) continue;
        auto old = old_pair(items, typ);
        std::optional<uint16_t> n, t_in_n;
        if (auto v = get(*nums)) std::tie(n, t_in_n) = parse_pair(*v);
        if (!named(*nums) && old) n = old->first;
        std::optional<uint16_t> t;
        if (auto v = get(*totals)) t = parse_u16(*v);
        if (!t) t = t_in_n;
        if (!t && !named(*totals) && old) t = old->second;
        std::erase_if(items, [&](auto& i) { return i.typ == typ; });
        if (n) {
            Bytes p = {0, 0};
            put_be16(p, *n);
            put_be16(p, t.value_or(0));
            if (typ == T("trkn")) p.insert(p.end(), {0, 0});
            items.push_back(box(typ, {data_atom(0, p)}));
        }
    }
    std::vector<Item> used;
    for (auto& [k, vs] : overlay.m) {
        Item it = item_for(k);
        if (it.what == Item::Ignore || (it.what == Item::Atom && it.kind == Kind::Pair)) continue;
        if (std::find(used.begin(), used.end(), it) != used.end()) continue;
        if (it.what == Item::Atom)
            std::erase_if(items, [&](auto& i) { return i.typ == it.atom || (it.atom == atom("©gen") && i.typ == T("gnre")); });
        else
            std::erase_if(items, [&](auto& i) {
                auto n = i.typ == T("----") ? freeform_name(i) : std::nullopt;
                return n && iequals(*n, it.name);
            });
        if (!vs.empty()) {
            std::vector<Node> kids;
            if (it.what == Item::Freeform) {
                Bytes mean = {0, 0, 0, 0}, name = {0, 0, 0, 0};
                append(mean, std::string_view("com.apple.iTunes"));
                append(name, it.name);
                kids.push_back(leaf(T("mean"), mean));
                kids.push_back(leaf(T("name"), name));
                for (auto& v : vs) kids.push_back(data_atom(1, bytes_of(v)));
            } else if (it.kind == Kind::Text) {
                for (auto& v : vs) kids.push_back(data_atom(1, bytes_of(v)));
            } else if (it.kind == Kind::U8) {
                std::string v = trim(vs[0]);
                std::optional<uint8_t> n;
                if (auto x = parse_u64(v); x && *x <= 255) n = uint8_t(*x);
                else if (v == "true") n = 1;
                else if (v == "false") n = 0;
                if (n) kids.push_back(data_atom(21, std::array<uint8_t, 1>{*n}));
            } else if (it.kind == Kind::U16) {
                try {
                    size_t used_chars = 0;
                    double d = std::stod(trim(vs[0]), &used_chars);
                    Bytes b;
                    put_be16(b, uint16_t(std::lround(d)));
                    kids.push_back(data_atom(21, b));
                } catch (...) {
                }
            }
            if (!kids.empty()) items.push_back(box(it.what == Item::Atom ? it.atom : T("----"), std::move(kids)));
        }
        used.push_back(it);
    }
}

/// ilst items: each item is a container of data/mean/name atoms.
std::vector<Node> parse_ilst(std::span<const uint8_t> data) {
    std::vector<Node> out;
    for (auto& n : parse(data)) {
        if (n.container) {
            out.push_back(std::move(n));
            continue;
        }
        try {
            out.push_back(box(n.typ, parse(n.data)));
        } catch (...) {
            out.push_back(std::move(n));
        }
    }
    return out;
}

/// Add `delta` to every chunk offset at or beyond `from`.
void shift_offsets(Node& n, uint64_t from, int64_t delta) {
    if (n.container) {
        for (auto& c : n.children) shift_offsets(c, from, delta);
        return;
    }
    if (n.typ != T("stco") && n.typ != T("co64")) return;
    bool wide = n.typ == T("co64");
    auto& d = n.data;
    WM_ENSURE(d.size() >= 8, "short stco");
    size_t count = be32(&d[4]), w = wide ? 8 : 4;
    WM_ENSURE(d.size() >= 8 + count * w, "short chunk offset table");
    for (size_t i = 0; i < count; i++) {
        size_t p = 8 + i * w;
        uint64_t v = wide ? be64(&d[p]) : be32(&d[p]);
        if (v < from) continue;
        uint64_t nv = uint64_t(int64_t(v) + delta);
        Bytes b;
        if (wide) put_be64(b, nv);
        else {
            WM_ENSURE(nv <= UINT32_MAX, "chunk offset overflow");
            put_be32(b, uint32_t(nv));
        }
        std::copy(b.begin(), b.end(), d.begin() + ptrdiff_t(p));
    }
}

}  // namespace

Retagged retag_m4a(const fs::path& p, const Tags& overlay) {
    File f(p);
    uint64_t flen = f.size(), pos = 0;
    std::optional<std::pair<uint64_t, uint64_t>> moov;
    while (pos + 8 <= flen) {
        uint8_t h[16];
        f.read_exact_at(h, 8, pos);
        uint64_t size = be32(h);
        if (size == 1) {
            f.read_exact_at(h + 8, 8, pos + 8);
            size = be64(h + 8);
        } else if (size == 0) {
            size = flen - pos;
        }
        WM_ENSURE(size >= 8, "bad box at {}", pos);
        WM_ENSURE(std::memcmp(h + 4, "moof", 4) != 0, "fragmented MP4");
        if (std::memcmp(h + 4, "moov", 4) == 0) moov = {pos, size};
        pos += size;
    }
    WM_ENSURE(moov.has_value(), "no moov box");
    auto [mpos, mlen] = *moov;
    Bytes raw(static_cast<size_t>(mlen));
    f.read_exact_at(raw.data(), raw.size(), mpos);
    auto top = parse(raw);
    WM_ENSURE(!top.empty(), "moov parse");
    Node root = std::move(top.back());

    // moov/udta/meta(hdlr mdir)/ilst, created when missing
    if (!root.child(T("udta"))) root.children.push_back(box(T("udta"), {}));
    Node* udta = root.child(T("udta"));
    if (!udta->child(T("meta"))) {
        Bytes hdlr(8, 0);
        append(hdlr, std::string_view("mdirappl"));
        hdlr.insert(hdlr.end(), 9, 0);
        udta->children.push_back(box(T("meta"), {leaf(T("hdlr"), hdlr)}, Bytes{0, 0, 0, 0}));
    }
    Node* meta = udta->child(T("meta"));
    Node* ilst = meta->child(T("ilst"));
    Bytes ilst_data = ilst && !ilst->container ? std::move(ilst->data) : Bytes{};
    auto items = parse_ilst(ilst_data);
    apply(items, overlay);
    Bytes body;
    for (auto& i : items) i.write(body);
    if (ilst && !ilst->container) ilst->data = std::move(body);
    else meta->children.push_back(leaf(T("ilst"), std::move(body)));

    int64_t delta = int64_t(root.len()) - int64_t(mlen);
    if (delta != 0) shift_offsets(root, mpos + mlen, delta);
    Retagged r{mpos, mlen, {}};
    r.moov.reserve(size_t(root.len()));
    root.write(r.moov);
    return r;
}

}  // namespace wm::mp4
