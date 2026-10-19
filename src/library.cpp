// SPDX-License-Identifier: GPL-3.0-or-later
#include "library.hpp"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstring>
#include <thread>

#include <nlohmann/json.hpp>

#include "retag.hpp"

namespace wm {

namespace {

/// Generated and retagged files are never older than this: bump it whenever a
/// change alters what they contain (tag rendering, encoding...), so that music
/// servers re-read them once. 2026-10-01 13:00 UTC: C++ rewrite (tag synonyms,
/// SACD genres, kept tag spelling, APE served as encoded FLAC).
/// 2026-10-01 16:00 UTC: padding in generated tag areas (editable mount).
/// 2026-10-19 19:00 UTC (0.2.0): ID3v2.4 tags written without unsynchronisation;
/// one tag-name table for ID3/MP4/APEv2 output; covers of APE/WV images and
/// variable-blocksize FLAC images in their tracks; SACD catalog numbers; short
/// FLAC tracks without undersized blocks.
constexpr int64_t OUTPUT_EPOCH_NS = 1792436813LL * 1'000'000'000;

const std::vector<std::string_view> AUDIO_IMAGE_EXT = {"flac", "ape", "wv", "tta", "tak", "wav", "m4a", "aiff", "aif"};
/// A file modified more recently than this is assumed to still be written.
constexpr auto SETTLE = std::chrono::seconds(60);
/// How long a listing is served without checking whether anything changed.
/// FLAC/ID3 picture type of the front cover
constexpr uint32_t PICTURE_FRONT_COVER = 3;

bool is_ignored(const std::string& name) {
    std::string l = lower(name);
    // .!qB/.parts: suffixes of files that are still being written
    return starts_with(name, ".") || ends_with(l, ".!qb") || ends_with(l, ".parts") || l == SIDECAR_NAME;
}

bool is_cover_name(const std::string& name) {
    std::string l = lower(name);
    auto dot = l.rfind('.');
    std::string stem = dot == std::string::npos ? l : l.substr(0, dot);
    return (stem == "cover" || stem == "folder" || stem == "front" || stem == "albumart" || stem == "album") &&
           (ends_with(l, ".jpg") || ends_with(l, ".jpeg") || ends_with(l, ".png") || ends_with(l, ".webp"));
}

std::optional<int64_t> newest_mtime(const fs::path& dir) {
    auto best = mtime_ns(dir);
    if (!best) return std::nullopt;
    DIR* d = ::opendir(dir.c_str());
    if (!d) return best;
    while (auto* e = ::readdir(d)) {
        struct stat st;
        if (::stat((dir / e->d_name).c_str(), &st) == 0 && S_ISREG(st.st_mode)) best = std::max(*best, ns_of(st.st_mtim));
    }
    ::closedir(d);
    return best;
}

/// For a file modified less than SETTLE ago (`mtime`), when it will have settled.
std::optional<Clock::time_point> settles_at(int64_t mtime) {
    auto age = std::chrono::nanoseconds(now_ns() - mtime);
    if (age < std::chrono::nanoseconds(0) || age >= SETTLE) return std::nullopt;  // a future mtime counts as settled
    return Clock::now() + (SETTLE - age);
}

std::string stem_of(const std::string& name) {
    auto dot = name.rfind('.');
    return dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
}

bool is_audio_name(const std::string& n) {
    auto e = ext_lower(n);
    return std::find(AUDIO_IMAGE_EXT.begin(), AUDIO_IMAGE_EXT.end(), e) != AUDIO_IMAGE_EXT.end();
}

/// The file name a cue sheet's FILE line refers to, without directories.
std::string cue_file_name(const CueSheet& cue) {
    std::string ref = cue.files[0];
    std::replace(ref.begin(), ref.end(), '\\', '/');
    return fs::path(ref).filename().string();
}

std::optional<uint32_t> picture_type(const Bytes& data) {
    if (data.size() < 4) return std::nullopt;
    return be32(data.data());
}

/// The first frame says how the stream is blocked (FrameIndex::build rejects
/// other first frames with a clear error).
bool variable_blocksize(const fs::path& p, const flac::FlacMeta& meta) {
    uint8_t h[16] = {};
    File(p).read_at(h, sizeof h, meta.audio_start);
    auto fh = flac::FrameHeader::parse(h);
    return fh && fh->variable;
}

int64_t max_mtime(std::initializer_list<std::optional<int64_t>> v) {
    int64_t m = 0;
    for (auto& x : v)
        if (x) m = std::max(m, *x);
    return m;
}

/// Section of status.txt a processing state is listed in.
template <class T>
int status_section(const detail::Work<T>& w) {
    switch (w.st) {
    case detail::Work<T>::Ready: return 0;
    case detail::Work<T>::Pending: return 1;
    case detail::Work<T>::Settling: return 2;
    case detail::Work<T>::Failed: break;
    }
    return 3;
}

/// Run `open`, turning its result or failure into a processing state.
template <class T, class Open>
detail::Work<T> attempt(Open&& open) {
    detail::Work<T> st;
    try {
        st.ready = open();
        st.st = detail::Work<T>::Ready;
    } catch (const std::exception& e) {
        st.st = detail::Work<T>::Failed;
        st.err = e.what();
    }
    return st;
}

}  // namespace

namespace detail {

struct DirItem {
    std::string name;
    bool is_dir = false, is_file = false;
    uint64_t size = 0;
    int64_t mtime = 0;
};

/// Sidecar "_cover"s resolved while building one listing (loaded once each).
struct CoverSet {
    const fs::path& covers_dir;
    const fs::path& cache_dir;  // scaled-down copies of large images
    std::map<std::string, std::optional<Cover>> loaded;
    std::vector<std::pair<fs::path, std::optional<int64_t>>> used;  // for Listing::covers

    const Cover* get(const std::string* spec) {
        if (!spec) return nullptr;
        auto it = loaded.find(*spec);
        if (it == loaded.end()) {
            fs::path p = *spec;
            if (p.is_relative()) {
                if (covers_dir.empty()) {
                    // once per process, not on every rebuild of every folder using it
                    static std::mutex mu;
                    static std::set<std::string> warned;
                    std::lock_guard g(mu);
                    if (warned.insert(*spec).second) warn("cover {}: a relative path needs --covers-dir", *spec);
                    loaded.emplace(*spec, std::nullopt);
                    return nullptr;
                }
                p = covers_dir / p;
            }
            used.emplace_back(p, mtime_ns(p));
            it = loaded.emplace(*spec, prepare_cover(p, cache_dir)).first;  // a missing or bad image keeps the original pictures
        }
        return it->second ? &*it->second : nullptr;
    }
};

/// What one build_listing call works with and produces.
struct ListingBuild {
    fs::path dir;
    std::vector<DirItem> names;
    std::set<std::string> name_set;
    std::optional<fs::path> overlay;
    std::optional<Sidecar> sidecar;
    const Sidecar* sc = nullptr;  // &*sidecar, or null
    /// newest sidecar or tags-tree change: files whose tags come from sidecars change with it
    std::optional<int64_t> edits_mtime;
    CoverSet covers;
    std::set<std::string> hidden;  // source files not listed as they are
    std::vector<Entry> entries;    // listed under their own names
    std::vector<Entry> virtuals;   // generated; renamed when a real file has their name
    bool has_cover = false;        // the folder has a cover image of its own
    std::optional<Clock::time_point> retry_at;

