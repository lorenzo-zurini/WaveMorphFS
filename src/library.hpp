// SPDX-License-Identifier: GPL-3.0-or-later
// The library: turns a source directory into the list of entries the filesystem
// exposes, and runs background workers for the slow parts (indexing images,
// verifying non-FLAC images and measuring their tracks, scanning SACD ISOs,
// per-track MD5s). Audio is never copied: every read goes to the source file.
//
// Rules per source directory:
//  * X.cue + existing single-file image   -> virtual per-track FLAC files; cue and image hidden
//    (the image stays hidden while it is incomplete or being processed)
//  * *.iso with an SACD master TOC         -> virtual per-track DSF files; ISO hidden
//  * other files                           -> passthrough (FLAC/MP3/M4A/DSF retagged if a sidecar applies)
//  * partial files, dotfiles, sidecars     -> hidden
//  * cover image from the sidecar tree     -> exposed as cover.jpg/png if the folder has none
#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <variant>

#include "cache.hpp"
#include "cover.hpp"
#include "cue.hpp"
#include "encoded.hpp"
#include "sacd.hpp"
#include "sidecar.hpp"
#include "track.hpp"
#include "vfile.hpp"

namespace wm {

using Clock = std::chrono::steady_clock;

struct Root {
    std::string name;
    fs::path path;
};

struct Config {
    std::vector<Root> roots;
    fs::path tags_dir, cache_dir;
    /// where relative sidecar "_cover" paths point (empty: only absolute ones work)
    fs::path covers_dir;
    size_t workers = 2;
    /// also expose SACD multichannel areas ("MC NN - Title.dsf", album "... (Multichannel)")
    bool sacd_multichannel = false;
};

struct Entry {
    std::string name;
    bool is_dir = false;
    fs::path dir;   // source path (directories)
    VFilePtr file;  // contents (files)
    int64_t mtime = 0;
    /// where tag edits of this file are stored: sidecar section ("track" or
    /// "file") and key, and the file's tag format (flac, dsf, mp3, m4a); empty
    /// section = not editable
    std::string tag_section, tag_key, tag_ext;
};

struct Sig {
    std::optional<int64_t> dir_mtime, sidecar_mtime, overlay_mtime;
    uint64_t generation = 0;
    bool operator==(const Sig&) const = default;
};

struct Listing {
    std::vector<Entry> entries;
    int64_t mtime = 0;
    Sig sig;
    mutable std::mutex mu;
    mutable Clock::time_point checked;
    /// rebuild after this instant even if nothing changed (files still settling)
    std::optional<Clock::time_point> retry_at;
    /// cover images used, with their mtime then: the listing is rebuilt when one changes
    std::vector<std::pair<fs::path, std::optional<int64_t>>> covers;

    const Entry* find(std::string_view name) const;
};

/// An entry of a mount directory: a source folder's entry, possibly placed here
/// from elsewhere by a sidecar "_target".
struct VEntry {
    Entry e;           // directories: e.dir = the source folder behind it (empty if purely virtual)
    fs::path src_dir;  // files: the source folder they belong to (tag edits are stored there)
};

/// A directory of the mount: the source folders placed at its path (by default
/// the folder at the same path under a root) plus what "_target" moves here.
struct VListing {
    std::vector<VEntry> entries;
    int64_t mtime = 0;
    Clock::time_point built;
    uint64_t version = 0;  // placement index it was built from
    const VEntry* find(std::string_view name) const;
};

/// Where sidecar "_target"s place things (read from the tags tree).
struct Placements {
    uint64_t version = 0;
    std::map<fs::path, std::string> folder;                  // source folder -> its directory in the mount
    std::multimap<std::string, fs::path> folder_by_target;   // and back
    struct EntryTargets {
        std::map<std::string, std::string> tracks, files;    // canonical track key / file name -> directory
        bool operator==(const EntryTargets&) const = default;
    };
    std::map<fs::path, EntryTargets> entries;                // source folder -> its files placed elsewhere
    std::map<std::string, std::set<fs::path>> entries_by_target;
    std::set<std::string> targets;                           // every target directory
    std::set<std::string> sources;                           // source folders with any placement (as strings)
};

/// A processed image: a FLAC image (split by copying frames) or a decoded one
/// (tracks encoded on the fly), plus the source's own tags.
struct ReadyImage {
    std::shared_ptr<const FlacImage> flac;
    std::shared_ptr<const AvImage> av;
    Tags source_tags;
    std::vector<flac::HeaderBlock> pictures;  // referenced in the image file, not held
};

template <class T>
struct Work {
    enum State { Pending, Settling, Ready, Failed } st = Pending;
    Clock::time_point until{};  // Settling: retry after this
    std::shared_ptr<const T> ready;
    std::string err;
};

class Library : public std::enable_shared_from_this<Library> {
public:
    static std::shared_ptr<Library> create(Config cfg);

    const Config cfg;
    const Cache cache;

