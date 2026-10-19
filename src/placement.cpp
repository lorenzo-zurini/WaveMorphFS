// SPDX-License-Identifier: GPL-3.0-or-later
// The mount's directory tree. By default every source folder appears at the same
// path under its root; sidecar "_target"s move whole folders (with their
// subfolders) or single files to other directories of the mount. Placements are
// read from the sidecars of the tags tree only, so the whole tree is known
// without listing any source folder; folders left empty by moves are hidden.
#include <algorithm>

#include "library.hpp"

namespace wm {

namespace {

/// how often the tags tree is checked for changed targets, and how long a mount
/// directory listing is reused
constexpr auto PLACEMENT_RECHECK = std::chrono::seconds(2);
constexpr auto VLIST_RECHECK = std::chrono::milliseconds(1500);

std::string join(const std::string& dir, const std::string& name) { return dir.empty() ? name : dir + "/" + name; }

std::string parent_of(const std::string& v) {
    auto s = v.rfind('/');
    return s == std::string::npos ? "" : v.substr(0, s);
}

/// "a/b" is under "a" (and under ""), not under "a/bc".
bool is_under(const std::string& v, const std::string& dir) { return dir.empty() || (v.size() > dir.size() && v.starts_with(dir) && v[dir.size()] == '/'); }

}  // namespace

const VEntry* VListing::find(std::string_view name) const {
    for (auto& e : entries)
        if (e.e.name == name) return &e;
    return nullptr;
}

std::optional<std::string> Library::vpath_of(const Placements& p, const fs::path& dir) const {
    auto r = root_of(dir);
    if (!r) return std::nullopt;
    std::string v = r->first->name;
    fs::path cur = r->first->path;
    for (auto& c : r->second) {
        cur /= c;
        if (auto it = p.folder.find(cur); it != p.folder.end()) v = it->second;
        else v = join(v, c.string());
    }
    return v;
}

std::optional<std::string> Library::entry_target(const Placements& p, const fs::path& dir, const Entry& e) const {
    auto it = p.entries.find(dir);
    if (it == p.entries.end()) return std::nullopt;
    if (auto f = it->second.files.find(e.name); f != it->second.files.end()) return f->second;
    if (e.tag_section == "track")
        if (auto t = it->second.tracks.find(e.tag_key); t != it->second.tracks.end()) return t->second;
    return std::nullopt;
}

void Library::refresh_placements(bool force) {
    // one scan at a time: concurrent scans could publish out of order under one
    // version number. Lookups that merely pass by keep the current placements
    // rather than queue behind a scan.
    std::unique_lock scan(placement_scan_mu_, std::defer_lock);
    if (force) scan.lock();
    else if (!scan.try_lock()) return;
    std::unique_lock g(place_mu_);
    if (!force && placements_checked_ && Clock::now() - *placements_checked_ < PLACEMENT_RECHECK) return;
    placements_checked_ = Clock::now();
    auto old = placements_;
    auto old_files = placement_files_;
    g.unlock();

    // sidecars of the tags tree: <tags>/<root name>/<relative dir>/wavemorph.json
    std::map<fs::path, std::pair<int64_t, std::shared_ptr<const SidecarTargets>>> files;
    for (auto& r : cfg.roots) {
        fs::path base = cfg.tags_dir / r.name;
        std::error_code ec;
        fs::recursive_directory_iterator it(base, fs::directory_options::skip_permission_denied, ec), end;
        if (ec == std::errc::no_such_file_or_directory) continue;  // no sidecars for this root
        for (; !ec && it != end; it.increment(ec)) {
            if (it->path().filename() != SIDECAR_NAME) continue;
            auto m = mtime_ns(it->path());
            if (!m) continue;  // removed just now
            fs::path rel = it->path().parent_path().lexically_relative(base);
            fs::path src = (r.path / rel).lexically_normal();
            if (!src.has_filename()) src = src.parent_path();
            auto o = old_files.find(src);
            if (o != old_files.end() && o->second.first == *m) {
                files[src] = o->second;
                continue;
            }
            auto text = try_read_text(it->path());
            if (!text) {
                // keep what was known and read it again next time: remembering the
                // failure under this mtime would drop its targets until the file changes
                warn("placements: cannot read {}", it->path().string());
                if (o != old_files.end()) files[src] = o->second;
                continue;
            }
            // keep only the targets: most sidecars have none, and their tags are large
            std::shared_ptr<const SidecarTargets> st;
            if (text->find("_target") != std::string::npos) {
                try {
                    Sidecar sc = Sidecar::parse(*text);
                    if (sc.target && rel == ".") {
                        // a root is a top-level directory of the mount by definition
                        warn("{}: ignoring \"_target\" of the root folder {}", it->path().string(), r.name);
                        sc.target.reset();
                    }
                    if (sc.target || !sc.track_targets.empty() || !sc.file_targets.empty())
                        st = std::make_shared<const SidecarTargets>(SidecarTargets{sc.target, sc.track_targets, sc.file_targets});
                } catch (const std::exception&) {
                    // the folder's listing reports the parse error
                }
            }
            files[src] = {*m, st};
        }
        if (ec) {
            // a partial scan would move every folder it missed back to its source
            // path (and bump their directories): keep the placements we have
            warn("placements: scanning {}: {}; keeping the previous placements", base.string(), ec.message());
            return;
        }
    }

    auto p = std::make_shared<Placements>();
    p->loaded = true;
    int64_t newest = 0;  // newest sidecar that places anything
    for (auto& [src, ms] : files) {
        auto& st = ms.second;
        if (!st) continue;
        newest = std::max(newest, ms.first);
        if (st->folder) {
            p->folder[src] = *st->folder;
            p->folder_by_target.emplace(*st->folder, src);
            p->targets.insert(*st->folder);
            p->sources.insert(src.string());
        }
        if (!st->tracks.empty() || !st->files.empty()) {
            auto& et = p->entries[src];
            et.tracks = st->tracks;
            et.files = st->files;
            for (auto* m : {&et.tracks, &et.files})
                for (auto& [k, t] : *m) {
                    p->entries_by_target[t].insert(src);
                    p->targets.insert(t);
                }
            p->sources.insert(src.string());
        }
    }
    bool changed = p->folder != old->folder || p->entries != old->entries;
    p->version = old->version + (changed ? 1 : 0);

    g.lock();
    placement_files_ = std::move(files);
    if (!changed) {
        if (!old->loaded) placements_ = std::move(p);
        return;
    }
    // mount directories whose contents change: targets, and where moved folders
    // were or are (and every directory above them), so music servers rescan them
    std::set<std::string> touched;
    const Placements* both[] = {old.get(), p.get()};
    for (auto* q : both) {
        touched.insert(q->targets.begin(), q->targets.end());
        for (auto& s : q->sources)
            for (auto* r : both)
                for (auto& d : {fs::path(s), fs::path(s).parent_path()})
                    if (auto v = vpath_of(*r, d)) touched.insert(*v);
    }
    // the first scan finds the tree as the sidecars shaped it when they were
    // written: stamping it with the start time would make music servers rescan
    // every moved folder after each restart
    int64_t stamp = old->loaded ? now_ns() : newest;
    for (auto v : touched)
        for (;; v = parent_of(v)) {
            auto& b = vbumps_[v];
            b = std::max(b, stamp);
            if (v.empty()) break;
        }
    placements_ = std::move(p);
    info("placements: {} folders and {} files moved", placements_->folder.size(), [&] {
        size_t n = 0;
        for (auto& [s, et] : placements_->entries) n += et.tracks.size() + et.files.size();
        return n;
    }());
}

std::shared_ptr<const VListing> Library::list_vdir(const std::string& vpath) {
    refresh_placements();
    std::shared_ptr<const Placements> p;
    {
        std::lock_guard g(place_mu_);
        p = placements_;
        if (auto it = vlistings_.find(vpath); it != vlistings_.end() && it->second->version == p->version &&
                                               Clock::now() - it->second->built < VLIST_RECHECK)
            return it->second;
    }
    auto l = build_vlisting(vpath, p);
    std::lock_guard g(place_mu_);
    vlistings_[vpath] = l;
    // listings are reused briefly (lookups of one directory come in bursts), not kept
    if (++vlist_inserts_ % 256 == 0) {
        auto now = Clock::now();
        std::erase_if(vlistings_, [&](auto& kv) { return now - kv.second->built > 4 * VLIST_RECHECK; });
    }
    return l;
}

void Library::invalidate(const fs::path& dir) {
    std::shared_ptr<Listing> l;
    {
        std::lock_guard g(mu_);
        if (auto it = listings_.find(dir); it != listings_.end()) l = it->second;
    }
    if (l) {
        std::lock_guard g(l->mu);
        l->checked = {};  // list_dir compares its signature again (an edit changed it)
    }
    // which mount directories show the folder is not recorded; they are cheap to rebuild
    std::lock_guard g(place_mu_);
    vlistings_.clear();
}

std::optional<VEntry> Library::lookup(const std::string& vpath, std::string_view name) {
    auto l = list_vdir(vpath);
    auto* ve = l->find(name);
    return ve ? std::optional<VEntry>(*ve) : std::nullopt;
}

std::shared_ptr<VListing> Library::build_vlisting(const std::string& vpath, const std::shared_ptr<const Placements>& pp) {
    const Placements& p = *pp;
    auto out = std::make_shared<VListing>();
    out->version = p.version;
    out->built = Clock::now();
    {
        std::lock_guard g(place_mu_);
        if (auto it = vbumps_.find(vpath); it != vbumps_.end()) out->mtime = it->second;
    }
    std::set<std::string> taken;
    auto add = [&](VEntry ve) {
        if (!ve.e.is_dir) ve.src_name = ve.e.name;
        if (taken.contains(ve.e.name)) {
            if (ve.e.is_dir) return;  // the same directory reached two ways
            std::string base = ve.e.name, stem = base, ext;
            if (auto dot = base.rfind('.'); dot != std::string::npos && dot > 0) stem = base.substr(0, dot), ext = base.substr(dot);
            for (int i = 2; taken.contains(ve.e.name); i++) ve.e.name = std::format("{} ({}){}", stem, i, ext);
        }
        taken.insert(ve.e.name);
        out->entries.push_back(std::move(ve));
    };
    auto add_virtual_dir = [&](const std::string& name) {
        if (!taken.contains(name)) add({Entry{name, true, {}, nullptr, 0}, {}});
    };

    // hide directories that moves have emptied, and targets that place nothing
    auto hide_empty = [&] {
        std::erase_if(out->entries, [&](const VEntry& ve) {
            if (!ve.e.is_dir) return false;
            if (!ve.e.dir.empty()) {
                if (vpath.empty()) return false;  // a root stays even when everything moved out
                std::string s = ve.e.dir.string();
                // (not lower_bound(s): "s B" or "s (2)" sort between "s" and "s/...")
                auto it = p.sources.lower_bound(s + "/");
                bool moves_below = p.sources.contains(s) || (it != p.sources.end() && is_under(*it, s));
                if (!moves_below) return false;
            }
            return list_vdir(join(vpath, ve.e.name))->entries.empty();
        });
    };

    if (vpath.empty()) {  // the roots in their configured order, then top-level directories that only targets create
        for (auto& r : cfg.roots) add({Entry{r.name, true, r.path, nullptr, 0}, {}});
        for (auto& t : p.targets) add_virtual_dir(t.substr(0, t.find('/')));
        hide_empty();
        return out;
    }

    // source folders placed here: the one at the same path, folders targeted
    // here, and subfolders of folders targeted above
    std::vector<fs::path> mapped;
    auto consider = [&](const fs::path& d) {
        std::error_code ec;
        if (std::find(mapped.begin(), mapped.end(), d) != mapped.end() || !fs::is_directory(d, ec)) return;
        if (vpath_of(p, d) == vpath) mapped.push_back(d);
    };
    auto comps = split(vpath, '/');
    for (auto& r : cfg.roots)
        if (r.name == comps[0]) {
            fs::path d = r.path;
            for (size_t i = 1; i < comps.size(); i++) d /= comps[i];
            consider(d);
        }
    for (std::string a = vpath; !a.empty(); a = parent_of(a)) {  // (targets are never "")
        auto [lo, hi] = p.folder_by_target.equal_range(a);
        for (auto it = lo; it != hi; ++it) consider(a == vpath ? it->second : it->second / fs::path(vpath.substr(a.size() + 1)));
    }
    std::sort(mapped.begin(), mapped.end());

    for (auto& d : mapped) {
        auto l = list_dir(d);
        out->mtime = std::max(out->mtime, l->mtime);
        for (auto& e : l->entries) {
            if (e.is_dir) {
                if (vpath_of(p, e.dir) != join(vpath, e.name)) continue;  // moved elsewhere
                add({e, {}});
            } else if (auto t = entry_target(p, d, e); !t || *t == vpath) {
                add({e, d});
            }
        }
    }
    // single files placed here from other folders
    if (auto it = p.entries_by_target.find(vpath); it != p.entries_by_target.end())
        for (auto& d : it->second) {
            std::error_code ec;
            if (std::find(mapped.begin(), mapped.end(), d) != mapped.end() || !fs::is_directory(d, ec)) continue;
            auto l = list_dir(d);
            bool any = false;
            for (auto& e : l->entries)
                if (!e.is_dir && entry_target(p, d, e) == vpath) add({e, d}), any = true;
            if (any) out->mtime = std::max(out->mtime, l->mtime);
        }
    // directories that exist only because targets lie below them
    for (auto it = p.targets.lower_bound(vpath + "/"); it != p.targets.end() && is_under(*it, vpath); ++it) {
        std::string rest = it->substr(vpath.size() + 1);
        add_virtual_dir(rest.substr(0, rest.find('/')));
    }
    hide_empty();
    std::sort(out->entries.begin(), out->entries.end(), [](auto& a, auto& b) { return a.e.name < b.e.name; });
    return out;
}

}  // namespace wm