    ListingBuild(fs::path d, const Config& cfg) : dir(std::move(d)), covers{cfg.covers_dir, cfg.cache_dir, {}, {}} {}
    /// `n` is still being downloaded (qBittorrent writes it as "<n>.!qB")
    bool incomplete(const std::string& n) const { return name_set.contains(n + ".!qB") || name_set.contains(n + ".!qb"); }
    void settle_until(Clock::time_point until) { retry_at = retry_at ? std::min(*retry_at, until) : until; }
};

/// The disc tracks belong to. With several discs in one folder the disc number is
/// part of the track names and sidecar keys ("2-05 - Title.flac", track."2-05").
struct DiscSlot {
    std::optional<uint32_t> number;  // DISCNUMBER
    bool several = false;            // the folder holds several discs
    size_t count = 1;                // DISCTOTAL, when several
    /// the disc as track names and sidecar keys use it
    std::optional<uint32_t> keyed() const { return several ? number : std::nullopt; }
};

}  // namespace detail

using detail::CoverSet;
using detail::DirItem;
using detail::DiscSlot;
using detail::ListingBuild;

namespace {

std::vector<DirItem> read_dir(const fs::path& dir) {
    DIR* d = ::opendir(dir.c_str());
    if (!d) fail("read_dir {}: {}", dir.string(), std::strerror(errno));
    std::vector<DirItem> out;
    while (auto* e = ::readdir(d)) {
        std::string n = e->d_name;
        if (n == "." || n == "..") continue;
        struct stat st;
        if (::stat((dir / n).c_str(), &st) != 0) continue;  // follows symlinks, like the source tree should
        out.push_back({n, S_ISDIR(st.st_mode), S_ISREG(st.st_mode), uint64_t(st.st_size), ns_of(st.st_mtim)});
    }
    ::closedir(d);
    std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.name < b.name; });
    return out;
}

/// Find the audio file a single-FILE cue refers to. Rips are often converted after
/// the cue was written (cue says .wav, file is .flac), so fall back to matching by
/// stem, then by the cue's own stem.
std::optional<std::string> resolve_image(const CueSheet& cue, const std::string& cue_name, const std::vector<DirItem>& names) {
    std::string ref = cue_file_name(cue);
    if (ref.empty()) return std::nullopt;
    std::vector<const std::string*> files;
    for (auto& n : names)
        if (n.is_file) files.push_back(&n.name);
    if (is_audio_name(ref)) {
        for (auto* n : files)
            if (*n == ref) return *n;
        // case-insensitive exact
        for (auto* n : files)
            if (iequals(*n, ref)) return *n;
    }
    for (auto& want : {stem_of(ref), stem_of(cue_name)})
        for (auto* n : files)
            if (is_audio_name(*n) && stem_of(*n) == want) return *n;
    return std::nullopt;
}

struct ImageGroup {
    std::string cue_name;
    CueSheet cue;
    std::string img_name;
};

/// The folder's cue sheets with the image each describes, minus sheets hidden by
/// the sidecar and duplicates: several sheets describing the same image with the
/// same tracks (e.g. "X.ape.cue" and "X.wav.cue") list it once, from the sheet
/// whose FILE names the image itself, else the first by name. Both happen before
/// discs are numbered.
std::vector<ImageGroup> image_groups(ListingBuild& b) {
    std::vector<ImageGroup> groups;
    for (auto& n : b.names) {
        if (!n.is_file || ext_lower(n.name) != "cue") continue;
        CueSheet cue;
        try {
            cue = CueSheet::read(b.dir / n.name);
        } catch (const std::exception& e) {
            warn("cue {}: {}", (b.dir / n.name).string(), e.what());
            continue;
        }
        if (!cue.is_image()) continue;
        if (auto img = resolve_image(cue, n.name, b.names)) groups.push_back({n.name, std::move(cue), *img});
        else if (b.incomplete(cue_file_name(cue))) b.hidden.insert(n.name);  // image still being written as <name>.!qB
    }
    std::erase_if(groups, [&](const ImageGroup& g) {
        if (!b.sc || !(b.sc->hides(g.cue_name) || b.sc->hides(g.img_name))) return false;
        b.hidden.insert(g.cue_name);
        b.hidden.insert(g.img_name);
        return true;
    });
    auto same_tracks = [](const CueSheet& x, const CueSheet& y) {
        if (x.tracks.size() != y.tracks.size()) return false;
        for (size_t i = 0; i < x.tracks.size(); i++)
            if (x.tracks[i].number != y.tracks[i].number || x.tracks[i].index01 != y.tracks[i].index01) return false;
        return true;
    };
    auto names_image = [](const ImageGroup& g) { return cue_file_name(g.cue) == g.img_name; };
    std::set<size_t> dropped;
    for (size_t i = 0; i < groups.size(); i++)
        for (size_t j = i + 1; j < groups.size(); j++) {
            if (dropped.contains(i) || dropped.contains(j)) continue;
            if (groups[i].img_name != groups[j].img_name || !same_tracks(groups[i].cue, groups[j].cue)) continue;
            bool ni = names_image(groups[i]), nj = names_image(groups[j]);
            bool keep_j = (nj && !ni) || (nj == ni && groups[j].cue_name < groups[i].cue_name);
            dropped.insert(keep_j ? i : j);
        }
    for (auto it = dropped.rbegin(); it != dropped.rend(); ++it) {
        b.hidden.insert(groups[*it].cue_name);
        groups.erase(groups.begin() + ptrdiff_t(*it));
    }
    return groups;
}

/// TRACKNUMBER/TRACKTOTAL, and the disc fields when there is a disc number.
void set_position(Tags& tg, const DiscSlot& disc, uint32_t num, size_t total) {
    tg.set("TRACKNUMBER", std::to_string(num));
    tg.set("TRACKTOTAL", std::to_string(total));
    if (disc.number) {
        tg.set("DISCNUMBER", std::to_string(*disc.number));
        if (disc.several) tg.set("DISCTOTAL", std::to_string(disc.count));
    }
}

/// The sidecar's album table, then the track's own table.
void overlay_sidecar_track(Tags& tg, const Sidecar* sc, const DiscSlot& disc, uint32_t num) {
    if (!sc) return;
    tg.overlay(sc->album);
    if (auto t = sc->track(disc.keyed(), num)) tg.overlay(*t);
}

struct NamedTrack {
    std::string name, key;
    const Cover* cover;
};

/// What every split track derives the same way once its own tags are complete:
/// the file name ("[prefix][D-]NN - Title.ext", unless the sidecar pins one and
/// `pin` allows it), the tags of the sidecar's file table for that name, the
/// track's sidecar key and its cover.
NamedTrack name_track(ListingBuild& b, Tags& tg, const DiscSlot& disc, uint32_t num, std::string_view prefix, std::string_view ext, bool pin) {
    auto title_v = tg.get("TITLE");
    std::string title = title_v ? sanitize_name(*title_v) : std::format("Track {:02}", num);
    auto d = disc.keyed();
    std::string name = d ? std::format("{}{}-{:02} - {}.{}", prefix, *d, num, title, ext) : std::format("{}{:02} - {}.{}", prefix, num, title, ext);
    if (b.sc && pin)
        if (auto pinned = b.sc->track_name(d, num)) name = *pinned;
    if (b.sc)
        if (auto ft = b.sc->file(name)) tg.overlay(*ft);
    std::string key = track_key(d, num);
    const Cover* cv = b.sc ? b.covers.get(b.sc->cover_for(name, &key)) : nullptr;
    return {std::move(name), std::move(key), cv};
}

