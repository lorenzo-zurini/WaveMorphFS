// SPDX-License-Identifier: GPL-3.0-or-later
// The library: turns a source directory into the list of entries the filesystem
// exposes, and runs background workers for the slow parts (indexing images,
// verifying non-FLAC images and measuring their tracks, scanning SACD ISOs,
// per-track MD5s). Audio is never copied: every read goes to the source file.
//
// Rules per source directory:
//  * X.cue + existing single-file image   -> virtual per-track FLAC files; cue and image hidden
//    (the image stays hidden while it is incomplete or being processed); floating-point
//    WavPack images, which FLAC cannot hold, give per-track WavPack files instead
//  * *.iso with an SACD master TOC         -> virtual per-track DSF files; ISO hidden
//  * other files                           -> passthrough (FLAC/MP3/M4A/DSF retagged if a sidecar applies;
//                                             APE/WV/TTA/TAK as encoded FLAC, floating-point WV retagged)
//  * partial files, dotfiles, sidecars     -> hidden
//  * cover image from the sidecar tree     -> exposed as cover.jpg/png if the folder has none
#pragma once

#include <atomic>
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
#include "wavpack.hpp"

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
    /// how long a folder listing is trusted before its sources are checked again
    /// (tags-tree placements: a third longer)
    std::chrono::milliseconds recheck{1500};
};

/// Sidecar sections that tag edits made on the mount are stored in.
inline constexpr std::string_view EDIT_TRACK = "track", EDIT_FILE = "file";

struct Entry {
    std::string name;
    bool is_dir = false;
    fs::path dir;   // source path (directories)
    VFilePtr file;  // contents (files)
    /// files: the mtime they report. Directories report the mtime of their own
    /// listing instead (Library::list_vdir); here it is the source folder's.
    int64_t mtime = 0;
    /// where tag edits of this file are stored: sidecar section (EDIT_TRACK or
    /// EDIT_FILE) and key, and the file's tag format (flac, dsf, mp3, m4a, wv);
    /// empty section = not editable
    std::string tag_section, tag_key, tag_ext;

    static Entry make_dir(std::string name, fs::path dir, int64_t mtime);
    /// A file that cannot be edited through the mount.
    static Entry make_file(std::string name, VFilePtr file, int64_t mtime);
    /// A file whose tag edits are stored under `section`/`key` of its folder's sidecar.
    static Entry make_tagged(std::string name, VFilePtr file, int64_t mtime, std::string_view section, std::string key, std::string ext);
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
    mutable std::mutex mu;  // guards `checked`
    /// when `sig` was last found current (re-checked at most every 1.5 s)
    mutable Clock::time_point checked;
    /// rebuild after this instant even if nothing changed (files still settling)
    std::optional<Clock::time_point> retry_at;
    /// cover images used, with their mtime then: the listing is rebuilt when one changes
    std::vector<std::pair<fs::path, std::optional<int64_t>>> covers;

    /// The entry called `name`. The pointer is only valid while the caller holds
    /// this listing: list_dir() may replace it at any moment.
    const Entry* find(std::string_view name) const;
};

