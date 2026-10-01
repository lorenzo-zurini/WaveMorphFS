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

/// Tags of a FLAC / DSF / MP3 / M4A file (lower-case extension), read through
/// `read`. Vorbis comments keep their names; ID3 and MP4 fields are mapped back
/// to Vorbis-style names.
Tags read_file_tags(const std::string& ext, const ByteReader& read, uint64_t size);

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
    bool dirty() const;
    void mark_clean();
    const VFilePtr& original() const { return original_; }

private:
    Bytes read_locked(uint64_t off, size_t len) const;
    VFilePtr original_;
    int fd_ = -1;
    mutable std::mutex mu_;
    std::map<uint64_t, uint64_t> written_;  // [start, end) ranges held by the scratch file
    uint64_t size_;
    uint64_t original_limit_;  // original bytes are visible below this (shrinks with truncation)
    bool dirty_ = false;
};

}  // namespace wm
