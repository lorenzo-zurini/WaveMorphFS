// SPDX-License-Identifier: GPL-3.0-or-later
// M4A/MP4 retagging: the moov/udta/meta/ilst item list is rewritten in memory and
// the file is served as [bytes before moov] + new moov + [bytes after moov]. When
// moov sits before the media data, its size change is compensated in every chunk
// offset table (stco/co64).
//
// Items the overlay does not name are kept byte for byte (cover art included).
// Tag names are never rewritten: names that are not a known iTunes atom become
// freeform `----:com.apple.iTunes:<name>` items with the name exactly as written.
#pragma once

#include "tags.hpp"
#include "util.hpp"

namespace wm::mp4 {

struct Retagged {
    uint64_t moov_pos, moov_len;  // the source's moov box
    Bytes moov;                   // its replacement
};

Retagged retag_m4a(const fs::path& p, const Tags& overlay);

}  // namespace wm::mp4
