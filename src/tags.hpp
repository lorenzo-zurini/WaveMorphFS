// SPDX-License-Identifier: GPL-3.0-or-later
// Tag model: names with multiple values, merged from several sources in
// increasing priority (file < cue/SACD text < sidecar).
//
// A tag name is compared case-insensitively (as Vorbis comments define), but the
// spelling it was written with is kept and used for output: names from sidecars
// and source files are never rewritten.
#pragma once

#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wm {

struct KeyLess {
    using is_transparent = void;
    bool operator()(std::string_view a, std::string_view b) const;
};

class Tags {
public:
    std::map<std::string, std::vector<std::string>, KeyLess> m;

    static Tags from_pairs(const std::vector<std::pair<std::string, std::string>>& pairs);
    void add(const std::string& key, std::string value);
    /// Replace all values of `key` (any spelling or synonym).
    void set(const std::string& key, std::string value);
    void set_many(const std::string& key, std::vector<std::string> values);
    /// Lookup by name (case-insensitive), falling back to a synonym (see same_field).
    const std::string* get(std::string_view key) const;
    const std::vector<std::string>* get_all(std::string_view key) const;
    /// Every key present in `other` replaces ours, including our entries under a
    /// synonym of it (album_artist replaces ALBUMARTIST); the spelling from `other`
    /// is kept. An empty value list deletes the field.
    void overlay(const Tags& other);
    /// Like overlay(), but an empty value list is kept as an explicit removal, so
    /// that layered sidecar tables can still delete fields of the layers below.
    void merge(const Tags& other);
    Tags without_track_specific() const;
    std::vector<std::pair<std::string, std::string>> to_pairs() const;
    bool empty() const { return m.empty(); }
    bool operator==(const Tags& o) const;
};

/// Keys that describe a single track; they must not leak from an image file's
/// own tags (which describe the whole disc) into the split tracks.
extern const std::vector<std::string_view> TRACK_SPECIFIC;
bool is_track_specific(std::string_view key);

/// Canonical name of a tag field: different conventions for the same field
/// (Vorbis/Picard ALBUMARTIST, ffprobe album_artist, ID3 TPE2, MP4 aART,
/// "MusicBrainz Album Id" / MUSICBRAINZ_ALBUMID, ...) share one; other names map
/// to their upper-case form.
std::string canonical_field(std::string_view key);
inline bool same_field(std::string_view a, std::string_view b) { return canonical_field(a) == canonical_field(b); }

Tags from_cue_disc(const std::map<std::string, std::string>& fields);
Tags from_cue_track(const std::map<std::string, std::string>& fields);

/// A filesystem-safe version of a tag value for use in a file name.
std::string sanitize_name(std::string_view s);

}  // namespace wm
