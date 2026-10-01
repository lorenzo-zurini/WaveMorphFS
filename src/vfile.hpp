// SPDX-License-Identifier: GPL-3.0-or-later
// Virtual file abstraction: every entry the filesystem exposes is a VFile with
// an exact size known up front and random-access reads.
#pragma once

#include <cstdint>
#include <memory>
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

/// A piece of a Spliced file: bytes in memory, or (offset, length) of the source.
struct SrcRange {
    uint64_t off, len;
};
using Seg = std::variant<Bytes, SrcRange>;

/// A source file with some byte ranges replaced (retagged FLAC / MP3 / M4A).
class Spliced : public VFile {
public:
    Spliced(fs::path p, std::string what, std::vector<Seg> segs);
    uint64_t size() const override { return size_; }
    Bytes read_at(uint64_t off, size_t len) const override;
    std::string describe() const override { return what_ + ":" + path_.string(); }

private:
    fs::path path_;
    std::string what_;
    std::vector<std::pair<uint64_t, Seg>> segs_;  // with virtual start offsets
    uint64_t size_ = 0;
};

}  // namespace wm
