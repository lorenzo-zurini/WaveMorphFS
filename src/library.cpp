// SPDX-License-Identifier: GPL-3.0-or-later
#include "library.hpp"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstring>
#include <thread>

#include <nlohmann/json.hpp>

#include "cue.hpp"
#include "retag.hpp"

namespace wm {

namespace {

/// Generated and retagged files are never older than this: bump it whenever a
/// change alters what they contain (tag rendering, encoding...), so that music
/// servers re-read them once. 2026-10-01 13:00 UTC: C++ rewrite (tag synonyms,
/// SACD genres, kept tag spelling, APE served as encoded FLAC).
/// 2026-10-01 16:00 UTC: padding in generated tag areas (editable mount).
constexpr int64_t OUTPUT_EPOCH_NS = 1790870400LL * 1'000'000'000;

const std::vector<std::string_view> AUDIO_IMAGE_EXT = {"flac", "ape", "wv", "tta", "tak", "wav", "m4a", "aiff", "aif"};
/// A file modified more recently than this is assumed to still be written.
constexpr auto SETTLE = std::chrono::seconds(60);
constexpr auto RECHECK = std::chrono::milliseconds(1500);

struct DirItem {
    std::string name;
    bool is_dir = false, is_file = false;
    uint64_t size = 0;
    int64_t mtime = 0;
};

std::vector<DirItem> read_dir(const fs::path& dir) {
    DIR* d = ::opendir(dir.c_str());
    if (!d) fail("read_dir {}: {}", dir.string(), std::strerror(errno));
    std::vector<DirItem> out;
    while (auto* e = ::readdir(d)) {
        std::string n = e->d_name;
        if (n == "." || n == "..") continue;
        struct stat st;
        if (::stat((dir / n).c_str(), &st) != 0) continue;  // follows symlinks, like the source tree should
        out.push_back({n, S_ISDIR(st.st_mode), S_ISREG(st.st_mode), uint64_t(st.st_size),
                       int64_t(st.st_mtim.tv_sec) * 1'000'000'000 + st.st_mtim.tv_nsec});
    }
    ::closedir(d);
    std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.name < b.name; });
    return out;
}

bool is_ignored(const std::string& name) {
    std::string l = lower(name);
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
        if (::stat((dir / e->d_name).c_str(), &st) == 0 && S_ISREG(st.st_mode))
            best = std::max(*best, int64_t(st.st_mtim.tv_sec) * 1'000'000'000 + st.st_mtim.tv_nsec);
    }
    ::closedir(d);
    return best;
}

bool recently_modified(const fs::path& p) {
    auto m = mtime_ns(p);
    return m && now_ns() - *m < std::chrono::duration_cast<std::chrono::nanoseconds>(SETTLE).count() && now_ns() >= *m;
}

std::string stem_of(const std::string& name) {
    auto dot = name.rfind('.');
    return dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
}

bool is_audio_name(const std::string& n) {
    auto e = ext_lower(n);
    return std::find(AUDIO_IMAGE_EXT.begin(), AUDIO_IMAGE_EXT.end(), e) != AUDIO_IMAGE_EXT.end();
}

/// Find the audio file a single-FILE cue refers to. Rips are often converted after
/// the cue was written (cue says .wav, file is .flac), so fall back to matching by
/// stem, then by the cue's own stem.
std::optional<std::string> resolve_image(const CueSheet& cue, const std::string& cue_name, const std::vector<DirItem>& names) {
    std::string ref = cue.files[0];
    std::replace(ref.begin(), ref.end(), '\\', '/');
    ref = fs::path(ref).filename().string();
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

std::optional<uint32_t> picture_type(const Bytes& data) {
    if (data.size() < 4) return std::nullopt;
    return be32(data.data());
}

std::string error_text(const std::exception& e) { return e.what(); }

}  // namespace

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
        write_status();
    }
}

