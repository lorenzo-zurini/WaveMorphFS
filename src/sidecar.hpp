// SPDX-License-Identifier: GPL-3.0-or-later
// Sidecar tag files.
//
// A sidecar is a JSON file named `wavemorph.json`, looked up (and merged, later
// wins) at:
//   1. <source dir>/wavemorph.json                          (optional, inside the source dir)
//   2. <tags dir>/<root name>/<relative dir>/wavemorph.json  (keeps source dirs pristine)
//
//   {
//     "album": {                          // applies to every track in the directory
//       "ALBUM": "Symphonie Nr. 5",
//       "ALBUMARTIST": ["Bayerisches Staatsorchester", "Wolfgang Sawallisch"]
//     },
//     "track": {
//       "3":    {"TITLE": "III. Scherzo"},  // by track number (split images / SACD)
//       "2-05": {"TITLE": "..."}            // disc 2, track 5 (several discs in one directory)
//     },                                    // ("05", "2-5", "2.5" are other spellings of a key)
//     "file": {
//       "01 - Allegro.flac": {"TITLE": "..."}  // by file name (regular files or virtual names)
//     }
//   }
//
// Values are strings or arrays of strings (numbers and booleans are taken as
// text); an empty string or empty array removes the tag. Tag names are used
// exactly as written. Names starting with "_" are not tags: "_name" in a track
// table pins that track's file name (set when it is first edited on the mount,
// so renaming by title does not move files under tag editors like beets), and
// "_hide": true in a track or file table hides that track or file ("_hide": false
// shows a file a pattern hides). A top-level "_hide": ["name", ...] hides files of
// the folder by name: a source CUE sheet, image or SACD ISO (with all its tracks;
// the remaining discs are numbered as if it were not there), or any entry of the
// mount; entries with "*", "?" or "/" are patterns (is_hide_pattern), which the
// tags tree passes down to every folder below. "_target" places files elsewhere
// in the mount: at the top level it moves the whole folder (subfolders follow
// unless they have their own), in a track or file table it moves that one file;
// the value is a directory path relative to the mount root ("Classical Music/
// Mahler/Symphony No. 5 (Bernstein, 1987)"). Targets are read from the sidecars
// of the tags tree. "_cover" (top level, in "album", or in a track or file table)
// names an image (absolute, relative to --covers-dir, or with "./" relative to
// the folder's tags-tree folder) embedded as the front cover of the tracks,
// replacing their pictures. "_keep" (a whitelist: only the names listed at any
// level are served) and "_drop" (a blacklist) are lists of tag names, "*"
// wildcards allowed (see TagFilter): at the top level or in "album" for the
// folder and, from the tags tree, every folder below it; in a track or file table
// for that one. A sidecar at the top of the tags tree sets them for the whole
// library. Other top-level keys (e.g.
// "_comment") are ignored, and // or /* */ comments are allowed; tag edits made
// through the mount rewrite the tags-tree sidecar, which drops its comments
// ("_comment" keys survive).
#pragma once

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "tags.hpp"
#include "util.hpp"

