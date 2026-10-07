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

uint64_t seg_size(const Seg& s) {
    if (auto* b = std::get_if<Bytes>(&s)) return b->size();
    if (auto* r = std::get_if<SrcRange>(&s)) return r->len;
    if (auto* f = std::get_if<FileRange>(&s)) return f->len;
    return std::get<Zeros>(s).len;
}

Segments::Segments(std::vector<Seg> segs) {
    for (auto& s : segs) {
        uint64_t n = seg_size(s);
        if (n == 0) continue;
        if (!segs_.empty())
            if (auto* prev = std::get_if<Bytes>(&segs_.back().second))
                if (auto* b = std::get_if<Bytes>(&s)) {
                    append(*prev, *b);
                    size_ += n;
                    continue;
                }
        segs_.emplace_back(size_, std::move(s));
        size_ += n;
    }
}

void Segments::read(const fs::path& src, uint64_t off, size_t len, Bytes& out) const {
    if (off >= size_) return;
    uint64_t end = off + std::min<uint64_t>(len, size_ - off);
    std::unique_ptr<File> f;
    for (auto& [start, seg] : segs_) {
        uint64_t n = seg_size(seg);
        uint64_t a = std::max(off, start), e = std::min(end, start + n);
        if (a >= e) continue;
        if (auto* b = std::get_if<Bytes>(&seg)) {
            out.insert(out.end(), b->begin() + ptrdiff_t(a - start), b->begin() + ptrdiff_t(e - start));
        } else if (auto* r = std::get_if<SrcRange>(&seg)) {
            if (!f) f = std::make_unique<File>(src);
            size_t old = out.size();
            out.resize(old + size_t(e - a));
            size_t got = f->read_at(out.data() + old, size_t(e - a), r->off + (a - start));
            WM_ENSURE(got == e - a, "short read in {}", src.string());
        } else if (auto* x = std::get_if<FileRange>(&seg)) {
            File other(*x->path);
            size_t old = out.size();
            out.resize(old + size_t(e - a));
            size_t got = other.read_at(out.data() + old, size_t(e - a), x->off + (a - start));
            WM_ENSURE(got == e - a, "short read in {} (changed since it was listed?)", x->path->string());
        } else {
            out.insert(out.end(), size_t(e - a), uint8_t(0));
        }
    }
}

void Segments::read_at_base(const fs::path& src, uint64_t base, uint64_t off, size_t len, Bytes& out) const {
    uint64_t end = off + len;
    if (end <= base || off >= base + size_) return;
    uint64_t a = std::max(off, base) - base, b = std::min(end, base + size_) - base;
    read(src, a, size_t(b - a), out);
}

Bytes Spliced::read_at(uint64_t off, size_t len) const {
    Bytes out;
    if (off >= size()) return out;
    out.reserve(size_t(std::min<uint64_t>(len, size() - off)));
    segs_.read(path_, off, len, out);
    return out;
}

}  // namespace wm