void Library::start_prescan(std::chrono::seconds every) {
    std::thread([self = shared_from_this(), every] {
        while (true) {
            auto t = Clock::now();
            size_t n = 0;
            for (auto& r : self->cfg.roots) self->walk(r.path, n);
            info("prescan: {} directories in {:.1f}s", n, std::chrono::duration<double>(Clock::now() - t).count());
            self->write_status();
            std::this_thread::sleep_for(every);
        }
    }).detach();
}

void Library::walk(const fs::path& dir, size_t& n) {
    n++;
    std::shared_ptr<const Listing> l;
    try {
        l = list_dir(dir);
    } catch (const std::exception& e) {
        return;
    }
    for (auto& e : l->entries)
        if (e.is_dir) walk(e.dir, n);
}

void Library::run_job(Job job) {
    if (auto* j = std::get_if<ImageJob>(&job)) {
        auto key = SrcKey::try_of(j->src);
        Work<ReadyImage> st;
        try {
            st.ready = std::make_shared<const ReadyImage>(process_image(j->src));
            st.st = Work<ReadyImage>::Ready;
            info("image ready: {}", j->src.string());
        } catch (const std::exception& e) {
            st.st = Work<ReadyImage>::Failed;
            st.err = error_text(e);
            warn("image failed: {}: {}", j->src.string(), st.err);
        }
        if (key) {
            std::lock_guard g(mu_);
            images_[j->src] = {*key, st};
        }
        bump(j->dir);
    } else if (auto* j = std::get_if<Md5Job>(&job)) {
        try {
            auto d = track_md5s(*j->image, j->ranges);
            Md5Map m;
            for (size_t i = 0; i < d.size(); i++) m[j->ranges[i]] = d[i];
            cache.store_md5s(j->image->path, m);
            info("track MD5s ready: {}", j->image->path.string());
        } catch (const std::exception& e) {
            warn("MD5 of {}: {}", j->image->path.string(), e.what());
        }
        {
            std::lock_guard g(mu_);
            md5_queued_.erase(j->image->path);
        }
        bump(j->dir);
    } else if (auto* j = std::get_if<EncodeJob>(&job)) {
        try {
            auto t = Clock::now();
            build_layout(*j->image, j->ranges, &cache);
            info("tracks measured: {} ({:.0f}s)", j->image->path.string(), std::chrono::duration<double>(Clock::now() - t).count());
        } catch (const std::exception& e) {
            warn("measuring tracks of {}: {}", j->image->path.string(), e.what());
        }
        {
            std::lock_guard g(mu_);
            encode_queued_.erase(j->image->path);
        }
        bump(j->dir);
    } else if (auto* j = std::get_if<SacdJob>(&job)) {
        auto key = SrcKey::try_of(j->src);
        const char* area = j->mc ? "multichannel" : "stereo";
        Work<SacdDisc> st;
        try {
            auto d = SacdDisc::open(j->src, &cache, j->mc);
            info("SACD {} area ready: {} ({} tracks, {} ch{})", area, j->src.string(), d->tracks.size(), d->channels, d->dst ? ", DST" : "");
            st.st = Work<SacdDisc>::Ready;
            st.ready = d;
        } catch (const std::exception& e) {
            st.st = Work<SacdDisc>::Failed;
            st.err = error_text(e);
            if (st.err.find("no multichannel area") != std::string::npos) debug("{}: {}", j->src.string(), st.err);
            else warn("SACD {} area failed: {}: {}", area, j->src.string(), st.err);
        }
        if (key) {
            std::lock_guard g(mu_);
            sacds_[{j->src, j->mc}] = {*key, st};
        }
        bump(j->dir);
    }
}

