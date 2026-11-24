// SPDX-License-Identifier: GPL-3.0-or-later
#include "vtree.hpp"

#include <sys/statvfs.h>

#include "writeback.hpp"

namespace wm {

std::optional<Node> resolve(Library& lib, std::string_view path) {
    std::string parent, vpath, last;
    for (auto& c : split(path, '/')) {
        if (c.empty()) continue;
        if (starts_with(c, ".")) return std::nullopt;
        parent = vpath;
        vpath = vpath.empty() ? c : vpath + "/" + c;
        last = c;
    }
    if (last.empty()) return Node{};
    auto ve = lib.lookup(parent, last);
    if (!ve) return std::nullopt;
    return Node{ve->e.is_dir ? Node::Dir : Node::File, std::move(*ve), vpath};
}

int64_t dir_mtime(Library& lib, const Node& n, int64_t started) {
    if (n.kind == Node::Root) return std::max(started, lib.list_vdir("")->mtime);
    return lib.list_vdir(n.vpath)->mtime;
}

void store_edit(Library& lib, const Node& n, WriteSession& ws, const std::string& path) {
    auto& ve = n.ve;
    uint64_t changes = ws.changes();  // what is read back below includes these
    auto original = [&](uint64_t o, size_t l) { return ve.e.file->read_at(o, l); };
    auto edited = [&](uint64_t o, size_t l) { return ws.read(o, l); };
    Tags before = with_context("generated file", [&] { return read_file_tags(ve.e.tag_ext, original, ve.e.file->size()); });
    Tags after = with_context("edited file", [&] { return read_file_tags(ve.e.tag_ext, edited, ws.size()); });
    Tags diff = tag_changes(before, after);
    // the front cover: a new picture becomes the entry's "_cover", a removed one drops it
    std::optional<Library::CoverEdit> cover;
    {
        auto was = read_front_cover(ve.e.tag_ext, original, ve.e.file->size());
        auto now = read_front_cover(ve.e.tag_ext, edited, ws.size());
        if (was != now) cover = Library::CoverEdit{std::move(now)};
    }
    if (diff.empty() && !cover) {
        info("edit of {}: no tag changes", path);
    } else {
        lib.apply_edit(ve.src_dir, ve.src_name, ve.e.tag_section, ve.e.tag_key, diff, cover);
        lib.invalidate(ve.src_dir);  // editors read the file back right away
        std::string what;
        for (auto& [k, v] : diff.m) what += (what.empty() ? "" : ", ") + k + (v.empty() ? " (removed)" : "");
        if (cover) what += std::string(what.empty() ? "" : ", ") + (cover->image ? "front cover" : "front cover (removed)");
        info("edit of {}: {} -> sidecar {}[{}]", path, what, ve.e.tag_section, ve.e.tag_key);
    }
    ws.mark_stored(changes);
}

Space space(const Library& lib) {
    Space out;
    if (lib.cfg.roots.empty()) return out;
    struct statvfs s;
    if (::statvfs(lib.cfg.roots[0].path.c_str(), &s) == 0) {
        out.bsize = s.f_bsize;
        out.frsize = s.f_frsize;
        out.blocks = s.f_blocks;
        out.files = s.f_files;
    }
    return out;
}

}  // namespace wm
