// SPDX-License-Identifier: GPL-3.0-or-later
// ID3v2.4 writer: builds the tag for DSF output and retags MP3 files.
//
// Retagging keeps every frame of the source tag that the overlay does not name
// (pictures, chapters, private frames...) byte for byte; only the frames for the
// overlay's tag names are replaced. Tag names are never rewritten: a name that is
// not a known text frame becomes a TXXX frame described by the name exactly as
// written.
#pragma once

#include <utility>

#include "tags.hpp"
#include "util.hpp"

namespace wm::id3 {

/// A complete ID3v2.4 tag built from scratch (DSF output).
Bytes build(const Tags& tags);

/// Retag an MP3: the new tag and the offset where the source's audio (everything
/// after its ID3v2 tag) starts.
std::pair<Bytes, uint64_t> retag_mp3(const fs::path& p, const Tags& overlay);

// exposed for tests
struct Frame {
    char id[4];
    uint8_t flags[2] = {0, 0};
    Bytes data;
    std::string_view sid() const { return {id, 4}; }
};
struct SourceTag {
    uint8_t flags = 0;  // tag-wide flags kept (v2.4 unsynchronisation)
    std::vector<Frame> frames;
};
/// Parse an ID3v2.3/2.4 tag; returns the tag and its total length.
std::pair<SourceTag, size_t> parse_tag(std::span<const uint8_t> b);
void apply(std::vector<Frame>& frames, const Tags& overlay);
Bytes serialize(uint8_t flags, const std::vector<Frame>& frames);

}  // namespace wm::id3