ReadyImage Library::process_image(const fs::path& src) const {
    ReadyImage r;
    if (ext_lower(src) != "flac") {
        auto av = AvImage::open(src, &cache);
        r.source_tags = av->tags;
        r.av = av;
        return r;
    }
    fs::path flac_path = src;
    auto meta = flac::FlacMeta::read(flac_path);
    flac::FrameIndex idx;
    try {
        idx = cache.index_for(flac_path, meta);
    } catch (const Error& e) {
        // frames can only be copied from fixed-blocksize images; others are
        // decoded and re-encoded on the fly like non-FLAC images
        if (std::string(e.what()).find("variable-blocksize") == std::string::npos) throw;
        auto av = AvImage::open(src, &cache);
        r.source_tags = Tags::from_pairs(meta.vorbis_comments());
        r.av = av;
        return r;
    }
    r.source_tags = Tags::from_pairs(meta.vorbis_comments());
    for (auto* b : meta.pictures())
        if (b->data.size() <= (2 << 20) && picture_type(b->data) == 3) {
            r.pictures.push_back(*b);
            break;
        }
    r.flac = FlacImage::open(flac_path, std::move(meta), std::move(idx));
    return r;
}

void Library::write_status() const {
    std::vector<std::string> sections[4];  // ready, pending, settling, failed
    auto push = [&](int i, std::string s) { sections[i].push_back(std::move(s)); };
    {
        std::lock_guard g(mu_);
        for (auto& [p, ks] : images_) {
            auto& st = ks.second;
            std::string label = "image  " + p.string();
            switch (st.st) {
            case Work<ReadyImage>::Ready: push(0, label); break;
            case Work<ReadyImage>::Pending: push(1, label); break;
            case Work<ReadyImage>::Settling: push(2, label); break;
            case Work<ReadyImage>::Failed: push(3, label + "\n         " + st.err); break;
            }
        }
        for (auto& [pk, ks] : sacds_) {
            auto& st = ks.second;
            std::string label = std::string(pk.second ? "SACD/MC " : "SACD    ") + pk.first.string();
            switch (st.st) {
            case Work<SacdDisc>::Ready:
                push(0, std::format("{}  ({} tracks, {} ch{})", label, st.ready->tracks.size(), st.ready->channels, st.ready->dst ? ", DST" : ""));
                break;
            case Work<SacdDisc>::Pending: push(1, label); break;
            case Work<SacdDisc>::Settling: push(2, label); break;
            case Work<SacdDisc>::Failed:
                if (st.err.find("no multichannel area") == std::string::npos) push(3, label + "\n         " + st.err);
                break;
            }
        }
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
    if (auto text = try_read_text(p)) doc = json::parse(*text, nullptr, true, /*ignore_comments=*/true);
    WM_ENSURE(doc.is_object(), "{} is not a JSON object", p.string());
    if (!doc.contains(section) || !doc[section].is_object()) doc[section] = json::object();
    json& entry = doc[section][key];
    if (!entry.is_object()) entry = json::object();
    auto erase_field = [](json& obj, const std::string& k, const std::string& keep) {
        std::vector<std::string> drop;
        for (auto& [ek, ev] : obj.items())
            if (ek != keep && same_field(ek, k)) drop.push_back(ek);
        for (auto& d : drop) obj.erase(d);
    };
    for (auto& [k, vals] : changes.m) {
        // keep the spelling already used in this entry, else in the album table
        std::string name = k;
        bool found = false;
        for (auto& [ek, ev] : entry.items())
            if (same_field(ek, k)) {
                name = ek;
                found = true;
                break;
            }
        if (!found && doc.contains("album") && doc["album"].is_object())
            for (auto& [ak, av] : doc["album"].items())
                if (same_field(ak, k)) {
                    name = ak;
                    break;
                }
        erase_field(entry, k, name);
        entry[name] = vals.empty() ? json("") : vals.size() == 1 ? json(vals[0]) : json(vals);
        // a per-file entry would override the track entry: drop the field there
        if (section == "track" && doc.contains("file") && doc["file"].contains(file_name) && doc["file"][file_name].is_object())
            erase_field(doc["file"][file_name], k, "");
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

Work<ReadyImage> Library::image_state(const fs::path& src, const fs::path& dir) {
    auto key = SrcKey::try_of(src);
    if (!key) return {Work<ReadyImage>::Failed, {}, nullptr, "stat failed"};
    {
        std::lock_guard g(mu_);
        if (auto it = images_.find(src); it != images_.end() && it->second.first == *key) {
            auto& st = it->second.second;
            if (st.st != Work<ReadyImage>::Settling || Clock::now() < st.until) return st;
        }
    }
    if (recently_modified(src)) {
        Work<ReadyImage> st{Work<ReadyImage>::Settling, Clock::now() + SETTLE, nullptr, ""};
        std::lock_guard g(mu_);
        images_[src] = {*key, st};
        return st;
    }
    // cached work is cheap to reopen: do it inline so albums never vanish from a
    // listing just because the process restarted
    bool cached = cache.image_is_cached(src);
    {
        std::lock_guard g(mu_);
        images_[src] = {*key, Work<ReadyImage>{}};
    }
    if (!cached && enqueue(ImageJob{src, dir})) return {};
    Work<ReadyImage> st;
    try {
        st.ready = std::make_shared<const ReadyImage>(process_image(src));
        st.st = Work<ReadyImage>::Ready;
    } catch (const std::exception& e) {
        st.st = Work<ReadyImage>::Failed;
        st.err = error_text(e);
    }
    std::lock_guard g(mu_);
    images_[src] = {*key, st};
    return st;
}

Work<SacdDisc> Library::sacd_state(const fs::path& src, const fs::path& dir, bool mc) {
    auto mkey = std::pair{src, mc};
    auto key = SrcKey::try_of(src);
    if (!key) return {Work<SacdDisc>::Failed, {}, nullptr, "stat failed"};
    {
        std::lock_guard g(mu_);
        if (auto it = sacds_.find(mkey); it != sacds_.end() && it->second.first == *key) {
            auto& st = it->second.second;
            if (st.st != Work<SacdDisc>::Settling || Clock::now() < st.until) return st;
        }
    }
    if (recently_modified(src)) {
        Work<SacdDisc> st{Work<SacdDisc>::Settling, Clock::now() + SETTLE, nullptr, ""};
        std::lock_guard g(mu_);
        sacds_[mkey] = {*key, st};
        return st;
    }
    bool cached = sacd_is_cached(cache, src, mc);
    {
        std::lock_guard g(mu_);
        sacds_[mkey] = {*key, Work<SacdDisc>{}};
    }
    if (!cached && enqueue(SacdJob{src, dir, mc})) return {};
    Work<SacdDisc> st;
    try {
        st.ready = SacdDisc::open(src, &cache, mc);
        st.st = Work<SacdDisc>::Ready;
    } catch (const std::exception& e) {
        st.st = Work<SacdDisc>::Failed;
        st.err = error_text(e);
    }
    std::lock_guard g(mu_);
    sacds_[mkey] = {*key, st};
    return st;
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
        if (Clock::now() - cached->checked < RECHECK) return cached;
    }
    std::lock_guard dg(*lock);
    Sig sig = signature(dir);
    {
        std::lock_guard g(mu_);
        if (auto it = listings_.find(dir); it != listings_.end()) cached = it->second;
    }
    if (cached && cached->sig == sig && !(cached->retry_at && Clock::now() >= *cached->retry_at)) {
        std::lock_guard g(cached->mu);
        cached->checked = Clock::now();
        return cached;
    }
    auto l = build_listing(dir, sig);
    std::lock_guard g(mu_);
    listings_[dir] = l;
    return l;
}

std::shared_ptr<Listing> Library::build_listing(const fs::path& dir, const Sig& sig) {
    auto names = with_context("read_dir " + dir.string(), [&] { return read_dir(dir); });
    std::set<std::string> name_set;
    for (auto& n : names) name_set.insert(n.name);
    auto downloading = [&](const std::string& n) { return name_set.contains(n + ".!qB") || name_set.contains(n + ".!qb"); };
    auto item = [&](const std::string& n) -> const DirItem* {
        for (auto& x : names)
            if (x.name == n) return &x;
        return nullptr;
    };

    auto overlay = overlay_dir(dir);
    std::optional<Sidecar> sidecar;
    try {
        sidecar = Sidecar::load(dir, overlay);
    } catch (const std::exception& e) {
        warn("sidecar in {}: {}", dir.string(), e.what());
    }
    std::optional<int64_t> sidecar_mtime = sidecar ? sidecar->mtime : std::nullopt;
    const Sidecar* sc = sidecar ? &*sidecar : nullptr;
    std::set<std::string> hidden;
    std::vector<Entry> virtuals;
    std::optional<Clock::time_point> retry_at;
    auto note_settle = [&](Clock::time_point until) { retry_at = retry_at ? std::min(*retry_at, until) : until; };
    auto max_mtime = [](std::initializer_list<std::optional<int64_t>> v) {
        int64_t m = 0;
        for (auto& x : v)
            if (x) m = std::max(m, *x);
        return m;
    };

    // --- CUE + image groups
    struct Group {
        std::string cue_name;
        CueSheet cue;
        std::string img_name;
    };
    std::vector<Group> groups;
    for (auto& n : names) {
        if (!n.is_file || ext_lower(n.name) != "cue") continue;
        CueSheet cue;
        try {
            cue = CueSheet::read(dir / n.name);
        } catch (const std::exception& e) {
            warn("cue {}: {}", (dir / n.name).string(), e.what());
            continue;
        }
        if (!cue.is_image()) continue;
        if (auto img = resolve_image(cue, n.name, names)) {
            groups.push_back({n.name, std::move(cue), *img});
        } else {
            std::string ref = cue.files[0];
            std::replace(ref.begin(), ref.end(), '\\', '/');
            if (downloading(fs::path(ref).filename().string())) hidden.insert(n.name);  // image still downloading as <name>.!qB
        }
    }
    bool multi = groups.size() > 1;
    for (size_t gi = 0; gi < groups.size(); gi++) {
        auto& [cue_name, cue, img_name] = groups[gi];
        fs::path img_path = dir / img_name;
        if (downloading(img_name)) {
            hidden.insert(cue_name);
            hidden.insert(img_name);
            continue;
        }
        auto st = image_state(img_path, dir);
        if (st.st == Work<ReadyImage>::Ready) {
            std::optional<uint32_t> disc;
            if (auto d = cue.fields.find("DISCNUMBER"); d != cue.fields.end())
                if (auto v = parse_u64(trim(split(d->second, '/')[0])); v && *v <= UINT32_MAX) disc = uint32_t(*v);
            if (!disc && multi) disc = uint32_t(gi + 1);
            try {
                auto v = image_tracks(dir, cue, *st.ready, disc, multi, groups.size(), sc,
                                      max_mtime({mtime_ns(dir / cue_name), mtime_ns(img_path), sidecar_mtime}));
                hidden.insert(cue_name);
                hidden.insert(img_name);
                if (v)
                    for (auto& e : *v) virtuals.push_back(std::move(e));
            } catch (const std::exception& e) {
                warn("splitting {}: {}", img_path.string(), e.what());
            }
        } else if (st.st == Work<ReadyImage>::Pending || st.st == Work<ReadyImage>::Settling) {
            if (st.st == Work<ReadyImage>::Settling) note_settle(st.until);
            hidden.insert(cue_name);
            hidden.insert(img_name);
        }
        // Failed: leave cue + image visible as-is
    }

    // --- SACD ISOs
    std::vector<const DirItem*> isos;
    for (auto& n : names)
        if (n.is_file && ext_lower(n.name) == "iso") isos.push_back(&n);
    bool multi_iso = isos.size() > 1;
    for (size_t ii = 0; ii < isos.size(); ii++) {
        const std::string& n = isos[ii]->name;
        fs::path p = dir / n;
        if (downloading(n) || !is_sacd(p)) continue;
        std::optional<uint32_t> disc_no = multi_iso ? std::optional<uint32_t>(uint32_t(ii + 1)) : std::nullopt;
        auto st = sacd_state(p, dir, false);
        int64_t mt = max_mtime({mtime_ns(p), sidecar_mtime});
        if (auto k = SrcKey::try_of(p)) mt = std::max(mt, mtime_ns(cache.sacd_frames_path(p, *k, false)).value_or(0));
        if (st.st == Work<SacdDisc>::Ready) {
            for (auto& e : sacd_tracks(st.ready, disc_no, isos.size(), sc, mt, false)) virtuals.push_back(std::move(e));
            hidden.insert(n);
            if (cfg.sacd_multichannel) {
                auto mc = sacd_state(p, dir, true);
                if (mc.st == Work<SacdDisc>::Ready)
                    for (auto& e : sacd_tracks(mc.ready, disc_no, isos.size(), sc, mt, true)) virtuals.push_back(std::move(e));
            }
        } else if (st.st == Work<SacdDisc>::Pending || st.st == Work<SacdDisc>::Settling) {
            if (st.st == Work<SacdDisc>::Settling) note_settle(st.until);
            hidden.insert(n);
        }
    }

    // --- everything else
    std::vector<Entry> entries;
    bool has_cover = false;
    for (auto& n : names) {
        if (hidden.contains(n.name) || is_ignored(n.name)) continue;
        fs::path p = dir / n.name;
        if (n.is_dir) {
            entries.push_back({n.name, true, p, nullptr, n.mtime});
            continue;
        }
        if (is_cover_name(n.name)) has_cover = true;
        std::string ext = ext_lower(n.name);
        bool taggable = ext == "flac" || ext == "mp3" || ext == "m4a";
        VFilePtr vf;
        if (sc && taggable && (!sc->album.empty() || sc->file(n.name))) {
            Tags ov = sc->album;
            if (auto f = sc->file(n.name)) ov.overlay(*f);
            try {
                vf = retag_file(p, ext, ov);
            } catch (const std::exception& e) {
                warn("retag {}: {}", p.string(), e.what());
            }
        }
        bool retagged = vf != nullptr;
        if (!vf) vf = std::make_shared<Passthrough>(p, n.size);
        int64_t mt = taggable ? std::max(n.mtime, sidecar_mtime.value_or(n.mtime)) : n.mtime;
        if (retagged) mt = std::max(mt, OUTPUT_EPOCH_NS);
        Entry e{n.name, false, {}, vf, mt};
        if (taggable) e.tag_section = "file", e.tag_key = n.name, e.tag_ext = ext;
        entries.push_back(std::move(e));
    }

    // cover art exported into the sidecar tree
    if (!has_cover && !virtuals.empty() && overlay) {
        for (auto c : {"cover.jpg", "cover.png"}) {
            fs::path p = *overlay / c;
            struct stat st;
            if (::stat(p.c_str(), &st) == 0) {
                entries.push_back({c, false, {}, std::make_shared<Passthrough>(p, uint64_t(st.st_size)),
                                   int64_t(st.st_mtim.tv_sec) * 1'000'000'000 + st.st_mtim.tv_nsec});
                break;
            }
        }
    }

    // virtual tracks, de-duplicated against real names
    std::set<std::string> taken;
    for (auto& e : entries) taken.insert(e.name);
    for (auto& v : virtuals) {
        std::string name = v.name;
        for (int i = 2; taken.contains(name); i++) {
            auto dot = v.name.rfind('.');
            std::string stem = dot == std::string::npos ? v.name : v.name.substr(0, dot);
            std::string ext = dot == std::string::npos ? "" : v.name.substr(dot + 1);
            name = std::format("{} ({}).{}", stem, i, ext);
        }
        v.name = name;
        entries.push_back(std::move(v));
    }
    std::sort(entries.begin(), entries.end(), [](auto& a, auto& b) { return a.name < b.name; });

    std::optional<int64_t> gen_time;
    {
        std::lock_guard g(mu_);
        if (auto it = generations_.find(dir); it != generations_.end()) gen_time = it->second.second;
    }
    auto l = std::make_shared<Listing>();
    l->entries = std::move(entries);
    l->mtime = max_mtime({sig.dir_mtime, sig.sidecar_mtime, sig.overlay_mtime, gen_time});
    for (auto& e : l->entries)
        if (!e.is_dir) l->mtime = std::max(l->mtime, e.mtime);
    l->sig = sig;
    l->checked = Clock::now();
    l->retry_at = retry_at;
    (void)item;
    return l;
}

std::optional<std::vector<Entry>> Library::image_tracks(const fs::path& dir, const CueSheet& cue, const ReadyImage& ready,
                                                        std::optional<uint32_t> disc, bool multi, size_t ndiscs, const Sidecar* sidecar, int64_t mt) {
    auto& img = ready.flac;
    uint32_t rate = img ? img->si().sample_rate : ready.av->sample_rate;
    uint64_t total = img ? img->si().total_samples : ready.av->total;
    WM_ENSURE(rate % 75 == 0, "sample rate {} is not a multiple of 75 (cue frames)", rate);
    uint64_t spf = rate / 75;
    Tags base = ready.source_tags.without_track_specific();
    base.overlay(from_cue_disc(cue.fields));
    size_t ntracks = cue.tracks.size();
    std::vector<std::pair<uint64_t, uint64_t>> ranges;
    for (size_t i = 0; i < ntracks; i++) {
        uint64_t e = i + 1 < ntracks ? cue.tracks[i + 1].index01 * spf : total;
        ranges.emplace_back(cue.tracks[i].index01 * spf, std::min(e, total));
    }
    std::optional<std::vector<TrackLayout>> layout;
    if (ready.av) {
        layout = load_layout(*ready.av, ranges, cache);
        if (!layout) {
            bool fresh;
            {
                std::lock_guard g(mu_);
                fresh = encode_queued_.insert(ready.av->path).second;
            }
            if (!fresh) return std::nullopt;
            if (enqueue(EncodeJob{ready.av, ranges, dir})) return std::nullopt;
            try {
                layout = build_layout(*ready.av, ranges, &cache);
            } catch (...) {
                std::lock_guard g(mu_);
                encode_queued_.erase(ready.av->path);
                throw;
            }
            std::lock_guard g(mu_);
            encode_queued_.erase(ready.av->path);
        }
    }
    Md5Map md5s = img ? cache.load_md5s(img->path) : Md5Map{};
    bool missing = img && std::any_of(ranges.begin(), ranges.end(), [&](auto& r) { return !md5s.contains(r); });
    if (missing) {
        bool fresh;
        {
            std::lock_guard g(mu_);
            fresh = md5_queued_.insert(img->path).second;
        }
        if (fresh && !enqueue(Md5Job{img, ranges, dir})) {
            std::lock_guard g(mu_);
            md5_queued_.erase(img->path);
        }
    }
    // the content changes when processing results land (layout, MD5s): make that
    // visible in the timestamps, also across restarts of this process
    if (ready.av) {
        auto k = SrcKey::of(ready.av->path);
        mt = std::max({mt, mtime_ns(cache.av_index_path(ready.av->path, k)).value_or(0), mtime_ns(layout_path(*ready.av, ranges, cache)).value_or(0)});
    } else if (auto k = SrcKey::try_of(img->path)) {
        mt = std::max({mt, mtime_ns(cache.idx_path(img->path, *k)).value_or(0), mtime_ns(cache.md5_path(img->path, *k)).value_or(0)});
    }
    mt = std::max(mt, OUTPUT_EPOCH_NS);
    std::vector<Entry> out;
    for (size_t i = 0; i < ntracks; i++) {
        const auto& t = cue.tracks[i];
        auto [s, e] = ranges[i];
        WM_ENSURE(s < e, "track {} starts beyond the end of the image", t.number);
        Tags tg = base;
        tg.overlay(from_cue_track(t.fields));
        tg.set("TRACKNUMBER", std::to_string(t.number));
        tg.set("TRACKTOTAL", std::to_string(ntracks));
        if (disc) {
            tg.set("DISCNUMBER", std::to_string(*disc));
            if (multi) tg.set("DISCTOTAL", std::to_string(ndiscs));
        }
        if (!tg.get("ARTIST"))
            if (auto aa = tg.get_all("ALBUMARTIST")) tg.set_many("ARTIST", *aa);
        if (sidecar) {
            tg.overlay(sidecar->album);
            if (auto tt = sidecar->track(multi ? disc : std::nullopt, t.number)) tg.overlay(*tt);
        }
        auto title_v = tg.get("TITLE");
        std::string title = title_v ? sanitize_name(*title_v) : std::format("Track {:02}", t.number);
        std::string name = multi && disc ? std::format("{}-{:02} - {}.flac", *disc, t.number, title) : std::format("{:02} - {}.flac", t.number, title);
        if (sidecar)
            if (auto ft = sidecar->file(name)) tg.overlay(*ft);
        std::string key = multi && disc ? std::format("{}-{:02}", *disc, t.number) : std::to_string(t.number);
        if (ready.av) {
            out.push_back({name, false, {}, std::make_shared<EncodedTrack>(ready.av, s, e, tg, (*layout)[i]), mt, "track", key, "flac"});
            continue;
        }
        std::optional<std::array<uint8_t, 16>> md5;
        if (auto it = md5s.find({s, e}); it != md5s.end()) md5 = it->second;
        out.push_back({name, false, {}, std::make_shared<FlacTrack>(img, s, e, tg, ready.pictures, md5), mt, "track", key, "flac"});
    }
    return out;
}

std::vector<Entry> Library::sacd_tracks(const std::shared_ptr<const SacdDisc>& disc, std::optional<uint32_t> disc_no, size_t ndiscs,
                                        const Sidecar* sidecar, int64_t mt, bool mc) {
    size_t n = disc->tracks.size();
    mt = std::max(mt, OUTPUT_EPOCH_NS);
    std::vector<Entry> out;
    for (size_t i = 0; i < n; i++) {
        uint32_t num = uint32_t(i + 1);
        Tags tg = disc->track_tags(i);
        tg.set("TRACKNUMBER", std::to_string(num));
        tg.set("TRACKTOTAL", std::to_string(n));
        if (disc_no) {
            tg.set("DISCNUMBER", std::to_string(*disc_no));
            tg.set("DISCTOTAL", std::to_string(ndiscs));
        }
        if (sidecar) {
            tg.overlay(sidecar->album);
            if (auto tt = sidecar->track(disc_no, num)) tg.overlay(*tt);
        }
        if (mc) {
            // a separate album in music servers, e.g. "Pictures at an Exhibition (Multichannel)"
            auto a = tg.get("ALBUM");
            tg.set("ALBUM", (a ? *a : std::string("SACD")) + " (Multichannel)");
        }
        auto title_v = tg.get("TITLE");
        std::string title = title_v ? sanitize_name(*title_v) : std::format("Track {:02}", num);
        std::string prefix = mc ? "MC " : "";
        std::string name = disc_no ? std::format("{}{}-{:02} - {}.dsf", prefix, *disc_no, num, title) : std::format("{}{:02} - {}.dsf", prefix, num, title);
        if (sidecar)
            if (auto ft = sidecar->file(name)) tg.overlay(*ft);
        std::string key = disc_no ? std::format("{}-{:02}", *disc_no, num) : std::to_string(num);
        out.push_back({name, false, {}, std::make_shared<DsfTrack>(disc, i, tg), mt, "track", key, "dsf"});
    }
    return out;
}

}  // namespace wm