/// mtime of generated content: never older than OUTPUT_EPOCH_NS (changes of the
/// output format), than what it is made from, or than the cover embedded in it
int64_t output_mtime(int64_t mt, const Cover* cover) { return std::max({mt, OUTPUT_EPOCH_NS, cover ? cover->mtime : 0}); }

/// Add the generated entries, numbering names that are taken ("X (2).flac"):
/// real files keep theirs, then generated ones in the order they were made.
void merge_virtuals(ListingBuild& b) {
    std::set<std::string> taken;
    for (auto& e : b.entries) taken.insert(e.name);
    for (auto& v : b.virtuals) {
        if (taken.contains(v.name)) {
            auto dot = v.name.rfind('.');
            bool has_ext = dot != std::string::npos && dot > 0;
            std::string stem = has_ext ? v.name.substr(0, dot) : v.name, ext = has_ext ? v.name.substr(dot) : "";
            for (int i = 2; taken.contains(v.name); i++) v.name = std::format("{} ({}){}", stem, i, ext);
        }
        taken.insert(v.name);
        b.entries.push_back(std::move(v));
    }
    b.virtuals.clear();
}

/// Cover art exported into the sidecar tree, for folders of generated tracks
/// without a cover image of their own.
void add_overlay_cover(ListingBuild& b) {
    if (b.has_cover || b.virtuals.empty() || !b.overlay) return;
    for (auto c : {"cover.jpg", "cover.png"}) {
        fs::path p = *b.overlay / c;
        struct stat st;
        if (::stat(p.c_str(), &st) == 0) {
            b.entries.push_back(Entry::make_file(c, std::make_shared<Passthrough>(p, uint64_t(st.st_size)), ns_of(st.st_mtim)));
            return;
        }
    }
}

}  // namespace

Entry Entry::make_dir(std::string name, fs::path dir, int64_t mtime) {
    Entry e;
    e.name = std::move(name);
    e.is_dir = true;
    e.dir = std::move(dir);
    e.mtime = mtime;
    return e;
}

Entry Entry::make_file(std::string name, VFilePtr file, int64_t mtime) {
    Entry e;
    e.name = std::move(name);
    e.file = std::move(file);
    e.mtime = mtime;
    return e;
}

Entry Entry::make_tagged(std::string name, VFilePtr file, int64_t mtime, std::string_view section, std::string key, std::string ext) {
    Entry e = make_file(std::move(name), std::move(file), mtime);
    e.tag_section = section;
    e.tag_key = std::move(key);
    e.tag_ext = std::move(ext);
    return e;
}

const Entry* Listing::find(std::string_view name) const {
    for (auto& e : entries)
        if (e.name == name) return &e;
    return nullptr;
}

Library::Library(Config c) : cfg(std::move(c)), cache(cfg.cache_dir) { fs::create_directories(cfg.tags_dir); }

std::shared_ptr<Library> Library::create(Config cfg) { return std::shared_ptr<Library>(new Library(std::move(cfg))); }

void Library::start_workers() {
    has_workers_ = true;
    for (size_t i = 0; i < std::max<size_t>(cfg.workers, 1); i++) std::thread([self = shared_from_this()] { self->worker_loop(); }).detach();
}

