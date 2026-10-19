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
    /// Same names (case-insensitive) and values; used by the tests.
    bool operator==(const Tags& o) const;

private:
    void replace_fields_of(const Tags& other, bool keep_removals);
};

/// Keys that describe a single track; they must not leak from an image file's
/// own tags (which describe the whole disc) into the split tracks.
bool is_track_specific(std::string_view key);

/// A tag field and where ID3v2.4 and MP4 store it. Several fields can share a
/// place (YEAR and DATE both go to TDRC/©day, WORK and GROUPING to TIT1): the
/// first field naming a place owns it, so the place's id is a synonym of that
/// field and a tag reader reports the place under that field's name; the other
/// fields are written there only when the owner is not given.
struct FieldSpec {
    std::string_view name;  // canonical (Vorbis/Picard style) name
    std::string_view id3;   // ID3v2.4 frame ("" if none: TXXX by the given name)
    std::string_view mp4;   // iTunes atom, "©" in UTF-8 ("" if none: freeform item)
};
/// The field `key` names, under any spelling or synonym; nullptr if not a known one.
const FieldSpec* field_spec(std::string_view key);
/// The field that owns ID3 frame `id` / MP4 atom `atom` (see FieldSpec); nullptr if none.
const FieldSpec* id3_owner(std::string_view id);
const FieldSpec* mp4_owner(std::string_view atom);

/// Canonical name of a tag field: different conventions for the same field
/// (Vorbis/Picard ALBUMARTIST, ffprobe album_artist, ID3 TPE2, MP4 aART,
/// "MusicBrainz Album Id" / MUSICBRAINZ_ALBUMID, ...) share one; other names map
/// to their upper-case form.
std::string canonical_field(std::string_view key);
bool same_field(std::string_view a, std::string_view b);

/// TRACKNUMBER, TRACKTOTAL, DISCNUMBER or DISCTOTAL under any spelling: the
/// fields ID3, MP4 and APEv2 store as number/total pairs.
bool is_number_field(std::string_view key);
/// Track and disc numbers as ID3 (TRCK/TPOS) and APEv2 (Track/Disc) store them,
/// "n/total": `number`, with "/total" appended unless it carries one already.
std::string join_number(const std::string& number, const std::string* total);
/// Add a "n/total" value read from such a tag as `num_field` and `total_field`.
/// An empty number is skipped, and the total only fills a field `t` lacks: call
/// this after the tag's other items, which may carry the total on their own.
void add_number(Tags& t, const std::string& num_field, const std::string& total_field, std::string_view value);

/// The name=value pairs of `t` that a Vorbis comment can hold: its field names
/// are printable ASCII without '=' (a sidecar written for MP4 may say ©nam).
/// Other names are left out, with a debug message.
std::vector<std::pair<std::string, std::string>> vorbis_comments(const Tags& t);

Tags from_cue_disc(const std::map<std::string, std::string>& fields);
Tags from_cue_track(const std::map<std::string, std::string>& fields);

/// A filesystem-safe version of a tag value for use in a file name.
std::string sanitize_name(std::string_view s);

}  // namespace wm
