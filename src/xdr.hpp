// SPDX-License-Identifier: GPL-3.0-or-later
// XDR (RFC 4506), the encoding of ONC RPC messages: big-endian 32-bit units,
// variable-length data prefixed by its length and padded to a multiple of 4.
#pragma once

#include <span>
#include <stdexcept>
#include <string>

#include "util.hpp"

namespace wm::xdr {

/// Input that is not valid XDR for what was expected (truncated, too long).
struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

inline size_t padded(size_t n) { return (n + 3) & ~size_t(3); }

/// Reads a message; every read is bounds-checked and throws Error.
class In {
public:
    explicit In(std::span<const uint8_t> b) : b_(b) {}
    uint32_t u32() { return be32(take(4).data()); }
    uint64_t u64() { return be64(take(8).data()); }
    bool boolean() {
        uint32_t v = u32();
        if (v > 1) throw Error("bad boolean");
        return v;
    }
    /// Fixed-length opaque data of `n` bytes (padded).
    std::span<const uint8_t> fixed(size_t n) {
        auto s = take(n);
        take(padded(n) - n);
        return s;
    }
    /// Variable-length opaque data of at most `max` bytes.
    std::span<const uint8_t> opaque(size_t max) {
        uint32_t n = u32();
        if (n > max) throw Error("opaque data too long");
        return fixed(n);
    }
    std::string str(size_t max) { return std::string(str_of(opaque(max))); }
    size_t left() const { return b_.size() - p_; }
    std::span<const uint8_t> rest() const { return b_.subspan(p_); }

private:
    std::span<const uint8_t> take(size_t n) {
        if (n > left()) throw Error("message too short");
        auto s = b_.subspan(p_, n);
        p_ += n;
        return s;
    }
    std::span<const uint8_t> b_;
    size_t p_ = 0;
};

/// Builds a message.
class Out {
public:
    void u32(uint32_t v) { put_be32(b, v); }
    void u64(uint64_t v) { put_be64(b, v); }
    void boolean(bool v) { u32(v ? 1 : 0); }
    void fixed(std::span<const uint8_t> d) {
        append(b, d);
        b.resize(b.size() + padded(d.size()) - d.size(), 0);
    }
    void opaque(std::span<const uint8_t> d) {
        u32(uint32_t(d.size()));
        fixed(d);
    }
    void str(std::string_view s) { opaque(bytes_of(s)); }
    Bytes b;
};

}  // namespace wm::xdr
