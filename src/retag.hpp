// SPDX-License-Identifier: GPL-3.0-or-later
// Regular files with sidecar tags applied: only the tag area is rebuilt, the
// audio is served straight from the source file.
#pragma once

#include <memory>

#include "tags.hpp"
#include "vfile.hpp"

namespace wm {

/// FLAC: the VORBIS_COMMENT block is replaced (source tags overlaid with `overlay`);
/// all other metadata blocks are kept. MP3: new ID3v2.4 tag. M4A: new ilst.
/// DSF: new ID3v2.4 tag at the end, DSD chunk sizes patched.
/// `ext` is the lower-case extension ("flac", "mp3", "m4a", "dsf").
std::shared_ptr<Spliced> retag_file(const fs::path& p, const std::string& ext, const Tags& overlay);

}  // namespace wm
