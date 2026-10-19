// SPDX-License-Identifier: GPL-3.0-or-later
#include "apetag.hpp"

#include <cstring>

namespace wm::ape {

namespace {

constexpr uint32_t HAS_HEADER = 0x80000000, IS_HEADER = 0x20000000;
constexpr uint32_t ITEM_BINARY = 1 << 1;
constexpr std::string_view FRONT_COVER = "Cover Art (Front)";

/// APEv2 keys: 2..255 printable ASCII characters, and not one of the reserved words.
bool valid_key(std::string_view k) {
    if (k.size() < 2 || k.size() > 255) return false;
    for (char c : k)
        if (c < 0x20 || c > 0x7E) return false;
    for (auto r : {"ID3", "TAG", "OggS", "MP+"})
        if (iequals(k, r)) return false;
    return true;
}

Bytes header_or_footer(bool header, uint32_t tag_size, uint32_t count) {
    Bytes b;
    append(b, std::string_view("APETAGEX"));
    put_le32(b, 2000);
    put_le32(b, tag_size);
    put_le32(b, count);
    put_le32(b, HAS_HEADER | (header ? IS_HEADER : 0));
    b.insert(b.end(), 8, 0);
    return b;
}

void put_item_head(Bytes& b, std::string_view key, uint64_t value_len, uint32_t flags) {
    put_le32(b, uint32_t(value_len));
    put_le32(b, flags);
    append(b, key);
    b.push_back(0);
}

}  // namespace

std::optional<Tag> read(const ByteReader& read, uint64_t size) {
    uint64_t end = size;
    if (size >= 128) {
        Bytes v1 = read(size - 128, 3);
        if (v1.size() == 3 && std::memcmp(v1.data(), "TAG", 3) == 0) end = size - 128;
    }
    if (end < 32) return std::nullopt;
    Bytes f = read(end - 32, 32);
    if (f.size() < 32 || std::memcmp(f.data(), "APETAGEX", 8) != 0) return std::nullopt;
    uint32_t version = le32(&f[8]), tag_size = le32(&f[12]), count = le32(&f[16]), flags = le32(&f[20]);
    if (tag_size < 32 || tag_size > end || (flags & IS_HEADER)) return std::nullopt;
    uint64_t items_start = end - tag_size;
    Tag tag;
    tag.start = (flags & HAS_HEADER) && items_start >= 32 ? items_start - 32 : items_start;
    Bytes b = read(items_start, tag_size - 32);
    if (b.size() != tag_size - 32) return std::nullopt;
    size_t p = 0;
    for (uint32_t i = 0; i < count && p + 9 <= b.size(); i++) {
        uint32_t len = le32(&b[p]), iflags = le32(&b[p + 4]);
        size_t k0 = p + 8, k1 = k0;
        while (k1 < b.size() && b[k1] != 0) k1++;
        if (k1 >= b.size() || k1 + 1 + uint64_t(len) > b.size()) break;
        std::string key(str_of(std::span(b).subspan(k0, k1 - k0)));
        size_t v0 = k1 + 1;
        p = v0 + len;
        if (version >= 2000 && (iflags >> 1 & 3) != 0) {  // binary or external locator
            if ((iflags >> 1 & 3) == 1 && iequals(key, FRONT_COVER)) tag.front_cover = SrcRange{items_start + v0, len};
            continue;
        }
        std::vector<std::string> values;
        for (auto& v : split(lossy_utf8(std::span(b).subspan(v0, len)), '\0'))
            if (!v.empty()) values.push_back(v);
        if (values.empty()) continue;
        if (iequals(key, "Track") || iequals(key, "Disc")) {
            bool track = iequals(key, "Track");
            auto parts = split(values[0], '/');
            tag.tags.add(track ? "TRACKNUMBER" : "DISCNUMBER", trim(parts[0]));
            if (parts.size() > 1) tag.tags.add(track ? "TRACKTOTAL" : "DISCTOTAL", trim(parts[1]));
        } else {
            std::string name = iequals(key, "Year") ? "DATE" : key;
            for (auto& v : values) tag.tags.add(name, v);
        }
    }
    return tag;
}

std::vector<Seg> build(const Tags& tags, const Cover* cover, std::optional<SrcRange> source_cover) {
    Bytes items;
    uint32_t count = 0;
    std::vector<std::string> used;  // keys are unique regardless of case
    auto add = [&](const std::string& key, const std::vector<std::string>& values) {
        if (values.empty()) return;
        if (!valid_key(key)) {
            debug("APEv2: tag name \"{}\" cannot be written", key);
            return;
        }
        for (auto& u : used)
            if (iequals(u, key)) return;
        used.push_back(key);
        std::string v;
        for (size_t i = 0; i < values.size(); i++) {
            if (i) v += '\0';
            v += values[i];
        }
        put_item_head(items, key, v.size(), 0);
        append(items, v);
        count++;
    };
    for (auto [num, total, key] : {std::tuple{"TRACKNUMBER", "TRACKTOTAL", "Track"}, std::tuple{"DISCNUMBER", "DISCTOTAL", "Disc"}}) {
        auto n = tags.get(num);
        if (!n) continue;
        auto t = tags.get(total);
        add(key, {t && n->find('/') == std::string::npos ? *n + "/" + *t : *n});
    }
    bool has_year = false;
    for (auto& [k, v] : tags.m)
        if (canonical_field(k) == "YEAR") has_year = true;
    for (auto& [k, vs] : tags.m) {
        std::string c = canonical_field(k);
        if (c == "TRACKNUMBER" || c == "TRACKTOTAL" || c == "DISCNUMBER" || c == "DISCTOTAL") continue;
        if (c == "ALBUMARTIST") add("Album Artist", vs);
        else if (c == "DATE" && !has_year) add("Year", vs);
        else add(k, vs);
    }
    std::vector<Seg> segs;
    uint64_t picture_len = 0;
    Bytes pic_head;
    Seg pic_body = Bytes{};
    if (cover) {
        std::string name = cover->mime == "image/png" ? "cover.png" : "cover.jpg";
        put_item_head(pic_head, FRONT_COVER, name.size() + 1 + cover->size, ITEM_BINARY);
        append(pic_head, name);
        pic_head.push_back(0);
        pic_body = FileRange{cover->path, 0, cover->size};
        count++;
    } else if (source_cover) {
        put_item_head(pic_head, FRONT_COVER, source_cover->len, ITEM_BINARY);
        pic_body = *source_cover;
        count++;
    }
    if (!pic_head.empty()) picture_len = pic_head.size() + seg_size(pic_body);
    uint32_t tag_size = uint32_t(items.size() + picture_len + 32);
    Bytes head = header_or_footer(true, tag_size, count);
    append(head, items);
    segs.push_back(std::move(head));
    if (!pic_head.empty()) {
        segs.push_back(std::move(pic_head));
        segs.push_back(std::move(pic_body));
    }
    segs.push_back(header_or_footer(false, tag_size, count));
    return segs;
}

}  // namespace wm::ape
