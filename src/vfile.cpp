// SPDX-License-Identifier: GPL-3.0-or-later
#include "vfile.hpp"

#include <algorithm>

namespace wm {

Bytes Passthrough::read_at(uint64_t off, size_t len) const {
    if (off >= size_) return {};
    len = size_t(std::min<uint64_t>(len, size_ - off));
    File f(path_);
    return f.read_vec(off, len);
}

void copy_overlap(Bytes& out, std::span<const uint8_t> seg, uint64_t seg_start, uint64_t off, size_t len) {
    uint64_t seg_end = seg_start + seg.size();
    uint64_t a = std::max(off, seg_start);
    uint64_t b = std::min(off + len, seg_end);
    if (a < b) out.insert(out.end(), seg.begin() + ptrdiff_t(a - seg_start), seg.begin() + ptrdiff_t(b - seg_start));
}

Spliced::Spliced(fs::path p, std::string what, std::vector<Seg> segs) : path_(std::move(p)), what_(std::move(what)) {
    for (auto& s : segs) {
        uint64_t start = size_;
        size_ += std::visit([](auto& x) -> uint64_t {
            if constexpr (std::is_same_v<std::decay_t<decltype(x)>, Bytes>) return x.size();
            else return x.len;
        }, s);
        segs_.emplace_back(start, std::move(s));
    }
}

Bytes Spliced::read_at(uint64_t off, size_t len) const {
    Bytes out;
    if (off >= size_) return out;
    uint64_t end = off + std::min<uint64_t>(len, size_ - off);
    out.reserve(size_t(end - off));
    std::unique_ptr<File> f;
    for (auto& [start, seg] : segs_) {
        if (auto* b = std::get_if<Bytes>(&seg)) {
            copy_overlap(out, *b, start, off, size_t(end - off));
            continue;
        }
        auto& r = std::get<SrcRange>(seg);
        uint64_t a = std::max(off, start), e = std::min(end, start + r.len);
        if (a >= e) continue;
        if (!f) f = std::make_unique<File>(path_);
        size_t n = size_t(e - a);
        size_t old = out.size();
        out.resize(old + n);
        out.resize(old + f->read_at(out.data() + old, n, r.off + (a - start)));
    }
    return out;
}

}  // namespace wm
