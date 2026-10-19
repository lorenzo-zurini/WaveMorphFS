// SPDX-License-Identifier: GPL-3.0-or-later
// ID3v2.4 writer: builds the tag for DSF output and retags MP3 files.
//
// Retagging keeps every frame of the source tag that the overlay does not name
// (pictures, chapters, private frames...); only the frames for the overlay's
// fields are replaced. Kept frames keep their content, but the tag is written as
// ID3v2.4, so they are converted where the versions differ: unsynchronisation is
// undone, ID3v2.3 frame flags and the bytes they add move to their v2.4 places,
// v2.3 dates become TDRC/TDOR and IPLS becomes TIPL, and ID3v2.2 frames get their
// later ids (the few without a later counterpart are dropped). Tag names are
// never rewritten: a name that is not a known text frame becomes a TXXX frame
// described by the name exactly as written.
#pragma once

#include <optional>
#include <utility>

#include "cover.hpp"
#include "tags.hpp"
#include "util.hpp"

namespace wm::id3 {

/// A tag built from scratch (DSF output), with an optional front cover.
std::vector<Seg> build_segments(const Tags& tags, const Cover* cover);
/// The same without a cover, as one buffer (tests).
Bytes build(const Tags& tags);

/// Length of the ID3v2 tag whose 10-byte header is `head` (footer included);
/// nullopt if `head` is not an ID3v2 header.
std::optional<uint64_t> tag_length(std::span<const uint8_t> head);

/// The ID3v2 tag of `f` at `pos` (none if no tag starts there) with `overlay`
/// applied, and the length of the source tag. With `cover`, the source's pictures
/// are replaced by it (a front-cover APIC frame whose image is read from the
/// cover file).
std::pair<std::vector<Seg>, uint64_t> retag_at(const File& f, uint64_t pos, const Tags& overlay, const Cover* cover);

/// Tags of an ID3v2 tag (as written by this program or a tag editor), with
/// frames mapped back to Vorbis-style names (TIT2 -> TITLE, TRCK -> TRACKNUMBER
/// + TRACKTOTAL, TXXX -> its description, COMM -> COMMENT, other text frames by
/// id). Pictures and binary frames are not tags and are skipped.
Tags read_tags(std::span<const uint8_t> tag);
/// The image of an ID3v2 tag's front-cover picture (APIC type 3), else of its
/// first picture; nullopt if it has none.
std::optional<Bytes> front_picture(std::span<const uint8_t> tag);

// exposed for tests
struct Frame {
    char id[4];
    uint8_t flags[2] = {0, 0};
    Bytes data;
    std::string_view sid() const { return {id, 4}; }
};
struct SourceTag {
    std::vector<Frame> frames;  // as ID3v2.4, without unsynchronisation
};
/// Parse an ID3v2.2/2.3/2.4 tag; returns the tag and its total length.
std::pair<SourceTag, size_t> parse_tag(std::span<const uint8_t> b);
void apply(std::vector<Frame>& frames, const Tags& overlay);
Bytes serialize(const std::vector<Frame>& frames);

}  // namespace wm::id3
