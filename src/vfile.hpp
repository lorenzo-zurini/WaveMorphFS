// SPDX-License-Identifier: GPL-3.0-or-later
// Virtual file abstraction: every entry the filesystem exposes is a VFile with
// an exact size known up front and random-access reads.
#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "util.hpp"

namespace wm {

class VFile {
public:
    virtual ~VFile() = default;
    virtual uint64_t size() const = 0;
    /// Read up to `len` bytes at `off`; shorter only at EOF.
    virtual Bytes read_at(uint64_t off, size_t len) const = 0;
    /// Where the bytes come from (for xattrs / debugging), "kind:path...".
    virtual std::string describe() const = 0;
};

using VFilePtr = std::shared_ptr<const VFile>;

/// A source file exposed unchanged.
class Passthrough : public VFile {
public:
    Passthrough(fs::path p, uint64_t size) : path_(std::move(p)), size_(size) {}
    uint64_t size() const override { return size_; }
    Bytes read_at(uint64_t off, size_t len) const override;
    std::string describe() const override { return "passthrough:" + path_.string(); }

private:
    fs::path path_;
    uint64_t size_;
};

/// Copy the part of `seg` (starting at virtual offset `seg_start`) that overlaps
/// the request [off, off+len) into `out`.
void copy_overlap(Bytes& out, std::span<const uint8_t> seg, uint64_t seg_start, uint64_t off, size_t len);

/// One part of a generated file for `stitch`: `size` bytes, of which
/// `read(rel_off, n, out)` appends [rel_off, rel_off + n) to `out`.
template <class Read>
struct Piece {
    uint64_t size;
    Read read;
};
template <class Read>
Piece(uint64_t, Read) -> Piece<Read>;

/// Lay `pieces` end to end from offset 0 and append to `out` what each covers of
/// the request [off, off+len); pieces the request misses are not read.
template <class... Read>
void stitch(Bytes& out, uint64_t off, size_t len, const Piece<Read>&... pieces) {
    uint64_t pos = 0, end = off + len;
    auto one = [&](const auto& p) {
        uint64_t a = std::max(off, pos), b = std::min(end, pos + p.size);
        if (a < b) p.read(a - pos, size_t(b - a), out);
        pos += p.size;
    };
    (one(pieces), ...);
}

/// Bytes made on first use and shared by later readers (e.g. a track's re-encoded
/// boundary frames). Two first readers may both make them; one result is kept.
class LazyBytes {
public:
    using Shared = std::shared_ptr<const Bytes>;
    template <class Make>
    Shared get(Make make) const {
        {
            std::lock_guard g(mu_);
            if (bytes_) return bytes_;
        }
        auto b = std::make_shared<const Bytes>(make());  // outside the lock: may take long
        std::lock_guard g(mu_);
        if (!bytes_) bytes_ = std::move(b);
        return bytes_;
    }
    void reset() const {
        std::lock_guard g(mu_);
        bytes_.reset();
    }

private:
    mutable std::mutex mu_;
    mutable Shared bytes_;
};

/// A piece of generated output: bytes in memory, (offset, length) of a source
/// file, or zeros. Large parts that already exist in the source (cover art,
/// audio) are referenced rather than held, so generated files cost little memory.
struct SrcRange {
    uint64_t off, len;
};
struct Zeros {
    uint64_t len;
};
/// (offset, length) of another file (e.g. a cover image shared by many tracks).
struct FileRange {
    std::shared_ptr<const fs::path> path;
    uint64_t off, len;
};
using Seg = std::variant<Bytes, SrcRange, Zeros, FileRange>;
uint64_t seg_size(const Seg& s);

/// Segments laid end to end from offset 0 (adjacent in-memory bytes are merged).
class Segments {
public:
    Segments() = default;
    explicit Segments(std::vector<Seg> segs);
    uint64_t size() const { return size_; }
    /// Append the part of [off, off+len) these segments cover; source ranges are read from `src`.
    void read(const fs::path& src, uint64_t off, size_t len, Bytes& out) const;
    /// The same with the segments placed at offset `base` of the request's file.
    void read_at_base(const fs::path& src, uint64_t base, uint64_t off, size_t len, Bytes& out) const;

private:
    std::vector<std::pair<uint64_t, Seg>> segs_;  // with start offsets
    uint64_t size_ = 0;
};

/// A source file with some byte ranges replaced (retagged FLAC / MP3 / M4A).
class Spliced : public VFile {
public:
    Spliced(fs::path p, std::string what, std::vector<Seg> segs) : path_(std::move(p)), what_(std::move(what)), segs_(std::move(segs)) {}
    uint64_t size() const override { return segs_.size(); }
    Bytes read_at(uint64_t off, size_t len) const override;
    std::string describe() const override { return what_ + ":" + path_.string(); }

private:
    fs::path path_;
    std::string what_;
    Segments segs_;
};

}  // namespace wm
