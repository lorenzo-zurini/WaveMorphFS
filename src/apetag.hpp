// SPDX-License-Identifier: GPL-3.0-or-later
// APEv2 tags (the tag format of WavPack files): read from the end of a file and
// built for generated WavPack tracks.
//
// Names are kept as given, except the fields APEv2 spells its own way: track
// and disc numbers ("Track", "Disc", as "n/total"), the date ("Year") and the
// album artist ("Album Artist"). Reading maps those back to Vorbis-style names,
// as the ID3 reader does with its frames.
#pragma once

#include <optional>

#include "cover.hpp"
#include "tags.hpp"
#include "vfile.hpp"

namespace wm::ape {

struct Tag {
    Tags tags;
    uint64_t start = 0;  // where the tag (header, or first item) begins in the file
    /// value of the "Cover Art (Front)" item (file name, NUL, image), if any
    std::optional<SrcRange> front_cover;
};

/// The APEv2 tag at the end of a file (before an ID3v1 tag, if there is one).
std::optional<Tag> read(const ByteReader& read, uint64_t size);

/// A complete tag (header, items, footer) for `tags`. The front cover is
/// `cover` if given, else `source_cover` (a "Cover Art (Front)" value of the
/// source file); both are referenced, not copied.
std::vector<Seg> build(const Tags& tags, const Cover* cover, std::optional<SrcRange> source_cover);

}  // namespace wm::ape
