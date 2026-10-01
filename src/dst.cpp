// SPDX-License-Identifier: GPL-3.0-or-later
// (derived from FFmpeg's dstdec.c, LGPL-2.1-or-later; see dst.hpp)
#include "dst.hpp"

#include <algorithm>
#include <bit>
#include <cstring>

#include "util.hpp"

namespace wm {

namespace {

constexpr int32_t FSETS_CODE_PRED_COEFF[3][3] = {{-8, 0, 0}, {-16, 8, 0}, {-9, -5, 6}};
constexpr int32_t PROBS_CODE_PRED_COEFF[3][3] = {{-8, 0, 0}, {-16, 8, 0}, {-24, 24, -8}};

struct Bits {
    std::span<const uint8_t> data;
    size_t pos = 0;  // bit position

    int64_t left() const { return int64_t(data.size() * 8) - int64_t(pos); }
    uint32_t bit() {
        // reading past the end yields zeros, like FFmpeg's padded bit reader
        uint32_t b = (pos >> 3) < data.size() ? (data[pos >> 3] >> (7 - (pos & 7))) & 1 : 0;
        pos++;
        return b;
    }
    uint32_t bits(uint32_t n) {
        uint32_t v = 0;
        for (uint32_t i = 0; i < n; i++) v = v << 1 | bit();
        return v;
    }
    int32_t sbits(uint32_t n) {
        uint32_t v = bits(n);
        return int32_t(v << (32 - n)) >> (32 - n);
    }
    /// JPEG-LS style Rice code: unary prefix of zeros terminated by a one, then k bits.
    int32_t ur_golomb(uint32_t k) {
        int64_t limit = left();
        int32_t q = 0;
        while (bit() == 0) {
            q++;
            WM_ENSURE(q < limit, "golomb code overruns frame");
        }
        int32_t rem = k > 0 ? int32_t(bits(k)) : 0;
        return (q << k) + rem;
    }
    int32_t sr_golomb(uint32_t k) {
        int32_t v = ur_golomb(k);
        return v != 0 && bit() == 1 ? -v : v;
    }
};

uint32_t log2u(uint32_t x) { return x == 0 ? 0 : 31 - uint32_t(std::countl_zero(x)); }

using Table = DstDecoder::Table;
using Map = std::array<size_t, DstDecoder::MAX_CHANNELS>;

void read_map(Bits& gb, Table& t, Map& map, size_t channels) {
    t.elements = 1;
    map[0] = 0;
    if (gb.bit() == 0) {
        for (size_t ch = 1; ch < channels; ch++) {
            uint32_t bits = log2u(uint32_t(t.elements)) + 1;
            map[ch] = gb.bits(bits);
            if (map[ch] == t.elements) {
                t.elements++;
                WM_ENSURE(t.elements < DstDecoder::MAX_ELEMENTS, "too many elements");
            } else if (map[ch] > t.elements) {
                fail("bad channel map");
            }
        }
    } else {
        map.fill(0);
    }
}

void read_table(Bits& gb, Table& t, const int32_t pred[3][3], uint32_t length_bits, uint32_t coeff_bits, bool is_signed, int32_t offset) {
    for (size_t i = 0; i < t.elements; i++) {
        t.length[i] = gb.bits(length_bits) + 1;
        auto uncoded = [&](size_t n) {
            for (size_t j = 0; j < n; j++) t.coeff[i][j] = (is_signed ? gb.sbits(coeff_bits) : int32_t(gb.bits(coeff_bits))) + offset;
        };
        if (gb.bit() == 0) {
            uncoded(t.length[i]);
        } else {
            size_t method = gb.bits(2);
            WM_ENSURE(method != 3, "bad coding method");
            uncoded(method + 1);
            uint32_t lsb_size = gb.bits(3);
            for (size_t j = method + 1; j < t.length[i]; j++) {
                int32_t x = 0;
                for (size_t k = 0; k <= method; k++) x = int32_t(uint32_t(x) + uint32_t(pred[method][k]) * uint32_t(t.coeff[i][j - k - 1]));
                int32_t c = gb.sr_golomb(lsb_size);
                if (x >= 0) c -= (x + 4) / 8;
                else c += (-x + 3) / 8;
                WM_ENSURE(is_signed || (c >= offset && c < offset + (1 << coeff_bits)), "probability out of range");
                t.coeff[i][j] = c;
            }
        }
    }
}

struct Ac {
    uint32_t a = 4095, c = 0;
    uint32_t get(Bits& gb, uint32_t p) {
        uint32_t k = (a >> 8) | ((a >> 7) & 1);
        uint32_t q = k * p;
        uint32_t a_q = a - q;
        uint32_t e = c < a_q;
        if (e) {
            a = a_q;
        } else {
            a = q;
            c -= a_q;
        }
        if (a < 2048) {
            uint32_t n = 11 - log2u(a);
            a <<= n;
            c = (c << n) | gb.bits(n);
        }
        return e;
    }
};

uint8_t reverse8(uint8_t b) {
    b = uint8_t((b & 0xF0) >> 4 | (b & 0x0F) << 4);
    b = uint8_t((b & 0xCC) >> 2 | (b & 0x33) << 2);
    return uint8_t((b & 0xAA) >> 1 | (b & 0x55) << 1);
}

uint32_t prob_dst_x_bit(int32_t c) { return uint32_t(reverse8(uint8_t(c & 127)) >> 1) + 1; }

}  // namespace

DstDecoder::DstDecoder(size_t channels)
    : channels_(channels), fsets_(std::make_unique<Table>()), probs_(std::make_unique<Table>()), filter_(MAX_ELEMENTS) {
    WM_ENSURE(channels >= 1 && channels <= MAX_CHANNELS, "DST: unsupported channel count {}", channels);
}

void DstDecoder::decode(std::span<const uint8_t> frame, std::span<uint8_t> out) {
    size_t channels = channels_;
    size_t total = SAMPLES_PER_FRAME / 8 * channels;
    WM_ENSURE(out.size() == total, "output buffer size");
    WM_ENSURE(frame.size() > 1, "empty DST frame");
    Bits gb{frame};

    if (gb.bit() == 0) {
        // uncompressed frame: raw DSD follows the first byte
        gb.bit();
        WM_ENSURE(gb.bits(6) == 0, "bad uncompressed DST frame header");
        size_t n = std::min(frame.size() - 1, total);
        std::memcpy(out.data(), frame.data() + 1, n);
        std::fill(out.begin() + ptrdiff_t(n), out.end(), 0x69);
        return;
    }
    WM_ENSURE(gb.bit() == 1, "DST: 'not same segmentation' unsupported");
    WM_ENSURE(gb.bit() == 1, "DST: 'not same segmentation for all channels' unsupported");
    WM_ENSURE(gb.bit() == 1, "DST: 'not end of channel segmentation' unsupported");

    bool same_map = gb.bit() == 1;
    Map map_f{}, map_p{};
    read_map(gb, *fsets_, map_f, channels);
    if (same_map) {
        probs_->elements = fsets_->elements;
        map_p = map_f;
    } else {
        read_map(gb, *probs_, map_p, channels);
    }
    std::array<bool, MAX_CHANNELS> half_prob{};
    for (size_t ch = 0; ch < channels; ch++) half_prob[ch] = gb.bit() == 1;
    read_table(gb, *fsets_, FSETS_CODE_PRED_COEFF, 7, 9, true, 0);
    read_table(gb, *probs_, PROBS_CODE_PRED_COEFF, 6, 7, false, 1);
    WM_ENSURE(gb.bit() == 0, "DST: bad arithmetic-coding marker");
    Ac ac;
    ac.c = gb.bits(12);

    // build_filter
    for (size_t i = 0; i < fsets_->elements; i++) {
        int32_t length = int32_t(fsets_->length[i]);
        for (int j = 0; j < 16; j++) {
            size_t taps = size_t(std::clamp(length - j * 8, 0, 8));
            for (int k = 0; k < 256; k++) {
                int64_t v = 0;
                for (size_t l = 0; l < taps; l++) {
                    int64_t bit = int64_t((k >> l) & 1) * 2 - 1;
                    v += bit * fsets_->coeff[i][size_t(j) * 8 + l];
                }
                WM_ENSURE(int64_t(int16_t(v)) == v, "DST: filter coefficient overflow");
                filter_[i][size_t(j)][size_t(k)] = int16_t(v);
            }
        }
    }

    uint64_t status[MAX_CHANNELS][2];
    for (auto& s : status) s[0] = s[1] = 0xAAAAAAAAAAAAAAAAull;
    std::fill(out.begin(), out.end(), 0);
    (void)ac.get(gb, prob_dst_x_bit(fsets_->coeff[0][0]));

    for (size_t i = 0; i < SAMPLES_PER_FRAME; i++) {
        for (size_t ch = 0; ch < channels; ch++) {
            size_t felem = map_f[ch];
            auto& filt = filter_[felem];
            uint64_t lo = status[ch][0], hi = status[ch][1];
            int32_t sum = 0;
            for (int x = 0; x < 8; x++) {
                sum += filt[size_t(x)][(lo >> (8 * x)) & 0xFF];
                sum += filt[size_t(x) + 8][(hi >> (8 * x)) & 0xFF];
            }
            int16_t predict = int16_t(sum);
            uint32_t prob;
            if (!half_prob[ch] || i >= fsets_->length[felem]) {
                size_t pelem = map_p[ch];
                size_t index = size_t(std::abs(int32_t(predict))) >> 3;
                prob = uint32_t(probs_->coeff[pelem][std::min(index, probs_->length[pelem] - 1)]);
            } else {
                prob = 128;
            }
            uint32_t residual = ac.get(gb, prob);
            uint32_t v = ((uint32_t(int32_t(predict) >> 15)) ^ residual) & 1;
            out[(i >> 3) * channels + ch] |= uint8_t(v << (7 - (i & 7)));
            status[ch][0] = lo << 1 | v;
            status[ch][1] = hi << 1 | lo >> 63;
        }
    }
}

}  // namespace wm