namespace wm {

inline constexpr const char* SIDECAR_NAME = "wavemorph.json";

/// "_keep" / "_drop" of one level: tag-name patterns. A pattern matches a name
/// case-insensitively and across synonyms (as sidecar names do); "*" matches any
/// run of characters ("MUSICBRAINZ_*", "*SORT").
struct TagFilter {
    std::vector<std::string> keep, drop;
    bool empty() const { return keep.empty() && drop.empty(); }
    void add(const TagFilter& o) {
        keep.insert(keep.end(), o.keep.begin(), o.keep.end());
        drop.insert(drop.end(), o.drop.begin(), o.drop.end());
    }
};
/// A "_hide" entry with "*", "?" or "/" is a pattern; others are plain file names.
inline bool is_hide_pattern(std::string_view s) { return s.find_first_of("*?/") != std::string_view::npos; }
/// Whether `path` matches "_hide" pattern `pattern`: case-insensitive, "*" and
/// "?" within one path component. (A pattern with "/" is matched against the end
/// of a file's path below the pattern's folder, or with a leading "/" against
/// the whole of it; see ListingBuild::hides.)
bool hide_pattern_matches(std::string_view pattern, std::string_view path);
/// Whether tag name `name` matches `pattern` (see TagFilter).
bool tag_pattern_matches(std::string_view pattern, std::string_view name);
/// "a//b/ c" -> "a/b/ c"; nullopt for empty paths and "." / ".." / hidden components.
std::optional<std::string> normalize_target(std::string_view p);
/// Track key as the listing spells it: "2-05", "2.5", "2-5" -> "2-05"; "05" -> "5".
std::optional<std::string> canonical_track_key(std::string_view k);
/// The canonical key of track `n` of disc `disc` ("5", or "2-05" in folders of several discs).
std::string track_key(std::optional<uint32_t> disc, uint32_t n);

struct Sidecar {
    Tags album;
    std::map<std::string, Tags> tracks;  // by canonical track key (canonical_track_key)
    std::map<std::string, Tags> files;
    std::map<std::string, std::string> track_names;  // canonical track key -> pinned file name ("_name")
    std::set<std::string> hidden;                     // "_hide": names, and file tables with "_hide": true
    std::vector<std::string> hide_patterns;           // "_hide" entries with wildcards or "/" (is_hide_pattern)
    std::set<std::string> shown;                      // file tables with "_hide": false
    std::set<std::string> hidden_tracks;              // canonical keys of track tables with "_hide": true
    // "_target"s, from parse() only: load() leaves them out (placements come from the tags tree)
    std::optional<std::string> target;                // "_target" of the folder (normalized)
    std::map<std::string, std::string> track_targets;  // canonical track key -> "_target"
    std::map<std::string, std::string> file_targets;   // file name -> "_target"
    std::optional<std::string> cover;                 // "_cover" for the folder (top level or in "album")
    std::map<std::string, std::string> track_covers;  // canonical track key -> "_cover"
    std::map<std::string, std::string> file_covers;   // file name -> "_cover"
    TagFilter filter;                                 // "_keep"/"_drop" of the folder (top level or in "album")
    std::map<std::string, TagFilter> track_filters;   // canonical track key -> its "_keep"/"_drop"
    std::map<std::string, TagFilter> file_filters;    // file name -> its "_keep"/"_drop"
    /// "_cover" for a file (by name), else for its track key, else the folder's.
    const std::string* cover_for(const std::string& name, const std::string* track_key) const;
    /// newest mtime among the sidecar files that contributed
    std::optional<int64_t> mtime;

    static Sidecar parse(const std::string& text);
    /// The sidecar files that apply to `src_dir`, in the order they are merged.
    static std::vector<fs::path> paths(const fs::path& src_dir, const std::optional<fs::path>& overlay_dir);
    /// Merge the sidecars that apply to `src_dir`; nullopt if there are none.
    static std::optional<Sidecar> load(const fs::path& src_dir, const std::optional<fs::path>& overlay_dir);
    /// Layer `later` (a sidecar file with mtime `later_mtime`) over this one.
    void merge(const Sidecar& later, int64_t later_mtime);
    /// Tags for track `n` of disc `disc` (disc only matters in multi-disc dirs).
    const Tags* track(std::optional<uint32_t> disc, uint32_t n) const;
    const Tags* file(const std::string& name) const;
    /// Pinned file name of track `n` of disc `disc`, if any.
    const std::string* track_name(std::optional<uint32_t> disc, uint32_t n) const;
    /// Whether this sidecar hides file `name` of its folder (by name or by a pattern).
    bool hides(const std::string& name) const;
    bool hides_track(std::optional<uint32_t> disc, uint32_t n) const;
    /// Serialize tags to sidecar JSON (used by `tags-init`).
    static std::string render(const Tags& album, const std::vector<std::pair<std::string, Tags>>& tracks,
                              const std::vector<std::pair<std::string, Tags>>& files);
};

}  // namespace wm
