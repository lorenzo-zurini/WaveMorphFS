// SPDX-License-Identifier: GPL-3.0-or-later
// MD5 (RFC 1321), used for FLAC STREAMINFO audio checksums and cache names.
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace wm {

class Md5 {
public:
    Md5();
    void update(std::span<const uint8_t> data);
    void update(std::string_view s) { update({reinterpret_cast<const uint8_t*>(s.data()), s.size()}); }
    std::array<uint8_t, 16> finish();

private:
    void block(const uint8_t* p);
    uint32_t h_[4];
    uint8_t buf_[64];
    uint64_t len_ = 0;
};

}  // namespace wm
