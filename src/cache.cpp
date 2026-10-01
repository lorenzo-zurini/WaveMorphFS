// SPDX-License-Identifier: GPL-3.0-or-later
#include "cache.hpp"

#include <cstring>

#include "md5.hpp"

namespace wm {

static const char IDX_MAGIC[8] = {'W', 'M', 'I', 'D', 'X', '0', '0', '1'};

Cache::Cache(fs::path dir) : dir_(std::move(dir)) {
    for (auto sub : {"flacidx", "md5"}) fs::create_directories(dir_ / sub);
}

std::string Cache::name_for(const fs::path& src, const SrcKey& key, std::string_view extra) {
    Md5 h;
    h.update(src.string());
    h.update(std::format("\n{}\n{}\n", key.size, key.mtime_ns));
    h.update(extra);
    auto d = h.finish();
    return hex(std::span(d).subspan(0, 8));
}

fs::path Cache::idx_path(const fs::path& src, const SrcKey& key) const { return dir_ / "flacidx" / (name_for(src, key) + ".idx"); }

std::optional<flac::FrameIndex> Cache::load_index(const fs::path& src, const SrcKey& key) const {
    auto b = try_read_file(idx_path(src, key));
    if (!b || b->size() < 24 || std::memcmp(b->data(), IDX_MAGIC, 8) != 0) return std::nullopt;
    flac::FrameIndex idx;
    idx.block_size = le32(&(*b)[8]);
    idx.bps_in_header = (*b)[12] != 0;
    uint64_t n = le64(&(*b)[16]);
    if (b->size() != 24 + n * 8) return std::nullopt;
    idx.offsets.resize(size_t(n));
    for (size_t i = 0; i < n; i++) idx.offsets[i] = le64(&(*b)[24 + 8 * i]);
    return idx;
}

void Cache::store_index(const fs::path& src, const SrcKey& key, const flac::FrameIndex& idx) const {
    Bytes b(IDX_MAGIC, IDX_MAGIC + 8);
    put_le32(b, idx.block_size);
    b.push_back(idx.bps_in_header ? 1 : 0);
    b.insert(b.end(), 3, 0);
    put_le64(b, idx.offsets.size());
    for (auto o : idx.offsets) put_le64(b, o);
    atomic_write(idx_path(src, key), b);
}

bool Cache::image_is_cached(const fs::path& src) const {
    auto key = SrcKey::try_of(src);
    if (!key) return false;
    return fs::exists(av_index_path(src, *key)) || (ext_lower(src) == "flac" && fs::exists(idx_path(src, *key)));
}

flac::FrameIndex Cache::index_for(const fs::path& src, const flac::FlacMeta& meta) const {
    auto key = SrcKey::of(src);
    if (auto i = load_index(src, key)) return *i;
    auto idx = flac::FrameIndex::build(src, meta);
    // the file must not have changed while we read it (e.g. still downloading)
    WM_ENSURE(SrcKey::of(src) == key, "{} changed while indexing", src.string());
    store_index(src, key, idx);
    return idx;
}

fs::path Cache::md5_path(const fs::path& flac, const SrcKey& key) const { return dir_ / "md5" / (name_for(flac, key) + ".txt"); }

Md5Map Cache::load_md5s(const fs::path& flac) const {
    Md5Map m;
    auto key = SrcKey::try_of(flac);
    if (!key) return m;
    auto text = try_read_text(md5_path(flac, *key));
    if (!text) return m;
    for (auto& line : split(*text, '\n')) {
        std::vector<std::string> w;
        for (auto& x : split(line, ' '))
            if (!x.empty()) w.push_back(x);
        if (w.size() < 3 || w[2].size() != 32) continue;
        auto a = parse_u64(w[0]), b = parse_u64(w[1]);
        if (!a || !b) continue;
        std::array<uint8_t, 16> d;
        bool ok = true;
        for (size_t i = 0; i < 16 && ok; i++) {
            char* end;
            std::string byte = w[2].substr(2 * i, 2);
            d[i] = uint8_t(std::strtoul(byte.c_str(), &end, 16));
            ok = *end == 0 && std::isxdigit(uint8_t(byte[0])) && std::isxdigit(uint8_t(byte[1]));
        }
        if (ok) m[{*a, *b}] = d;
    }
    return m;
}

void Cache::store_md5s(const fs::path& flac, const Md5Map& entries) const {
    auto key = SrcKey::of(flac);
    auto m = load_md5s(flac);
    for (auto& [r, d] : entries) m[r] = d;
    std::vector<std::string> lines;
    for (auto& [r, d] : m) lines.push_back(std::format("{} {} {}", r.first, r.second, hex(d)));
    std::sort(lines.begin(), lines.end());
    std::string text;
    for (auto& l : lines) text += l + "\n";
    atomic_write(md5_path(flac, key), text);
}

fs::path Cache::av_index_path(const fs::path& src, const SrcKey& key) const { return dir_ / "flacidx" / (name_for(src, key, "av") + ".avidx"); }

fs::path Cache::sacd_frames_path(const fs::path& src, const SrcKey& key, bool multichannel) const {
    return dir_ / "flacidx" / (name_for(src, key, multichannel ? "sacd-mc" : "sacd") + ".sacdidx");
}

}  // namespace wm
