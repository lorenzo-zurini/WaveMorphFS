// SPDX-License-Identifier: GPL-3.0-or-later
#include "mp4.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>

namespace wm::mp4 {

namespace {

using Type = std::array<uint8_t, 4>;

/// Largest moov box handled: sample tables of hours of audio plus cover art fit
/// easily; a larger size is a corrupt file, not a reason to allocate it.
constexpr uint64_t MAX_MOOV = 64 << 20;
/// Deepest box nesting followed (real files nest about 8 deep).
constexpr int MAX_DEPTH = 32;

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
std::vector<Node> parse(std::span<const uint8_t> b, int depth = 0) {
    WM_ENSURE(depth < MAX_DEPTH, "boxes nested too deeply");
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
        // (compared with what is left: pos + size could wrap around for 64-bit sizes)
        WM_ENSURE(size >= hdr && size <= b.size() - pos, "box overruns parent");
        auto payload = b.subspan(pos + hdr, size_t(size) - hdr);
        if (is_container(typ)) {
            // meta is a full box in ISO files but a plain container in some QuickTime files
            size_t pl = typ == T("meta") && payload.size() >= 8 && std::memcmp(&payload[4], "hdlr", 4) != 0 ? 4 : 0;
            out.push_back(box(typ, parse(payload.subspan(pl), depth + 1), Bytes(payload.begin(), payload.begin() + ptrdiff_t(pl))));
        } else {
            out.push_back(leaf(typ, Bytes(payload.begin(), payload.end())));
        }
        pos += size_t(size);
    }
    return out;
}

/// Position and size of the top-level moov box. A last box running past the end
/// (a truncated file) ends the scan; fragmented files are refused, as their moof
/// boxes would need their offsets shifted too.
std::pair<uint64_t, uint64_t> find_moov(const ByteReader& read, uint64_t size) {
    std::optional<std::pair<uint64_t, uint64_t>> moov;
    for (uint64_t pos = 0; pos + 8 <= size;) {
        Bytes h = read(pos, 16);
        WM_ENSURE(h.size() >= 8, "short read at {}", pos);
        uint64_t sz = be32(h.data()), hdr = 8;
        if (sz == 1) {
            WM_ENSURE(h.size() >= 16, "short read at {}", pos);
            sz = be64(&h[8]);
            hdr = 16;
        } else if (sz == 0) {
            sz = size - pos;
        }
        WM_ENSURE(sz >= hdr, "bad box size {} at {}", sz, pos);
        // (compared with what is left: a bogus 64-bit size could wrap pos around)
        if (sz > size - pos) break;
        WM_ENSURE(std::memcmp(&h[4], "moof", 4) != 0, "fragmented MP4");
        if (std::memcmp(&h[4], "moov", 4) == 0) moov = {pos, sz};
        pos += sz;
    }
    WM_ENSURE(moov.has_value(), "no moov box");
    WM_ENSURE(moov->second <= MAX_MOOV, "moov box of {} bytes", moov->second);
    return *moov;
}

enum class Kind { Text, U8, U16 };

/// How an ilst atom stores its value.
Kind kind_of(const Type& atom) {
    for (auto a : {"cpil", "pgap", "stik", "rtng"})
        if (atom == T(a)) return Kind::U8;
    return atom == T("tmpo") ? Kind::U16 : Kind::Text;
}

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

/// What an ilst item is keyed by.
struct Item {
    enum { Atom, Freeform, Ignore } what;
    Type atom{};
    std::string name;   // freeform name
    bool owner = true;  // the field owns the atom (see FieldSpec)

