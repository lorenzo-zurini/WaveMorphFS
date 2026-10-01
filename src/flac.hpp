// SPDX-License-Identifier: GPL-3.0-or-later
// FLAC primitives: metadata parsing, frame header parsing/rewriting, CRCs, a
// full-file frame indexer and a VERBATIM frame encoder.
//
// Splitting strategy (see track.hpp): frames that lie entirely inside a track
// are copied byte-for-byte, only their header is rewritten to a variable-blocksize
// header carrying the track-relative sample number (plus new CRC-8/CRC-16). The
// partial frames at track boundaries are decoded and re-emitted as VERBATIM
// frames, so the output is sample-exact without re-encoding the track.
#pragma once

#include <array>
#include <functional>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "util.hpp"

namespace wm::flac {

uint8_t crc8(std::span<const uint8_t> data);
uint16_t crc16_update(uint16_t crc, std::span<const uint8_t> data);
inline uint16_t crc16(std::span<const uint8_t> data) { return crc16_update(0, data); }

// FLAC's extended UTF-8 number coding (frame/sample numbers, up to 36 bits)
size_t coded_len(uint64_t v);
void encode_number(uint64_t v, Bytes& out);
/// (value, byte length)
std::optional<std::pair<uint64_t, size_t>> decode_number(std::span<const uint8_t> b);

struct FrameHeader {
    bool variable = false;
    uint8_t bs_code = 0, sr_code = 0, ch_code = 0, bps_code = 0;
    /// frame number (fixed blocksize) or first sample number (variable)
    uint64_t number = 0;
    size_t number_len = 0;
    uint32_t block_size = 0;
    /// bytes of explicit block-size / sample-rate fields following the number
    size_t extra_len = 0;
    /// total header length including the CRC-8 byte
    size_t len = 0;

    /// Parse and CRC-check a frame header at the start of `b`.
    static std::optional<FrameHeader> parse(std::span<const uint8_t> b);
};

/// Rewrite a header (`orig`, parsed as `h`) into a variable-blocksize header
/// carrying `sample_number`, optionally with an explicit bps code.
Bytes rewrite_header(std::span<const uint8_t> orig, const FrameHeader& h, uint64_t sample_number, std::optional<uint8_t> bps_code);
uint8_t bps_code_for(uint32_t bps);

struct StreamInfo {
    uint16_t min_block = 0, max_block = 0;
    uint32_t sample_rate = 0, channels = 0, bps = 0;
    uint64_t total_samples = 0;
    std::array<uint8_t, 16> md5{};

    static StreamInfo parse(std::span<const uint8_t> b);
    std::array<uint8_t, 34> encode(uint32_t min_frame, uint32_t max_frame) const;
};

constexpr uint8_t BLOCK_STREAMINFO = 0;
constexpr uint8_t BLOCK_PADDING = 1;
constexpr uint8_t BLOCK_VORBIS = 4;
constexpr uint8_t BLOCK_PICTURE = 6;

struct MetaBlock {
    uint8_t kind = 0;
    Bytes data;
};

struct FlacMeta {
    StreamInfo streaminfo;
    std::vector<MetaBlock> blocks;  // all but STREAMINFO and PADDING
    /// byte offset of the first audio frame
    uint64_t audio_start = 0;

    static FlacMeta read(const fs::path& p);
    std::vector<std::pair<std::string, std::string>> vorbis_comments() const;
    std::vector<const MetaBlock*> pictures() const;
};

/// Vorbis comment pairs; names keep the spelling stored in the file.
std::vector<std::pair<std::string, std::string>> parse_vorbis(std::span<const uint8_t> b);
Bytes build_vorbis(const std::string& vendor, const std::vector<std::pair<std::string, std::string>>& tags);
/// "fLaC" + STREAMINFO + blocks.
Bytes build_header(const std::array<uint8_t, 34>& si, const std::vector<MetaBlock>& blocks);

/// Byte offsets of every frame of a fixed-blocksize FLAC image, built by a single
/// sequential pass that verifies each frame's header CRC-8, frame number and
/// CRC-16. A successful build therefore also proves the file is complete (no
/// zero-filled holes from an unfinished download).
struct FrameIndex {
    uint32_t block_size = 0;
    /// offsets[k] = start of frame k; offsets[nframes] = end of the last frame
    std::vector<uint64_t> offsets;
    bool bps_in_header = false;

    static FrameIndex build(const fs::path& p, const FlacMeta& meta);
    uint64_t nframes() const { return offsets.size() - 1; }
};

/// Size in bytes of a VERBATIM frame produced by `encode_verbatim`.
uint64_t verbatim_size(uint32_t samples, uint32_t channels, uint32_t bps, uint64_t sample_number, size_t sr_extra);

/// Encode per-channel samples as one VERBATIM frame with a variable-blocksize
/// header; the sample-rate code and its explicit bytes are copied from the source.
Bytes encode_verbatim(const std::vector<std::span<const int32_t>>& chans, uint32_t bps, uint64_t sample_number, uint8_t sr_code,
                      std::span<const uint8_t> sr_extra);

/// Encode one block of per-channel samples as a compressed FLAC frame (libFLAC,
/// compression level 5) numbered `frame_number` in a fixed-blocksize stream of
/// `block_size`-sample frames. Frames are encoded independently, so the result
/// depends only on the samples and the number: the same frame can be produced
/// again at any time, byte for byte.
Bytes encode_frame(const std::vector<std::span<const int32_t>>& chans, uint32_t bps, uint32_t sample_rate, uint32_t block_size, uint64_t frame_number);

/// Decode one complete frame (first header byte to CRC-16) to per-channel samples.
/// bps and sample rate come from STREAMINFO when the header defers to it.
std::vector<std::vector<int32_t>> decode_frame(std::span<const uint8_t> frame, uint32_t bps, uint32_t sample_rate);

/// Decode a whole FLAC file, calling `sink(first_sample, per-channel samples)` per
/// frame. Decoder complaints after `expected` samples (trailing tags) are ignored.
void decode_file(const fs::path& p, uint64_t expected, const std::function<void(uint64_t, const std::vector<std::span<const int32_t>>&)>& sink);

}  // namespace wm::flac
