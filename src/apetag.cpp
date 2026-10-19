// SPDX-License-Identifier: GPL-3.0-or-later
#include "apetag.hpp"

#include <algorithm>
#include <cstring>
#include <tuple>

namespace wm::ape {

namespace {

constexpr uint32_t HAS_HEADER = 0x80000000, IS_HEADER = 0x20000000;
constexpr uint32_t ITEM_BINARY = 1 << 1;
constexpr std::string_view FRONT_COVER = "Cover Art (Front)";
/// Longest text value read; longer ones (no tag writer makes them) are skipped.
constexpr uint32_t MAX_TEXT = 1 << 20;

/// APEv2 keys: 2..255 printable ASCII characters, and not one of the reserved words.
bool valid_key(std::string_view k) {
    if (k.size() < 2 || k.size() > 255) return false;
    for (char c : k)
        if (c < 0x20 || c > 0x7E) return false;
    for (auto r : {"ID3", "TAG", "OggS", "MP+"})
        if (iequals(k, r)) return false;
    return true;
}

Bytes header_or_footer(bool header, uint64_t tag_size, uint32_t count) {
    Bytes b;
    append(b, std::string_view("APETAGEX"));
    put_le32(b, 2000);
    put_le32(b, uint32_t(tag_size));
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
    // Items are read one by one and binary ones (pictures) only located: a
    // corrupt footer can claim gigabytes, and the cover is served by reference.
    Tag tag;
    std::vector<std::pair<std::string, std::string>> numbers;  // Track/Disc, added last (see add_number)
    uint64_t p = end - tag_size, items_end = end - 32;
    for (uint32_t i = 0; i < count && p + 9 <= items_end; i++) {
        Bytes h = read(p, size_t(std::min<uint64_t>(8 + 256, items_end - p)));  // header, key (at most 255) and NUL
        auto nul = std::find(h.begin() + std::min<ptrdiff_t>(8, ptrdiff_t(h.size())), h.end(), 0);
        if (h.size() < 9 || nul == h.end()) break;
        uint32_t len = le32(&h[0]), iflags = le32(&h[4]);
        std::string key(h.begin() + 8, nul);
        uint64_t v0 = p + uint64_t(nul - h.begin()) + 1;
        if (len > items_end - v0) break;
        p = v0 + len;
        if (version >= 2000 && (iflags >> 1 & 3) != 0) {  // binary or external locator
            if ((iflags >> 1 & 3) == 1 && iequals(key, FRONT_COVER)) tag.front_cover = SrcRange{v0, len};
            continue;
        }
        if (len > MAX_TEXT) {
            debug("APEv2: skipping the {} byte value of \"{}\"", len, key);
            continue;
        }
        Bytes v = read(v0, len);
        if (v.size() != len) break;
        std::vector<std::string> values;
        for (auto& x : split(lossy_utf8(v), '\0'))
            if (!x.empty()) values.push_back(x);
        if (values.empty()) continue;
        if (iequals(key, "Track") || iequals(key, "Disc")) numbers.emplace_back(key, values[0]);
        else
            for (auto& x : values) tag.tags.add(key, x);
    }
    for (auto& [key, v] : numbers) {
        bool track = iequals(key, "Track");
        add_number(tag.tags, track ? "TRACKNUMBER" : "DISCNUMBER", track ? "TRACKTOTAL" : "DISCTOTAL", v);
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
        std::string v = join(values, std::string_view("\0", 1));
        put_item_head(items, key, v.size(), 0);
        append(items, v);
        count++;
    };
    for (auto [num, total, key] : {std::tuple{"TRACKNUMBER", "TRACKTOTAL", "Track"}, std::tuple{"DISCNUMBER", "DISCTOTAL", "Disc"}})
        if (auto n = tags.get(num)) add(key, {join_number(*n, tags.get(total))});
    bool has_year = tags.get_all("YEAR") != nullptr;
    for (auto& [k, vs] : tags.m) {
        if (is_number_field(k)) continue;
        std::string c = canonical_field(k);
        if (c == "ALBUMARTIST") add("Album Artist", vs);
        else if (c == "DATE") {
            if (!has_year) add("Year", vs);
        } else add(k, vs);
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
    uint64_t tag_size = items.size() + picture_len + 32;
    WM_ENSURE(tag_size <= UINT32_MAX, "APEv2 tag of {} bytes", tag_size);
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
