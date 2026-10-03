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
//     },
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
// "_hide": true in a track or file table hides that track or file. A top-level
// "_hide": ["name", ...] hides files of the folder by name: a source CUE sheet,
// image or SACD ISO (with all its tracks; the remaining discs are numbered as if
// it were not there), or any entry of the mount. "_target" places files elsewhere
// in the mount: at the top level it moves the whole folder (subfolders follow
// unless they have their own), in a track or file table it moves that one file;
// the value is a directory path relative to the mount root ("Classical Music/
// Mahler/Symphony No. 5 (Bernstein, 1987)"). Targets are read from the sidecars
// of the tags tree. Other top-level keys (e.g. "_comment") are ignored, and // or
// /* */ comments are allowed.
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
/// "a//b/ c" -> "a/b/ c"; nullopt for empty paths and "." / ".." / hidden components.
std::optional<std::string> normalize_target(std::string_view p);
/// Track key as the listing spells it: "2-05", "2.5", "2-5" -> "2-05"; "05" -> "5".
std::optional<std::string> canonical_track_key(std::string_view k);

struct Sidecar {
    Tags album;
    std::map<std::string, Tags> tracks;
    std::map<std::string, Tags> files;
    std::map<std::string, std::string> track_names;  // pinned file names ("_name")
    std::set<std::string> hidden;                     // "_hide": names, and file tables with "_hide": true
    std::set<std::string> hidden_tracks;              // track tables with "_hide": true
    std::optional<std::string> target;                // "_target" of the folder (normalized)
    std::map<std::string, std::string> track_targets;  // canonical track key -> "_target"
    std::map<std::string, std::string> file_targets;   // file name -> "_target"
    /// newest mtime among the sidecar files that contributed
    std::optional<int64_t> mtime;
    std::vector<fs::path> sources;

    static Sidecar parse(const std::string& text);
    /// Merge the sidecars that apply to `src_dir`; nullopt if there are none.
    static std::optional<Sidecar> load(const fs::path& src_dir, const std::optional<fs::path>& overlay_dir);
    /// Tags for track `n` of disc `disc` (disc only matters in multi-disc dirs).
    const Tags* track(std::optional<uint32_t> disc, uint32_t n) const;
    const Tags* file(const std::string& name) const;
    /// Pinned file name of track `n` of disc `disc`, if any.
    const std::string* track_name(std::optional<uint32_t> disc, uint32_t n) const;
    bool hides(const std::string& name) const { return hidden.contains(name); }
    bool hides_track(std::optional<uint32_t> disc, uint32_t n) const;
    /// Serialize tags to sidecar JSON (used by `tags-init`).
    static std::string render(const Tags& album, const std::vector<std::pair<std::string, Tags>>& tracks,
                              const std::vector<std::pair<std::string, Tags>>& files);
};

}  // namespace wm