/// An entry of a mount directory: a source folder's entry, possibly placed here
/// from elsewhere by a sidecar "_target".
struct VEntry {
    Entry e;           // directories: e.dir = the source folder behind it (empty if purely virtual)
    fs::path src_dir;  // files: the source folder they belong to (tag edits are stored there)
    /// files: the name in the source folder's listing, which the sidecar knows it
    /// by; e.name differs when a clash in the mount directory numbered it
    std::string src_name;
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
    bool loaded = false;  // false only for the empty set in place before the first scan
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

namespace detail {
// state of one Library::build_listing call, defined in library.cpp
struct DirItem;
struct ListingBuild;
struct DiscSlot;

/// Processing state of a source file (image or SACD area).
template <class T>
struct Work {
    enum State { Pending, Settling, Ready, Failed } st = Pending;
    Clock::time_point until{};  // Settling: retry after this
    std::shared_ptr<const T> ready;
    std::string err;
};
}  // namespace detail

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
    /// The entry `name` of mount directory `vpath`, copied. Another thread may
    /// replace the cached listing at any moment, so nothing may keep a pointer
    /// into a listing it no longer holds.
    std::optional<VEntry> lookup(const std::string& vpath, std::string_view name);
    /// Re-read "_target"s from the tags tree (rate-limited unless forced; an
    /// unforced call returns at once while another thread is scanning).
    void refresh_placements(bool force = false);
    /// Make the next lookups re-check source folder `dir` and rebuild mount
    /// directories now, instead of trusting listings checked moments ago (after
    /// a tag edit, so the editor reads back what it wrote).
    void invalidate(const fs::path& dir);
    /// Root this source path belongs to and the path relative to it.
    std::optional<std::pair<const Root*, fs::path>> root_of(const fs::path& p) const;
    std::optional<fs::path> overlay_dir(const fs::path& dir) const;
    /// Store tag changes made to file `file_name` of source folder `dir` in the
    /// folder's sidecar, under `section`/`key`. Existing spellings are kept.
    void apply_edit(const fs::path& dir, const std::string& file_name, const std::string& section, const std::string& key, const Tags& changes);

private:
    explicit Library(Config cfg);

    /// A processed image: a FLAC image (split by copying frames), a floating-point
    /// WavPack image (split by copying blocks) or a decoded one (tracks encoded on
    /// the fly), plus the source's own tags.
    struct ReadyImage {
        std::shared_ptr<const FlacImage> flac;
        std::shared_ptr<const WvImage> wv;
        std::shared_ptr<const AvImage> av;
        Tags source_tags;
        std::vector<flac::HeaderBlock> pictures;  // FLAC images: the front cover, referenced in the image file
    };
    template <class T>
    using Work = detail::Work<T>;
    template <class K, class T>
    using WorkMap = std::map<K, std::pair<SrcKey, Work<T>>>;

    struct ImageJob {
        fs::path src, dir;
    };
    struct SacdJob {
        fs::path src, dir;
        bool mc;
    };
    struct Md5Job {
        std::shared_ptr<const FlacImage> image;
        SrcKey key;  // the version of the image the listing saw
        Ranges ranges;
        fs::path dir;
    };
    struct EncodeJob {  // measure the tracks of an image whose parts are encoded on the fly
        fs::path src;
        std::function<void()> measure;  // builds and caches the layout
        fs::path dir;
    };
    /// A background step that failed for a version of a source file: not retried
    /// until the file changes (each try reads the whole image).
    struct JobFailure {
        SrcKey key;
        std::string err;
    };
    using Job = std::variant<ImageJob, SacdJob, Md5Job, EncodeJob>;

    void worker_loop();
    void run_job(Job job);
    bool enqueue(Job job);
    void bump(const fs::path& dir);
    /// Prescan: list `dir` and its subfolders; `seen` holds the folders (device,
    /// inode) already walked.
    void walk(const fs::path& dir, size_t& n, std::set<std::pair<uint64_t, uint64_t>>& seen);
    /// Forget state of source files and folders that no longer exist.
    void prune();
    /// Store the outcome of processing version `key` of a file, unless a newer
    /// version has been seen meanwhile (its own state must stay).
    template <class K, class T>
    void settle_work(WorkMap<K, T>& states, const K& k, const SrcKey& key, Work<T> st);
    /// The error recorded in `failures` for the current version of `src`, if any.
    std::optional<std::string> failure_of(const std::map<fs::path, JobFailure>& failures, const fs::path& src) const;
    /// Verify and index an image.
    ReadyImage process_image(const fs::path& src) const;
    /// Human-readable summary of processing state, written next to the cache dir.
    void write_status() const;

    /// Processing state of `src` (entry `k` of `states`): known, else settling while
    /// the file is still being written, else processed by `job` in the background,
    /// or by `open` inline when there are no workers or `cached()` says it is cheap.
    template <class K, class T, class Cached, class Open>
    Work<T> work_state(WorkMap<K, T>& states, const K& k, const fs::path& src, Cached cached, Open open, Job job);
    Work<ReadyImage> image_state(const fs::path& src, const fs::path& dir);
    Work<SacdDisc> sacd_state(const fs::path& src, const fs::path& dir, bool mc);

