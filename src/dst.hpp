// SPDX-License-Identifier: GPL-3.0-or-later
// Direct Stream Transfer (DST) decoder: lossless DSD decompression used by most
// multichannel (and some stereo) SACD areas.
//
// Port of FFmpeg's libavcodec/dstdec.c, Copyright (c) 2014 Peter Ross
// <pross@xvid.org>, LGPL-2.1-or-later (used here under the GPL-3.0-or-later as
// the LGPL permits). Reference: ISO/IEC 14496-3 Part 3 Subpart 10.
//
// Output is raw DSD in SACD's native layout: MSB-first, byte-interleaved channels.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace wm {

/// Each byte with its bit order reversed: DSD bytes are MSB-first on SACD (and
/// in DST), LSB-first in DSF files.
inline constexpr std::array<uint8_t, 256> BIT_REVERSE = [] {
    std::array<uint8_t, 256> t{};
    for (size_t i = 0; i < 256; i++)
        for (size_t k = 0; k < 8; k++)
            if (i >> k & 1) t[i] |= uint8_t(0x80 >> k);
    return t;
}();

class DstDecoder {
public:
    static constexpr size_t MAX_CHANNELS = 6;
    static constexpr size_t MAX_ELEMENTS = 2 * MAX_CHANNELS;
    /// DSD64: 588 * 64 one-bit samples per channel per frame
    static constexpr size_t SAMPLES_PER_FRAME = 588 * 64;

    explicit DstDecoder(size_t channels);
    /// False for a frame that stores its DSD uncompressed.
    static bool compressed(std::span<const uint8_t> frame) { return !frame.empty() && frame[0] & 0x80; }
    /// Decode one DST frame into `out` (size SAMPLES_PER_FRAME/8 * channels).
    void decode(std::span<const uint8_t> frame, std::span<uint8_t> out);

    struct Table {
        size_t elements = 0;
        std::array<size_t, MAX_ELEMENTS> length{};
        std::array<std::array<int32_t, 128>, MAX_ELEMENTS> coeff{};
    };

private:
    size_t channels_;
    std::unique_ptr<Table> fsets_, probs_;
    std::vector<std::array<std::array<int16_t, 256>, 16>> filter_;
};

}  // namespace wm