    /// Start background workers. Without workers, processing happens inline (CLI tools).
    void start_workers();
    /// Walk all roots periodically so work is queued before anyone asks.
    void start_prescan(std::chrono::seconds every);
    /// Entries for a source directory (cached; rebuilt when anything relevant changes).
    std::shared_ptr<const Listing> list_dir(const fs::path& dir);
    /// A directory of the mount by its path ("" = the mount root, "Music/Artist").
    std::shared_ptr<const VListing> list_vdir(const std::string& vpath);
    /// Re-read "_target"s from the tags tree (rate-limited unless forced).
    void refresh_placements(bool force = false);
    /// Root this source path belongs to and the path relative to it.
    std::optional<std::pair<const Root*, fs::path>> root_of(const fs::path& p) const;
    std::optional<fs::path> overlay_dir(const fs::path& dir) const;
    /// Verify and index an image.
    ReadyImage process_image(const fs::path& src) const;
    /// Human-readable summary of processing state, written next to the cache dir.
    void write_status() const;
    /// Store tag changes made to file `file_name` of source folder `dir` in the
    /// folder's sidecar, under `section`/`key`. Existing spellings are kept.
    void apply_edit(const fs::path& dir, const std::string& file_name, const std::string& section, const std::string& key, const Tags& changes);

private:
    explicit Library(Config cfg);
    struct ImageJob {
        fs::path src, dir;
    };
    struct SacdJob {
        fs::path src, dir;
        bool mc;
    };
    struct Md5Job {
        std::shared_ptr<const FlacImage> image;
        std::vector<std::pair<uint64_t, uint64_t>> ranges;
        fs::path dir;
    };
    struct EncodeJob {  // measure the encoded tracks of a decoded image
        std::shared_ptr<const AvImage> image;
        Ranges ranges;
        fs::path dir;
    };
    using Job = std::variant<ImageJob, SacdJob, Md5Job, EncodeJob>;

    void worker_loop();
    void run_job(Job job);
    bool enqueue(Job job);
    void bump(const fs::path& dir);
    void walk(const fs::path& dir, size_t& n);
    Work<ReadyImage> image_state(const fs::path& src, const fs::path& dir);
    Work<SacdDisc> sacd_state(const fs::path& src, const fs::path& dir, bool mc);
    Sig signature(const fs::path& dir) const;
    std::shared_ptr<Listing> build_listing(const fs::path& dir, const Sig& sig);
    /// nullopt while the tracks are still being measured
    /// Sidecar "_cover"s resolved while building one listing (loaded once each).
    struct CoverSet {
        const fs::path& covers_dir;
        std::map<std::string, std::optional<Cover>> loaded;
        std::vector<std::pair<fs::path, std::optional<int64_t>>> used;  // for Listing::covers
        const Cover* get(const std::string* spec);
    };
    std::optional<std::vector<Entry>> image_tracks(const fs::path& dir, const CueSheet& cue, const ReadyImage& ready, std::optional<uint32_t> disc, bool multi,
                                    size_t ndiscs, const Sidecar* sidecar, int64_t mtime, CoverSet& covers);
    /// A standalone APE/WavPack/... file as one FLAC track ("<stem>.flac", edits go to
    /// file."<stem>.flac"); nullopt while being measured.
    std::optional<Entry> standalone_track(const fs::path& dir, const std::string& src_name, const ReadyImage& ready, const Sidecar* sidecar, int64_t mt,
                                          CoverSet& covers);
    /// Encoded-track layouts of `ranges` of a decoded image; nullopt while being measured.
    std::optional<std::vector<TrackLayout>> track_layouts(const std::shared_ptr<const AvImage>& av, const Ranges& ranges, const fs::path& dir);
    std::vector<Entry> sacd_tracks(const std::shared_ptr<const SacdDisc>& disc, std::optional<uint32_t> disc_no, size_t ndiscs, const Sidecar* sidecar,
                                   int64_t mtime, bool mc, CoverSet& covers);

    std::optional<std::string> vpath_of(const Placements& p, const fs::path& dir) const;
    std::optional<std::string> entry_target(const Placements& p, const fs::path& dir, const Entry& e) const;
    std::shared_ptr<VListing> build_vlisting(const std::string& vpath, const std::shared_ptr<const Placements>& p);

    std::mutex place_mu_;  // guards the placement state below
    std::shared_ptr<const Placements> placements_ = std::make_shared<Placements>();
    struct SidecarTargets {  // just the "_target"s of a tags-tree sidecar
        std::optional<std::string> folder;
        std::map<std::string, std::string> tracks, files;
    };
    // tags-tree sidecars by source folder: mtime, and their targets (null: none)
    std::map<fs::path, std::pair<int64_t, std::shared_ptr<const SidecarTargets>>> placement_files_;
    size_t vlist_inserts_ = 0;
    std::optional<Clock::time_point> placements_checked_;
    std::map<std::string, int64_t> vbumps_;  // mount directories whose contents moved, and when
    std::map<std::string, std::shared_ptr<VListing>> vlistings_;

    std::mutex edit_mu_;     // serializes sidecar writes
    mutable std::mutex mu_;  // guards the maps below
    std::map<fs::path, std::pair<SrcKey, Work<ReadyImage>>> images_;
    std::map<std::pair<fs::path, bool>, std::pair<SrcKey, Work<SacdDisc>>> sacds_;
    std::map<fs::path, std::shared_ptr<Listing>> listings_;
    std::map<fs::path, std::pair<uint64_t, int64_t>> generations_;
    std::map<fs::path, std::shared_ptr<std::mutex>> dir_locks_;
    std::set<fs::path> md5_queued_, encode_queued_;

    std::mutex jobs_mu_;
    std::condition_variable jobs_cv_;
    std::deque<Job> jobs_;
    bool has_workers_ = false;
};

}  // namespace wm