    Sig signature(const fs::path& dir) const;
    /// The sidecars of `dir` that parse, merged; `broken` if one does not.
    std::optional<Sidecar> load_sidecar(const fs::path& dir, const std::optional<fs::path>& overlay, bool& broken);
    /// A new listing of `dir`; `previous` (if any) is kept while a sidecar is broken.
    std::shared_ptr<Listing> build_listing(const fs::path& dir, const Sig& sig, const std::shared_ptr<const Listing>& previous);
    void add_image_groups(detail::ListingBuild& b);
    void add_sacd_isos(detail::ListingBuild& b);
    void add_source_file(detail::ListingBuild& b, const detail::DirItem& n);
    /// A standalone APE/WavPack/...: true if it was dealt with (listed in its
    /// served form, or hidden while processing); false to show it as it is.
    bool add_standalone(detail::ListingBuild& b, const detail::DirItem& n);
    /// Tracks of an image; nullopt while the tracks are still being measured.
    std::optional<std::vector<Entry>> image_tracks(detail::ListingBuild& b, const CueSheet& cue, const ReadyImage& ready, const detail::DiscSlot& disc,
                                                   int64_t mtime);
    /// A standalone APE/WavPack/... file as one FLAC track ("<stem>.flac", edits go to
    /// file."<stem>.flac"); nullopt while being measured.
    std::optional<Entry> standalone_track(detail::ListingBuild& b, const std::string& src_name, const ReadyImage& ready, int64_t mt);
    std::vector<Entry> sacd_tracks(detail::ListingBuild& b, const std::shared_ptr<const SacdDisc>& sacd, const detail::DiscSlot& disc, int64_t mtime,
                                   bool mc);
    /// Encoded-track layouts of `ranges` of a decoded image; nullopt while being measured.
    /// Layout `L` of an image's tracks (sizes of what is encoded on the fly): from
    /// the cache, or measured by a queued job (nullopt meanwhile) or right here.
    /// Throws if measuring this version of the image failed before.
    std::optional<std::vector<TrackLayout>> track_layouts(const std::shared_ptr<const AvImage>& av, const Ranges& ranges, const fs::path& dir);
    template <class L>
    std::optional<L> measured(const fs::path& src, const fs::path& dir, const std::function<std::optional<L>()>& load, std::function<L()> build);
    /// Newest of the cache files holding processing results the tracks of `ready`
    /// are made from (indexes, layouts, MD5s).
    int64_t results_mtime(const ReadyImage& ready, const Ranges& ranges) const;

    std::optional<std::string> vpath_of(const Placements& p, const fs::path& dir) const;
    std::optional<std::string> entry_target(const Placements& p, const fs::path& dir, const Entry& e) const;
    std::shared_ptr<VListing> build_vlisting(const std::string& vpath, const std::shared_ptr<const Placements>& p);

    std::mutex placement_scan_mu_;  // one scan of the tags tree at a time
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
    WorkMap<fs::path, ReadyImage> images_;
    WorkMap<std::pair<fs::path, bool>, SacdDisc> sacds_;
    std::map<fs::path, std::shared_ptr<Listing>> listings_;
    std::map<fs::path, std::pair<uint64_t, int64_t>> generations_;  // per source folder: generation, time of the last bump
    std::map<fs::path, std::shared_ptr<std::mutex>> dir_locks_;
    std::set<fs::path> md5_queued_, encode_queued_;
    std::map<fs::path, JobFailure> md5_failed_, encode_failed_;
    std::map<fs::path, std::string> sidecar_errors_;  // source folder -> why its sidecars do not parse

    std::mutex jobs_mu_;
    std::condition_variable jobs_cv_;
    std::deque<Job> jobs_;
    bool has_workers_ = false;
    mutable std::atomic<Clock::time_point> status_written_{};  // when status.txt was last written
};

}  // namespace wm
