// SPDX-License-Identifier: GPL-3.0-or-later
// Tag edits made on the mount, captured into sidecars.
//
// A tag editor opens a file on the mount for writing and changes it in place.
// Its writes go to a WriteSession: a sparse scratch file holding only the bytes
// written, layered over the original (virtual) content. When the editor closes
// the file, the tags are read back from both the original and the edited bytes
// and the differences are stored in the folder's sidecar; the scratch file is
// discarded. Source files are never written, and audio changes are ignored.
#pragma once

#include <map>
#include <mutex>

#include "tags.hpp"
#include "vfile.hpp"

namespace wm {

/// Tags of a FLAC / DSF / MP3 / M4A / WavPack file (lower-case extension), read
/// through `read`. Vorbis comments and APEv2 items keep their names; ID3 and MP4
/// fields (and APEv2 Track/Disc/Year) are mapped back to Vorbis-style names.
/// Throws if the bytes are not a complete file of that kind, so that a file an
/// editor left half-written is never taken for one whose tags were all removed.
Tags read_file_tags(const std::string& ext, const ByteReader& read, uint64_t size);

/// The front-cover image of a FLAC / DSF / MP3 / WavPack file (the picture of
/// type "front cover", else the first one), read through `read`; nullopt if it
/// has none. M4A covers are not read: sidecar covers cannot be shown in M4A
/// output, so a change to one could not be honoured.
std::optional<Bytes> read_front_cover(const std::string& ext, const ByteReader& read, uint64_t size);

/// What changed from `before` to `after`, field by field (synonyms are the same
/// field): changed or added fields with their new values, removed fields with
/// an empty value list.
Tags tag_changes(const Tags& before, const Tags& after);

class WriteSession {
public:
    WriteSession(VFilePtr original, const fs::path& scratch_dir);
    ~WriteSession();
    WriteSession(const WriteSession&) = delete;
    WriteSession& operator=(const WriteSession&) = delete;

    uint64_t size() const;
    Bytes read(uint64_t off, size_t len) const;
    void write(uint64_t off, std::span<const uint8_t> data);
    void truncate(uint64_t size);
    /// Writes and truncations so far. Storing an edit records the count it saw,
    /// so a write that lands while the edit is stored keeps the session dirty.
    uint64_t changes() const;
    void mark_stored(uint64_t changes);
    bool dirty() const;

private:
    Bytes read_locked(uint64_t off, size_t len) const;
    VFilePtr original_;
    int fd_ = -1;
    mutable std::mutex mu_;
    std::map<uint64_t, uint64_t> written_;  // [start, end) ranges held by the scratch file
    uint64_t size_ = 0;
    uint64_t original_limit_ = 0;  // original bytes are visible below this (shrinks with truncation)
    uint64_t changes_ = 0, stored_ = 0;
};

}  // namespace wm
