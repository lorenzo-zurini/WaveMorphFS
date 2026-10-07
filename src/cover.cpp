// SPDX-License-Identifier: GPL-3.0-or-later
#include "cover.hpp"

#include <cstring>

namespace wm {

std::optional<Cover> load_cover(const fs::path& p) {
    auto m = mtime_ns(p);
    std::error_code ec;
    auto size = fs::file_size(p, ec);
    if (!m || ec) {
        warn("cover {}: not found", p.string());
        return std::nullopt;
    }
    if (size > MAX_COVER_BYTES) {
        warn("cover {}: {} bytes, more than the {} allowed", p.string(), size, MAX_COVER_BYTES);
        return std::nullopt;
    }
    Bytes h;
    try {
        File f(p);
        h = f.read_vec(0, size_t(std::min<uint64_t>(size, 65536)));
    } catch (const std::exception& e) {
        warn("cover {}: {}", p.string(), e.what());
        return std::nullopt;
    }
    Cover c;
    c.path = std::make_shared<const fs::path>(p);
    c.size = size;
    c.mtime = *m;
    if (h.size() >= 26 && std::memcmp(h.data(), "\x89PNG\r\n\x1a\n", 8) == 0 && std::memcmp(&h[12], "IHDR", 4) == 0) {
        c.mime = "image/png";
        c.width = be32(&h[16]);
        c.height = be32(&h[20]);
        static const uint32_t channels[] = {1, 0, 3, 1, 2, 0, 4};
        uint8_t ct = h[25];
        c.depth = h[24] * (ct < 7 ? channels[ct] : 0);
        return c;
    }
    if (h.size() >= 4 && h[0] == 0xFF && h[1] == 0xD8) {
        c.mime = "image/jpeg";
        // walk the markers to the frame header (SOFn) for the dimensions
        for (size_t i = 2; i + 9 < h.size();) {
            if (h[i] != 0xFF) break;
            uint8_t mk = h[i + 1];
            if (mk == 0xFF) {
                i++;
                continue;
            }
            uint16_t len = uint16_t(h[i + 2] << 8 | h[i + 3]);
            if (mk >= 0xC0 && mk <= 0xCF && mk != 0xC4 && mk != 0xC8 && mk != 0xCC) {
                c.height = uint32_t(h[i + 5] << 8 | h[i + 6]);
                c.width = uint32_t(h[i + 7] << 8 | h[i + 8]);
                c.depth = uint32_t(h[i + 4]) * h[i + 9];
                break;
            }
            i += 2 + len;
        }
        return c;
    }
    warn("cover {}: not a JPEG or PNG image", p.string());
    return std::nullopt;
}

std::vector<Seg> flac_picture(const Cover& c) {
    Bytes b;
    put_be32(b, 3);  // front cover
    put_be32(b, uint32_t(c.mime.size()));
    append(b, c.mime);
    put_be32(b, 0);  // description
    put_be32(b, c.width);
    put_be32(b, c.height);
    put_be32(b, c.depth);
    put_be32(b, 0);  // colours (indexed images only)
    put_be32(b, uint32_t(c.size));
    return {std::move(b), FileRange{c.path, 0, c.size}};
}

std::vector<Seg> id3_apic(const Cover& c) {
    Bytes b = {0};  // ISO-8859-1 text
    append(b, c.mime);
    b.push_back(0);
    b.push_back(3);  // front cover
    b.push_back(0);  // empty description
    return {std::move(b), FileRange{c.path, 0, c.size}};
}

}  // namespace wm