void Library::worker_loop() {
    while (true) {
        Job job;
        {
            std::unique_lock g(jobs_mu_);
            jobs_cv_.wait(g, [&] { return !jobs_.empty(); });
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        try {
            run_job(std::move(job));
        } catch (const std::exception& e) {
            warn("worker: {}", e.what());
        }
        // status.txt lists every source file: rewrite it at most once a second
        // while jobs keep finishing, and when the queue runs empty
        bool idle;
        {
            std::lock_guard g(jobs_mu_);
            idle = jobs_.empty();
        }
        if (idle || Clock::now() - status_written_.load() >= std::chrono::seconds(1)) write_status();
    }
}

void Library::start_prescan(std::chrono::seconds every) {
    std::thread([self = shared_from_this(), every] {
        while (true) {
            auto t = Clock::now();
            size_t n = 0;
            std::set<std::pair<uint64_t, uint64_t>> seen;
            for (auto& r : self->cfg.roots) self->walk(r.path, n, seen);
            info("prescan: {} directories in {:.1f}s", n, std::chrono::duration<double>(Clock::now() - t).count());
            self->prune();
            trim_heap();  // building listings reads (and frees) every image's metadata and pictures
            self->write_status();
            std::this_thread::sleep_for(every);
        }
    }).detach();
}

void Library::walk(const fs::path& dir, size_t& n, std::set<std::pair<uint64_t, uint64_t>>& seen) {
    // source folders are reached through symlinks too: visit each once, or a
    // link to an ancestor recurses until paths get too long
    struct stat st;
    if (::stat(dir.c_str(), &st) != 0 || !seen.insert({uint64_t(st.st_dev), uint64_t(st.st_ino)}).second) return;
    n++;
    std::shared_ptr<const Listing> l;
    try {
        l = list_dir(dir);
    } catch (const std::exception& e) {
        debug("prescan {}: {}", dir.string(), e.what());
        return;
    }
    for (auto& e : l->entries)
        if (e.is_dir) walk(e.dir, n, seen);
}

template <class K, class T>
void Library::settle_work(WorkMap<K, T>& states, const K& k, const SrcKey& key, Work<T> st) {
    std::lock_guard g(mu_);
    if (auto it = states.find(k); it != states.end() && it->second.first != key) return;
    states[k] = {key, std::move(st)};
}

std::optional<std::string> Library::failure_of(const std::map<fs::path, JobFailure>& failures, const fs::path& src) const {
    auto key = SrcKey::try_of(src);
    std::lock_guard g(mu_);
    if (auto it = failures.find(src); it != failures.end() && key == it->second.key) return it->second.err;
    return std::nullopt;
}

void Library::run_job(Job job) {
    if (auto* j = std::get_if<ImageJob>(&job)) {
        auto key = SrcKey::try_of(j->src);
        auto st = attempt<ReadyImage>([&] { return std::make_shared<const ReadyImage>(process_image(j->src)); });
        if (st.st == Work<ReadyImage>::Ready) info("image ready: {}", j->src.string());
        else warn("image failed: {}: {}", j->src.string(), st.err);
        if (key) settle_work(images_, j->src, *key, std::move(st));
        bump(j->dir);
    } else if (auto* j = std::get_if<Md5Job>(&job)) {
        const fs::path& path = j->image->path;
        // the frames are read through the path: the image must still be the
        // version the listing saw, before and after, or the MD5s describe another
        auto unchanged = [&] { return SrcKey::try_of(path) == j->key; };
        try {
            if (unchanged()) {
                auto d = track_md5s(*j->image, j->ranges);
                Md5Map m;
                for (size_t i = 0; i < d.size(); i++) m[j->ranges[i]] = d[i];
                if (unchanged()) {
                    cache.store_md5s(path, j->key, m);
                    info("track MD5s ready: {}", path.string());
                }
            }
        } catch (const std::exception& e) {
            warn("MD5 of {}: {}", path.string(), e.what());
            std::lock_guard g(mu_);
            md5_failed_[path] = {j->key, e.what()};
        }
        {
            std::lock_guard g(mu_);
            md5_queued_.erase(path);
        }
        bump(j->dir);
    } else if (auto* j = std::get_if<EncodeJob>(&job)) {
        const fs::path& path = j->src;
        auto key = SrcKey::try_of(path);
        try {
            auto t = Clock::now();
            j->measure();
            info("tracks measured: {} ({:.0f}s)", path.string(), std::chrono::duration<double>(Clock::now() - t).count());
        } catch (const std::exception& e) {
            warn("measuring tracks of {}: {}", path.string(), e.what());
            std::lock_guard g(mu_);
            if (key) encode_failed_[path] = {*key, e.what()};
        }
        {
            std::lock_guard g(mu_);
            encode_queued_.erase(path);
        }
        bump(j->dir);
    } else if (auto* j = std::get_if<SacdJob>(&job)) {
        auto key = SrcKey::try_of(j->src);
        const char* area = j->mc ? "multichannel" : "stereo";
        auto st = attempt<SacdDisc>([&] { return SacdDisc::open(j->src, &cache, j->mc); });
        if (st.st == Work<SacdDisc>::Ready)
            info("SACD {} area ready: {} ({} tracks, {} ch{})", area, j->src.string(), st.ready->tracks.size(), st.ready->channels, st.ready->dst ? ", DST" : "");
        else warn("SACD {} area failed: {}: {}", area, j->src.string(), st.err);
        if (key) settle_work(sacds_, std::pair{j->src, j->mc}, *key, std::move(st));
        bump(j->dir);
    }
}

void Library::prune() {
    std::set<fs::path> files, dirs;
    {
        std::lock_guard g(mu_);
        for (auto& [p, ks] : images_) files.insert(p);
        for (auto& [pk, ks] : sacds_) files.insert(pk.first);
        for (auto* failures : {&md5_failed_, &encode_failed_})
            for (auto& [p, f] : *failures) files.insert(p);
        for (auto& [d, l] : listings_) dirs.insert(d);
    }
    std::map<fs::path, std::optional<SrcKey>> keys;
    for (auto& f : files) keys[f] = SrcKey::try_of(f);
    std::set<fs::path> gone_dirs;
    for (auto& d : dirs) {
        std::error_code ec;
        if (!fs::is_directory(d, ec)) gone_dirs.insert(d);
    }
    std::lock_guard g(mu_);
    auto gone = [&](const fs::path& p) { return keys.contains(p) && !keys[p]; };
    std::erase_if(images_, [&](auto& kv) { return gone(kv.first); });
    std::erase_if(sacds_, [&](auto& kv) { return gone(kv.first.first); });
    // a failure is about one version of the file
    for (auto* failures : {&md5_failed_, &encode_failed_})
        std::erase_if(*failures, [&](auto& kv) { return keys.contains(kv.first) && keys[kv.first] != kv.second.key; });
    for (auto& d : gone_dirs) {
        listings_.erase(d);
        sidecar_errors_.erase(d);
        dir_locks_.erase(d);  // a list_dir() holding it keeps its own reference
        generations_.erase(d);
    }
}

Library::ReadyImage Library::process_image(const fs::path& src) const {
    ReadyImage r;
    std::string ext = ext_lower(src);
    if (ext == "wv" && WvImage::is_float(src)) {
        auto wv = WvImage::open(src, &cache);
        r.source_tags = wv->tags;
        r.wv = wv;
        return r;
    }
    if (ext != "flac") {
        auto av = AvImage::open(src, &cache);
        r.source_tags = av->tags;
        // the image's own front cover (an APEv2 "Cover Art (Front)" item) goes
        // into the tracks like a FLAC image's, referenced in the image file
        if (av->front_cover && av->front_cover->len <= MAX_COVER_BYTES)
            r.pictures.push_back({flac::BLOCK_PICTURE, flac::picture_body(av->front_cover_mime, *av->front_cover)});
        r.av = av;
        return r;
    }
    auto meta = flac::FlacMeta::read(src);
    r.source_tags = Tags::from_pairs(meta.vorbis_comments());
    // the front cover goes into the tracks, referenced in the image file
    for (auto* b : meta.pictures())
        if (b->data.size() <= MAX_COVER_BYTES && picture_type(b->data) == PICTURE_FRONT_COVER) {
            r.pictures.push_back({b->kind, SrcRange{b->offset, b->data.size()}});
            break;
        }
    if (variable_blocksize(src, meta)) {
        // frames can only be copied from fixed-blocksize images; others are
        // decoded and re-encoded on the fly like non-FLAC images
        r.av = AvImage::open(src, &cache);
        return r;
    }
    auto idx = cache.index_for(src, meta);
    r.flac = FlacImage::open(src, std::move(meta), std::move(idx));
    return r;
}

void Library::write_status() const {
    std::vector<std::string> sections[4];  // ready, pending, settling, failed
    {
        std::lock_guard g(mu_);
        for (auto& [p, ks] : images_) {
            auto& st = ks.second;
            std::string label = "image  " + p.string();
            if (st.st == Work<ReadyImage>::Failed) label += "\n         " + st.err;
            sections[status_section(st)].push_back(std::move(label));
        }
        for (auto& [pk, ks] : sacds_) {
            auto& st = ks.second;
            std::string label = std::string(pk.second ? "SACD/MC " : "SACD    ") + pk.first.string();
            if (st.st == Work<SacdDisc>::Ready)
                label += std::format("  ({} tracks, {} ch{})", st.ready->tracks.size(), st.ready->channels, st.ready->dst ? ", DST" : "");
            if (st.st == Work<SacdDisc>::Failed) label += "\n         " + st.err;
            sections[status_section(st)].push_back(std::move(label));
        }
        for (auto& [d, err] : sidecar_errors_) sections[3].push_back(std::format("sidecar {}\n         {}", d.string(), err));
        for (auto [what, failures] : {std::pair{"MD5s   ", &md5_failed_}, std::pair{"tracks ", &encode_failed_}})
            for (auto& [p, f] : *failures) sections[3].push_back(std::format("{}{}\n         {}", what, p.string(), f.err));
    }
    static const char* titles[4] = {"ready", "pending", "settling (still being written)", "FAILED"};
    std::string out = std::format("WaveMorphFS status (unix time {})\n\n", now_ns() / 1'000'000'000);
    for (int i = 0; i < 4; i++) {
        std::sort(sections[i].begin(), sections[i].end());
        out += std::format("== {}: {}\n", titles[i], sections[i].size());
        if (i != 0)
            for (auto& s : sections[i]) out += "   " + s + "\n";
        out += "\n";
    }
    status_written_ = Clock::now();
    fs::path parent = cfg.cache_dir.parent_path();
    try {
        atomic_write((parent.empty() ? cfg.cache_dir : parent) / "status.txt", out);
    } catch (...) {
    }
}

void Library::apply_edit(const fs::path& dir, const std::string& file_name, const std::string& section, const std::string& key, const Tags& changes) {
    using json = nlohmann::ordered_json;
    auto ov = overlay_dir(dir);
    WM_ENSURE(ov.has_value(), "{} is not inside a library root", dir.string());
    fs::path p = *ov / SIDECAR_NAME;
    std::lock_guard g(edit_mu_);
    json doc = json::object();
    if (auto text = try_read_text(p)) {
        doc = json::parse(*text, nullptr, true, /*ignore_comments=*/true);
        // the JSON library cannot write comments back
        if (!json::accept(*text)) warn("{}: comments are not kept when edits made through the mount are saved", p.string());
    }
    WM_ENSURE(doc.is_object(), "{} is not a JSON object", p.string());
    // the in-source sidecar is only read: source folders stay untouched
    std::optional<Sidecar> in_source;
    if (auto text = try_read_text(dir / SIDECAR_NAME)) {
        try {
            in_source = Sidecar::parse(*text);
        } catch (const std::exception& e) {
            warn("{}: {}", (dir / SIDECAR_NAME).string(), e.what());
        }
    }
    bool track = section == EDIT_TRACK;
    const Tags* src_entry = nullptr;  // the in-source table of this entry
    const Tags* src_file = nullptr;   // track edits: the in-source file table of its name
    if (in_source) {
        if (track) {
            if (auto it = in_source->tracks.find(key); it != in_source->tracks.end()) src_entry = &it->second;
            src_file = in_source->file(file_name);
        } else {
            src_entry = in_source->file(key);
        }
    }

    if (!doc.contains(section) || !doc[section].is_object()) doc[section] = json::object();
    // a track's table may already exist under another spelling of its key ("05",
    // "2-5"): edit that one (the last, if several: it wins when they are merged)
    std::vector<std::string> aliases;
    if (track)
        for (auto& [k, v] : doc[section].items())
            if (v.is_object() && canonical_track_key(k) == key) aliases.push_back(k);
    std::string table = aliases.empty() ? key : aliases.back();
    // (re-indexed on every use: inserting into `doc` may move its members)
    auto entry = [&]() -> json& { return doc[section][table]; };
    if (!entry().is_object()) entry() = json::object();
    // pin the file name: retitling must not move the file under the editor
    bool pinned = std::any_of(aliases.begin(), aliases.end(), [&](auto& a) { return doc[section][a].contains("_name"); });
    if (track && !pinned && !entry().contains("_name")) entry()["_name"] = file_name;

    auto in_json = [](const json& obj, const std::string& k) -> std::optional<std::string> {
        if (obj.is_object())
            for (auto& [ek, ev] : obj.items())
                if (same_field(ek, k)) return ek;
        return std::nullopt;
    };
    auto in_tags = [](const Tags* t, const std::string& k) -> std::optional<std::string> {
        if (t)
            for (auto& [ek, ev] : t->m)
                if (same_field(ek, k)) return ek;
        return std::nullopt;
    };
    auto erase_field = [](json& obj, const std::string& k, const std::string& keep) {
        std::vector<std::string> drop;
        for (auto& [ek, ev] : obj.items())
            if (ek != keep && same_field(ek, k)) drop.push_back(ek);
        for (auto& d : drop) obj.erase(d);
    };
    for (auto& [k, vals] : changes.m) {
        // keep the spelling already used for this entry, else for the album: in
        // this sidecar, then in the in-source one
        std::optional<std::string> name = in_json(entry(), k);
        for (auto& a : aliases)
            if (!name) name = in_json(doc[section][a], k);
        if (!name && doc.contains("album")) name = in_json(doc["album"], k);
        if (!name) name = in_tags(src_entry, k);
        if (!name && in_source) name = in_tags(&in_source->album, k);
        json value = vals.empty() ? json("") : vals.size() == 1 ? json(vals[0]) : json(vals);
        for (auto& a : aliases)
            if (a != table) erase_field(doc[section][a], k, "");
        erase_field(entry(), k, name.value_or(k));
        entry()[name.value_or(k)] = value;
        if (!track) continue;
        // the file table of this name is applied after the track table: an
        // in-source one setting the field would hide the edit, so this sidecar's
        // file table overrides it (this sidecar is read last); an entry there of
        // our own is dropped instead
        if (auto spelled = in_tags(src_file, k)) {
            if (!doc.contains("file") || !doc["file"].is_object()) doc["file"] = json::object();
            json& f = doc["file"][file_name];
            if (!f.is_object()) f = json::object();
            erase_field(f, k, *spelled);
            f[*spelled] = value;
        } else if (doc.contains("file") && doc["file"].contains(file_name) && doc["file"][file_name].is_object()) {
            erase_field(doc["file"][file_name], k, "");
        }
    }
    fs::create_directories(*ov);
    atomic_write(p, doc.dump(2, ' ', false, json::error_handler_t::replace) + "\n");
    bump(dir);
}

void Library::bump(const fs::path& dir) {
    std::lock_guard g(mu_);
    auto& e = generations_[dir];
    e.first++;
    e.second = now_ns();
}

bool Library::enqueue(Job job) {
    if (!has_workers_) return false;
    {
        std::lock_guard g(jobs_mu_);
        jobs_.push_back(std::move(job));
    }
    jobs_cv_.notify_one();
    return true;
}

template <class K, class T, class Cached, class Open>
Library::Work<T> Library::work_state(WorkMap<K, T>& states, const K& k, const fs::path& src, Cached cached, Open open, Job job) {
    auto key = SrcKey::try_of(src);
    if (!key) return {Work<T>::Failed, {}, nullptr, "stat failed"};
    {
        std::lock_guard g(mu_);
        if (auto it = states.find(k); it != states.end() && it->second.first == *key) {
            auto& st = it->second.second;
            if (st.st != Work<T>::Settling || Clock::now() < st.until) return st;
        }
    }
    if (auto until = settles_at(key->mtime_ns)) {
        Work<T> st{Work<T>::Settling, *until, nullptr, ""};
        std::lock_guard g(mu_);
        states[k] = {*key, st};
        return st;
    }
    // cached work is cheap to reopen: do it inline so albums never vanish from a
    // listing just because the process restarted
    bool inline_ok = cached();
    {
        std::lock_guard g(mu_);
        states[k] = {*key, Work<T>{}};
    }
    if (!inline_ok && enqueue(std::move(job))) return {};
    auto st = attempt<T>(open);
    settle_work(states, k, *key, st);
    return st;
}

Library::Work<Library::ReadyImage> Library::image_state(const fs::path& src, const fs::path& dir) {
    return work_state(
        images_, src, src, [&] { return cache.image_is_cached(src); }, [&] { return std::make_shared<const ReadyImage>(process_image(src)); },
        ImageJob{src, dir});
}

Library::Work<SacdDisc> Library::sacd_state(const fs::path& src, const fs::path& dir, bool mc) {
    return work_state(
        sacds_, std::pair{src, mc}, src, [&] { return sacd_is_cached(cache, src, mc); }, [&] { return SacdDisc::open(src, &cache, mc); },
        SacdJob{src, dir, mc});
}

std::optional<std::pair<const Root*, fs::path>> Library::root_of(const fs::path& p) const {
    for (auto& r : cfg.roots) {
        auto rel = p.lexically_relative(r.path);
        if (!rel.empty() && *rel.begin() != "..") return std::pair{&r, rel == "." ? fs::path() : rel};
    }
    return std::nullopt;
}

std::optional<fs::path> Library::overlay_dir(const fs::path& dir) const {
    auto r = root_of(dir);
    if (!r) return std::nullopt;
    fs::path o = cfg.tags_dir / r->first->name;
    if (!r->second.empty()) o /= r->second;
    return o;
}

Sig Library::signature(const fs::path& dir) const {
    Sig s;
    s.dir_mtime = mtime_ns(dir);
    s.sidecar_mtime = mtime_ns(dir / SIDECAR_NAME);
    if (auto ov = overlay_dir(dir)) s.overlay_mtime = newest_mtime(*ov);
    std::lock_guard g(mu_);
    if (auto it = generations_.find(dir); it != generations_.end()) s.generation = it->second.first;
    return s;
}

std::shared_ptr<const Listing> Library::list_dir(const fs::path& dir) {
    std::shared_ptr<Listing> cached;
    std::shared_ptr<std::mutex> lock;
    {
        std::lock_guard g(mu_);
        if (auto it = listings_.find(dir); it != listings_.end()) cached = it->second;
        auto& l = dir_locks_[dir];
        if (!l) l = std::make_shared<std::mutex>();
        lock = l;
    }
    if (cached) {
        std::lock_guard g(cached->mu);
        if (Clock::now() - cached->checked < cfg.recheck) return cached;
    }
    std::lock_guard dg(*lock);
    Sig sig = signature(dir);
    {
        std::lock_guard g(mu_);
        if (auto it = listings_.find(dir); it != listings_.end()) cached = it->second;
    }
    auto covers_unchanged = [](const Listing& l) {
        return std::all_of(l.covers.begin(), l.covers.end(), [](auto& c) { return mtime_ns(c.first) == c.second; });
    };
    if (cached && cached->sig == sig && !(cached->retry_at && Clock::now() >= *cached->retry_at) && covers_unchanged(*cached)) {
        std::lock_guard g(cached->mu);
        cached->checked = Clock::now();
        return cached;
    }
    auto l = build_listing(dir, sig, cached);
    std::lock_guard g(mu_);
    listings_[dir] = l;
    return l;
}

std::optional<Sidecar> Library::load_sidecar(const fs::path& dir, const std::optional<fs::path>& overlay, bool& broken) {
    std::optional<Sidecar> out;
    std::string errors;
    for (auto& p : Sidecar::paths(dir, overlay)) {
        auto m = mtime_ns(p);
        if (!m) continue;
        try {
            auto text = try_read_text(p);
            WM_ENSURE(text.has_value(), "cannot read it");
            Sidecar sc = Sidecar::parse(*text);
            if (!out) out.emplace();
            out->merge(sc, *m);
        } catch (const std::exception& e) {
            warn("sidecar {}: {}", p.string(), e.what());
            errors += std::format("{}{}: {}", errors.empty() ? "" : "\n         ", p.string(), e.what());
        }
    }
    broken = !errors.empty();
    std::lock_guard g(mu_);
    if (broken) sidecar_errors_[dir] = errors;
    else sidecar_errors_.erase(dir);
    return out;
}

std::shared_ptr<Listing> Library::build_listing(const fs::path& dir, const Sig& sig, const std::shared_ptr<const Listing>& previous) {
    ListingBuild b(dir, cfg);
    b.names = with_context("read_dir " + dir.string(), [&] { return read_dir(dir); });
    for (auto& n : b.names) b.name_set.insert(n.name);
    b.overlay = overlay_dir(dir);
    bool broken = false;
    b.sidecar = load_sidecar(dir, b.overlay, broken);
    if (broken && previous) {
        // a sidecar that does not parse (often one being edited) would take away
        // its pinned names, hidden files and tags: music servers would see the
        // album deleted and re-added. Keep serving what was there until it is fixed.
        auto l = std::make_shared<Listing>();
        l->entries = previous->entries;
        l->mtime = previous->mtime;
        l->sig = sig;
        l->checked = Clock::now();
        l->retry_at = previous->retry_at;
        l->covers = previous->covers;
        return l;
    }
    b.sc = b.sidecar ? &*b.sidecar : nullptr;
    // removing a sidecar (or cover.jpg) from the tags tree changes the overlay
    // folder's mtime, not any sidecar's: without it, mtimes would go back
    b.edits_mtime = b.sc ? max_mtime({b.sc->mtime, sig.overlay_mtime}) : sig.overlay_mtime;
    for (auto& n : b.names)
        if (n.is_file && !is_ignored(n.name))
            if (auto until = settles_at(n.mtime)) b.settle_until(*until);  // its size and contents are not final yet

    add_image_groups(b);
    add_sacd_isos(b);
    for (auto& n : b.names)
        if (!b.hidden.contains(n.name) && !is_ignored(n.name) && !(b.sc && !n.is_dir && b.sc->hides(n.name))) add_source_file(b, n);
    add_overlay_cover(b);
    merge_virtuals(b);
    if (b.sc) std::erase_if(b.entries, [&](const Entry& e) { return b.sc->hides(e.name); });
    std::stable_sort(b.entries.begin(), b.entries.end(), [](auto& x, auto& y) { return x.name < y.name; });

    std::optional<int64_t> gen_time;
    {
        std::lock_guard g(mu_);
        if (auto it = generations_.find(dir); it != generations_.end()) gen_time = it->second.second;
    }
    auto l = std::make_shared<Listing>();
    l->entries = std::move(b.entries);
    l->mtime = max_mtime({sig.dir_mtime, sig.sidecar_mtime, sig.overlay_mtime, gen_time});
    for (auto& e : l->entries)
        if (!e.is_dir) l->mtime = std::max(l->mtime, e.mtime);
    l->sig = sig;
    l->checked = Clock::now();
    l->retry_at = b.retry_at;
    l->covers = std::move(b.covers.used);
    return l;
}

/// CUE + image groups: split into tracks once processed, hidden until then.
void Library::add_image_groups(ListingBuild& b) {
    auto groups = image_groups(b);
    bool multi = groups.size() > 1;
    // discs are numbered by the sheets' DISCNUMBER, else by position; numbers
    // that repeat would give two tracks one name and one sidecar key
    std::vector<std::optional<uint32_t>> numbers;
    for (size_t gi = 0; gi < groups.size(); gi++) {
        std::optional<uint32_t> n;
        if (auto d = groups[gi].cue.fields.find("DISCNUMBER"); d != groups[gi].cue.fields.end())
            if (auto v = parse_u64(trim(split(d->second, '/')[0])); v && *v <= UINT32_MAX) n = uint32_t(*v);
        if (!n && multi) n = uint32_t(gi + 1);
        numbers.push_back(n);
    }
    if (multi && std::set(numbers.begin(), numbers.end()).size() < numbers.size()) {
        warn("{}: cue sheets repeat disc numbers; numbering the discs in name order", b.dir.string());
        for (size_t gi = 0; gi < groups.size(); gi++) numbers[gi] = uint32_t(gi + 1);
    }
    for (size_t gi = 0; gi < groups.size(); gi++) {
        auto& [cue_name, cue, img_name] = groups[gi];
        fs::path img_path = b.dir / img_name;
        if (b.incomplete(img_name)) {
            b.hidden.insert(cue_name);
            b.hidden.insert(img_name);
            continue;
        }
        auto st = image_state(img_path, b.dir);
        if (st.st == Work<ReadyImage>::Ready) {
            DiscSlot disc{numbers[gi], multi, groups.size()};
            try {
                auto v = image_tracks(b, cue, *st.ready, disc, max_mtime({mtime_ns(b.dir / cue_name), mtime_ns(img_path), b.edits_mtime}));
                b.hidden.insert(cue_name);
                b.hidden.insert(img_name);
                if (v)
                    for (auto& e : *v) b.virtuals.push_back(std::move(e));
            } catch (const std::exception& e) {
                warn("splitting {}: {}", img_path.string(), e.what());
            }
        } else if (st.st == Work<ReadyImage>::Pending || st.st == Work<ReadyImage>::Settling) {
            if (st.st == Work<ReadyImage>::Settling) b.settle_until(st.until);
            b.hidden.insert(cue_name);
            b.hidden.insert(img_name);
        }
        // Failed: leave cue + image visible as-is
    }
}

/// SACD ISOs: DSF tracks once scanned, hidden until then.
void Library::add_sacd_isos(ListingBuild& b) {
    // only SACDs count as discs: a DVD image next to one must not number it
    std::vector<const DirItem*> isos;
    for (auto& n : b.names)
        if (n.is_file && ext_lower(n.name) == "iso" && !(b.sc && b.sc->hides(n.name)) && !b.incomplete(n.name) && is_sacd(b.dir / n.name))
            isos.push_back(&n);
    bool multi = isos.size() > 1;
    for (size_t ii = 0; ii < isos.size(); ii++) {
        const std::string& n = isos[ii]->name;
        fs::path p = b.dir / n;
        DiscSlot disc{multi ? std::optional<uint32_t>(uint32_t(ii + 1)) : std::nullopt, multi, isos.size()};
        // the area's frame table lands when its scan finishes
        auto area_mtime = [&](bool mc) {
            int64_t mt = max_mtime({mtime_ns(p), b.edits_mtime});
            if (auto k = SrcKey::try_of(p)) mt = std::max(mt, mtime_ns(cache.sacd_frames_path(p, *k, mc)).value_or(0));
            return mt;
        };
        auto st = sacd_state(p, b.dir, false);
        if (st.st == Work<SacdDisc>::Ready) {
            try {
                auto tracks = sacd_tracks(b, st.ready, disc, area_mtime(false), false);
                // only discs with a separate multichannel area (one that has nothing
                // but multichannel serves it as its main area)
                if (cfg.sacd_multichannel && sacd_has_area(p, true))
                    if (auto mc = sacd_state(p, b.dir, true); mc.st == Work<SacdDisc>::Ready)
                        for (auto& e : sacd_tracks(b, mc.ready, disc, area_mtime(true), true)) tracks.push_back(std::move(e));
                for (auto& e : tracks) b.virtuals.push_back(std::move(e));
                b.hidden.insert(n);
            } catch (const std::exception& e) {
                warn("SACD tracks of {}: {}", p.string(), e.what());  // the ISO stays visible as it is
            }
        } else if (st.st == Work<SacdDisc>::Pending || st.st == Work<SacdDisc>::Settling) {
            if (st.st == Work<SacdDisc>::Settling) b.settle_until(st.until);
            b.hidden.insert(n);
        }
    }
}

/// A file or folder of the source folder that no image or ISO accounts for.
void Library::add_source_file(ListingBuild& b, const DirItem& n) {
    fs::path p = b.dir / n.name;
    if (n.is_dir) {
        b.entries.push_back(Entry::make_dir(n.name, p, n.mtime));
        return;
    }
    if (is_cover_name(n.name)) b.has_cover = true;
    std::string ext = ext_lower(n.name);
    if ((ext == "ape" || ext == "wv" || ext == "tta" || ext == "tak") && add_standalone(b, n)) return;
    bool taggable = ext == "flac" || ext == "mp3" || ext == "m4a" || ext == "dsf";
    VFilePtr vf;
    const Cover* cv = b.sc && taggable && ext != "m4a" ? b.covers.get(b.sc->cover_for(n.name, nullptr)) : nullptr;
    if (b.sc && taggable && (!b.sc->album.empty() || b.sc->file(n.name) || cv)) {
        Tags ov = b.sc->album;
        if (auto f = b.sc->file(n.name)) ov.merge(*f);  // keeps removals for retag_file
        try {
            vf = retag_file(p, ext, ov, cv);
        } catch (const std::exception& e) {
            warn("retag {}: {}", p.string(), e.what());
        }
    }
    bool retagged = vf != nullptr;
    if (!vf) vf = std::make_shared<Passthrough>(p, n.size);
    int64_t mt = taggable ? std::max(n.mtime, b.edits_mtime.value_or(n.mtime)) : n.mtime;
    if (retagged) mt = output_mtime(mt, cv);
    if (taggable) b.entries.push_back(Entry::make_tagged(n.name, vf, mt, EDIT_FILE, n.name, ext));
    else b.entries.push_back(Entry::make_file(n.name, vf, mt));
}

// Standalone tracks in formats players and taggers handle poorly are served as
// FLAC encoded on the fly, like the tracks of such images; floating-point
// WavPack, which FLAC cannot hold, as it is with the sidecar's tags.
bool Library::add_standalone(ListingBuild& b, const DirItem& n) {
    if (b.incomplete(n.name)) return true;
    fs::path p = b.dir / n.name;
    auto st = image_state(p, b.dir);
    if (st.st == Work<ReadyImage>::Pending) return true;
    if (st.st == Work<ReadyImage>::Settling) {
        b.settle_until(st.until);
        return true;
    }
    if (st.st != Work<ReadyImage>::Ready) return false;  // failed: shown as it is
    if (st.ready->wv) {
        Tags tg = st.ready->source_tags;
        if (b.sc) {
            tg.overlay(b.sc->album);
            if (auto f = b.sc->file(n.name)) tg.overlay(*f);
        }
        const Cover* cv = b.sc ? b.covers.get(b.sc->cover_for(n.name, nullptr)) : nullptr;
        int64_t mt = output_mtime(std::max(n.mtime, b.edits_mtime.value_or(0)), cv);
        try {
            b.entries.push_back(Entry::make_tagged(n.name, retag_wavpack(*st.ready->wv, tg, cv), mt, EDIT_FILE, n.name, "wv"));
            return true;
        } catch (const std::exception& e) {
            warn("retag {}: {}", p.string(), e.what());
            return false;
        }
    }
    if (!st.ready->av) return false;
    try {
        if (auto e = standalone_track(b, n.name, *st.ready, max_mtime({n.mtime, b.edits_mtime}))) b.virtuals.push_back(std::move(*e));
        return true;  // shown once measured
    } catch (const std::exception& e) {
        warn("encoding {}: {}", p.string(), e.what());
        return false;
    }
}

template <class L>
std::optional<L> Library::measured(const fs::path& src, const fs::path& dir, const std::function<std::optional<L>()>& load,
                                   std::function<L()> build) {
    if (auto layout = load()) return layout;
    // measuring this version failed before: fail again rather than decode it all
    // again on every rebuild (and leave the image visible as it is)
    if (auto err = failure_of(encode_failed_, src)) fail("measuring tracks failed before: {}", *err);
    bool fresh;
    {
        std::lock_guard g(mu_);
        fresh = encode_queued_.insert(src).second;
    }
    if (!fresh) return std::nullopt;
    if (enqueue(EncodeJob{src, [build] { build(); }, dir})) return std::nullopt;
    std::optional<L> layout;
    auto key = SrcKey::try_of(src);
    try {
        layout = build();
    } catch (const std::exception& e) {
        std::lock_guard g(mu_);
        encode_queued_.erase(src);
        if (key) encode_failed_[src] = {*key, e.what()};
        throw;
    }
    std::lock_guard g(mu_);
    encode_queued_.erase(src);
    return layout;
}

std::optional<std::vector<TrackLayout>> Library::track_layouts(const std::shared_ptr<const AvImage>& av, const Ranges& ranges, const fs::path& dir) {
    return measured<std::vector<TrackLayout>>(
        av->path, dir, [&] { return load_layout(*av, ranges, cache); }, [this, av, ranges] { return build_layout(*av, ranges, &cache); });
}

int64_t Library::results_mtime(const ReadyImage& ready, const Ranges& ranges) const {
    // the content changes when processing results land (layout, MD5s): make that
    // visible in the timestamps, also across restarts of this process
    auto newest = [](std::initializer_list<fs::path> files) {
        int64_t m = 0;
        for (auto& f : files) m = std::max(m, mtime_ns(f).value_or(0));
        return m;
    };
    const fs::path& src = ready.av ? ready.av->path : ready.wv ? ready.wv->path : ready.flac->path;
    auto k = SrcKey::try_of(src);
    if (!k) return 0;  // gone: the next listing drops its tracks
    if (ready.av) return newest({cache.av_index_path(src, *k), layout_path(*ready.av, ranges, cache)});
    if (ready.wv) return newest({cache.wv_index_path(src, ready.wv->key), wv_layout_path(*ready.wv, ranges, cache)});
    return newest({cache.idx_path(src, *k), cache.md5_path(src, *k)});
}

std::optional<Entry> Library::standalone_track(ListingBuild& b, const std::string& src_name, const ReadyImage& ready, int64_t mt) {
    auto& av = ready.av;
    Ranges ranges = {{0, av->total}};
    auto layout = track_layouts(av, ranges, b.dir);
    if (!layout) return std::nullopt;
    std::string name = stem_of(src_name) + ".flac";
    Tags tg = ready.source_tags;
    if (b.sc) {
        tg.overlay(b.sc->album);
        if (auto f = b.sc->file(name)) tg.overlay(*f);
    }
    const Cover* cv = b.sc ? b.covers.get(b.sc->cover_for(name, nullptr)) : nullptr;
    std::vector<flac::HeaderBlock> pictures = ready.pictures;
    if (cv) pictures = {{flac::BLOCK_PICTURE, flac_picture(*cv)}};
    mt = output_mtime(std::max(mt, results_mtime(ready, ranges)), cv);
    return Entry::make_tagged(name, std::make_shared<EncodedTrack>(av, 0, av->total, tg, (*layout)[0], pictures), mt, EDIT_FILE, name, "flac");
}

std::optional<std::vector<Entry>> Library::image_tracks(ListingBuild& b, const CueSheet& cue, const ReadyImage& ready, const DiscSlot& disc, int64_t mt) {
    auto& img = ready.flac;
    uint32_t rate = img ? img->si().sample_rate : ready.wv ? ready.wv->sample_rate : ready.av->sample_rate;
    uint64_t total = img ? img->si().total_samples : ready.wv ? ready.wv->total : ready.av->total;
    WM_ENSURE(rate % 75 == 0, "sample rate {} is not a multiple of 75 (cue frames)", rate);
    uint64_t spf = rate / 75;
    Tags base = ready.source_tags.without_track_specific();
    base.overlay(from_cue_disc(cue.fields));
    size_t ntracks = cue.tracks.size();
    Ranges ranges;
    for (size_t i = 0; i < ntracks; i++) {
        uint64_t e = i + 1 < ntracks ? cue.tracks[i + 1].index01 * spf : total;
        ranges.emplace_back(cue.tracks[i].index01 * spf, std::min(e, total));
    }
    std::optional<std::vector<TrackLayout>> layout;
    if (ready.av) {
        layout = track_layouts(ready.av, ranges, b.dir);
        if (!layout) return std::nullopt;
    }
    std::optional<std::vector<WvLayout>> wv_layout;
    if (ready.wv) {
        auto wv = ready.wv;
        wv_layout = measured<std::vector<WvLayout>>(
            wv->path, b.dir, [&] { return load_wv_layout(*wv, ranges, cache); }, [this, wv, ranges] { return build_wv_layout(*wv, ranges, &cache); });
        if (!wv_layout) return std::nullopt;
    }
    Md5Map md5s = img ? cache.load_md5s(img->path) : Md5Map{};
    bool missing = img && std::any_of(ranges.begin(), ranges.end(), [&](auto& r) { return !md5s.contains(r); });
    auto key = img ? SrcKey::try_of(img->path) : std::nullopt;
    if (missing && key && !failure_of(md5_failed_, img->path)) {
        bool fresh;
        {
            std::lock_guard g(mu_);
            fresh = md5_queued_.insert(img->path).second;
        }
        if (fresh && !enqueue(Md5Job{img, *key, ranges, b.dir})) {
            std::lock_guard g(mu_);
            md5_queued_.erase(img->path);
        }
    }
    mt = output_mtime(std::max(mt, results_mtime(ready, ranges)), nullptr);
    std::string ext = ready.wv ? "wv" : "flac";
    std::vector<Entry> out;
    for (size_t i = 0; i < ntracks; i++) {
        const auto& t = cue.tracks[i];
        if (b.sc && b.sc->hides_track(disc.keyed(), t.number)) continue;
        auto [s, e] = ranges[i];
        WM_ENSURE(s < e, "track {} starts beyond the end of the image", t.number);
        Tags tg = base;
        tg.overlay(from_cue_track(t.fields));
        set_position(tg, disc, t.number, ntracks);
        if (!tg.get("ARTIST"))
            if (auto aa = tg.get_all("ALBUMARTIST")) tg.set_many("ARTIST", *aa);
        overlay_sidecar_track(tg, b.sc, disc, t.number);
        auto [name, key, cv] = name_track(b, tg, disc, t.number, "", ext, true);
        VFilePtr vf;
        if (ready.wv) {
            vf = std::make_shared<WvTrack>(ready.wv, s, e, tg, (*wv_layout)[i], cv);
        } else {
            // a sidecar cover replaces the image's picture
            std::vector<flac::HeaderBlock> pictures = ready.pictures;
            if (cv) pictures = {{flac::BLOCK_PICTURE, flac_picture(*cv)}};
            if (ready.av) {
                vf = std::make_shared<EncodedTrack>(ready.av, s, e, tg, (*layout)[i], pictures);
            } else {
                std::optional<std::array<uint8_t, 16>> md5;
                if (auto it = md5s.find({s, e}); it != md5s.end()) md5 = it->second;
                vf = std::make_shared<FlacTrack>(img, s, e, tg, pictures, md5);
            }
        }
        out.push_back(Entry::make_tagged(std::move(name), std::move(vf), output_mtime(mt, cv), EDIT_TRACK, std::move(key), ext));
    }
    return out;
}

std::vector<Entry> Library::sacd_tracks(ListingBuild& b, const std::shared_ptr<const SacdDisc>& sacd, const DiscSlot& disc, int64_t mt, bool mc) {
    size_t n = sacd->tracks.size();
    std::vector<Entry> out;
    for (size_t i = 0; i < n; i++) {
        uint32_t num = uint32_t(i + 1);
        if (b.sc && b.sc->hides_track(disc.keyed(), num)) continue;
        Tags tg = sacd->track_tags(i);
        set_position(tg, disc, num, n);
        overlay_sidecar_track(tg, b.sc, disc, num);
        if (mc) {
            // a separate album in music servers, e.g. "Pictures at an Exhibition (Multichannel)"
            auto a = tg.get("ALBUM");
            tg.set("ALBUM", (a ? *a : std::string("SACD")) + " (Multichannel)");
        }
        // the track tables (and their pinned names) are the stereo tracks': edits of
        // a multichannel track go to the file table of its own name
        auto [name, key, cv] = name_track(b, tg, disc, num, mc ? "MC " : "", "dsf", !mc);
        auto vf = std::make_shared<DsfTrack>(sacd, i, tg, cv);
        if (mc) out.push_back(Entry::make_tagged(name, std::move(vf), output_mtime(mt, cv), EDIT_FILE, name, "dsf"));
        else out.push_back(Entry::make_tagged(std::move(name), std::move(vf), output_mtime(mt, cv), EDIT_TRACK, std::move(key), "dsf"));
    }
    return out;
}

}  // namespace wm