    bool same_place(const Item& o) const {
        if (what != o.what) return false;
        if (what == Atom) return atom == o.atom;
        if (what == Freeform) return same_field(name, o.name);
        return true;
    }
};

/// Tag name -> ilst item. Accepts every spelling field_spec() knows (Vorbis
/// TITLE, ffprobe album_artist / sort_album, atom names such as aART) and raw
/// ©-atom names. Track and disc numbers are not handled here.
Item item_for(const std::string& key) {
    std::string k = upper(key);
    // container metadata ffprobe reports next to the tags
    if (k == "MAJOR_BRAND" || k == "MINOR_VERSION" || k == "COMPATIBLE_BRANDS" || k == "CREATION_TIME") return {Item::Ignore};
    if (auto f = field_spec(key); f && !f->mp4.empty()) return {Item::Atom, atom(f->mp4), "", mp4_owner(f->mp4) == f};
    // a raw atom such as ©st3: © and three ASCII characters
    if (key.size() == 5 && starts_with(key, "©")) return {Item::Atom, atom(key)};
    return {Item::Freeform, {}, key};
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

/// The item for `vs`; nullopt if the value does not fit the atom (a number atom
/// given text).
std::optional<Node> make_item(const Item& it, const std::vector<std::string>& vs) {
    std::vector<Node> kids;
    if (it.what == Item::Freeform) {
        Bytes mean = {0, 0, 0, 0}, name = {0, 0, 0, 0};
        append(mean, std::string_view("com.apple.iTunes"));
        append(name, it.name);
        kids.push_back(leaf(T("mean"), mean));
        kids.push_back(leaf(T("name"), name));
        for (auto& v : vs) kids.push_back(data_atom(1, bytes_of(v)));
        return box(T("----"), std::move(kids));
    }
    std::string v = trim(vs[0]);
    switch (kind_of(it.atom)) {
    case Kind::Text:
        for (auto& x : vs) kids.push_back(data_atom(1, bytes_of(x)));
        break;
    case Kind::U8: {
        std::optional<uint8_t> n;
        if (auto x = parse_u64(v); x && *x <= 255) n = uint8_t(*x);
        else if (iequals(v, "true")) n = 1;
        else if (iequals(v, "false")) n = 0;
        if (!n) return std::nullopt;
        kids.push_back(data_atom(21, std::array<uint8_t, 1>{*n}));
        break;
    }
    case Kind::U16: {
        // BPM may come with decimals ("120.5"); the atom holds a whole number
        double d = 0;
        auto [end, ec] = std::from_chars(v.data(), v.data() + v.size(), d);
        if (v.empty() || ec != std::errc() || end != v.data() + v.size() || !(d >= 0 && d <= 65535)) return std::nullopt;
        Bytes b;
        put_be16(b, uint16_t(std::lround(d)));
        kids.push_back(data_atom(21, b));
        break;
    }
    }
    return box(it.atom, std::move(kids));
}

/// Apply `overlay` to an ilst's items.
void apply(std::vector<Node>& items, const Tags& overlay) {
    for (auto [typ, num, total] : {std::tuple{T("trkn"), "TRACKNUMBER", "TRACKTOTAL"}, std::tuple{T("disk"), "DISCNUMBER", "DISCTOTAL"}}) {
        bool num_named = overlay.get_all(num) != nullptr, total_named = overlay.get_all(total) != nullptr;
        if (!num_named && !total_named) continue;
        const std::string *nv = overlay.get(num), *tv = overlay.get(total);
        std::optional<uint16_t> n, t, t_in_n;
        if (nv) {
            auto p = split(*nv, '/');
            n = parse_u16(p[0]);
            if (p.size() > 1) t_in_n = parse_u16(p[1]);
        }
        if (tv) t = parse_u16(*tv);
        if ((nv && !n) || (tv && !t)) {
            // the atom holds numbers only ("A1" for a vinyl side does not fit)
            warn("MP4: \"{}\" is not a number; {} kept as it is", nv && !n ? *nv : *tv, str_of(typ));
            continue;
        }
        auto old = old_pair(items, typ);
        if (!num_named && old) n = old->first;
        if (!t) t = t_in_n;
        if (!t && !total_named && old) t = old->second;
        std::erase_if(items, [&](auto& i) { return i.typ == typ; });
        if (n) {
            Bytes p = {0, 0};
            put_be16(p, *n);
            put_be16(p, t.value_or(0));
            if (typ == T("trkn")) p.insert(p.end(), {0, 0});
            items.push_back(box(typ, {data_atom(0, p)}));
        }
    }
    // One item per place; when several fields of the overlay share an atom the
    // owner wins (see id3::apply).
    struct Pick {
        Item item;
        std::optional<Node> node;  // none: remove the item
    };
    std::vector<Pick> picks;
    for (auto& [k, vs] : overlay.m) {
        if (is_number_field(k)) continue;
        Item it = item_for(k);
        if (it.what == Item::Ignore) continue;
        auto p = std::find_if(picks.begin(), picks.end(), [&](auto& x) { return x.item.same_place(it); });
        if (p != picks.end() && (p->item.owner || !it.owner)) continue;
        std::optional<Node> node;
        if (!vs.empty() && !(node = make_item(it, vs))) {
            warn("MP4: \"{}\" does not fit the {} atom; kept as it is", vs[0], k);
            continue;
        }
        if (p != picks.end()) *p = {std::move(it), std::move(node)};
        else picks.push_back({std::move(it), std::move(node)});
    }
    for (auto& p : picks) {
        auto& it = p.item;
        std::erase_if(items, [&](auto& i) {
            if (it.what == Item::Atom) return i.typ == it.atom || (it.atom == atom("©gen") && i.typ == T("gnre"));
            auto n = i.typ == T("----") ? freeform_name(i) : std::nullopt;
            return n && same_field(*n, it.name);
        });
    }
    for (auto& p : picks)
        if (p.node) items.push_back(std::move(*p.node));
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
        uint64_t nv = v + uint64_t(delta);  // (two's complement: also moves down)
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

Tags read_tags(const ByteReader& read, uint64_t size) {
    Tags t;
    std::vector<Node> top;
    try {
        auto [pos, len] = find_moov(read, size);
        top = parse(read(pos, size_t(len)));
    } catch (const std::exception& e) {
        debug("MP4 tags: {}", e.what());
        return t;
    }
    if (top.empty()) return t;
    Node* udta = top.back().child(T("udta"));
    Node* meta = udta ? udta->child(T("meta")) : nullptr;
    Node* ilst = meta ? meta->child(T("ilst")) : nullptr;
    if (!ilst || ilst->container) return t;
    for (auto& item : parse_ilst(ilst->data)) {
        if (!item.container) continue;
        std::vector<const Node*> data;
        for (auto& c : item.children)
            if (!c.container && c.typ == T("data") && c.data.size() >= 8) data.push_back(&c);
        if (data.empty()) continue;
        auto payload = [](const Node* d) { return std::span<const uint8_t>(d->data).subspan(8); };
        uint32_t type = be32(data[0]->data.data()) & 0xFFFFFF;
        if (item.typ == T("trkn") || item.typ == T("disk")) {
            auto p = payload(data[0]);
            if (p.size() < 6) continue;
            bool trk = item.typ == T("trkn");
            if (uint16_t n = be16(&p[2])) t.add(trk ? "TRACKNUMBER" : "DISCNUMBER", std::to_string(n));
            if (uint16_t n = be16(&p[4])) t.add(trk ? "TRACKTOTAL" : "DISCTOTAL", std::to_string(n));
            continue;
        }
        if (item.typ == T("covr") || item.typ == T("gnre")) continue;
        std::string name;
        if (item.typ == T("----")) {
            auto n = freeform_name(item);
            if (!n) continue;
            name = *n;
        } else {
            for (uint8_t c : item.typ) {
                if (c == 0xA9) name += "©";
                else name += char(c);
            }
            if (auto o = mp4_owner(name)) name = o->name;
        }
        for (auto* d : data) {
            auto p = payload(d);
            if (type == 21 || type == 0) {  // integers (cpil, tmpo, rtng ...)
                uint64_t v = 0;
                for (auto b : p) v = v << 8 | b;
                if (p.size() <= 8 && !p.empty()) t.add(name, std::to_string(v));
            } else {
                t.add(name, lossy_utf8(p));
            }
        }
    }
    return t;
}

Retagged retag_m4a(const fs::path& p, const Tags& overlay) {
    File f(p);
    auto [mpos, mlen] = find_moov([&](uint64_t off, size_t len) { return f.read_vec(off, len); }, f.size());
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
    // padding next to the item list, so tag editors can save in place
    if (!meta->child(T("free"))) meta->children.push_back(leaf(T("free"), Bytes(2048, 0)));

    int64_t delta = int64_t(root.len()) - int64_t(mlen);
    if (delta != 0) shift_offsets(root, mpos + mlen, delta);
    Retagged r{mpos, mlen, {}};
    r.moov.reserve(size_t(root.len()));
    root.write(r.moov);
    return r;
}

}  // namespace wm::mp4
