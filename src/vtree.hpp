// SPDX-License-Identifier: GPL-3.0-or-later
// The mount tree as the NFS server (nfs.cpp) serves it: what a path names, the
// attributes reported for it, and storing a tag edit made to a file.
#pragma once

#include <optional>
#include <string>

#include "library.hpp"

namespace wm {

class WriteSession;

/// What a mount path names: the mount root, or an entry of a mount directory
/// (copied: the listing it came from may be replaced at any moment).
struct Node {
    enum Kind { Root, Dir, File } kind = Root;
    VEntry ve;
    std::string vpath;  // its path in the mount, without a leading '/' ("" for the root)
    bool editable() const { return !ve.e.tag_section.empty(); }
};

/// Resolve a mount path ("/Music/Artist/Album/01 - x.flac", the leading '/'
/// optional) through the parent directory's listing (directories may be placed
/// anywhere by sidecar "_target"s, so the path says nothing about the source
/// folders behind it). Only the last component is looked up: NFS clients walk the
/// path one name at a time and have checked the others. Names starting with a
/// dot are never served.
std::optional<Node> resolve(Library& lib, std::string_view path);

/// Modification time of directory node `n` (not a file). The root lists
/// directories that targets create: its time moves with them, and never goes
/// below `started`.
int64_t dir_mtime(Library& lib, const Node& n, int64_t started);

/// Store what an editor changed (session `ws` over file node `n`, at mount path
/// `path` for the log) in the sidecar. Throws if the edited bytes are not a
/// complete file of the node's kind.
void store_edit(Library& lib, const Node& n, WriteSession& ws, const std::string& path);

/// Sizes reported for the file system: those of the first root's, so that df
/// shows something sensible. Nothing can be stored here (edits go to the sidecar
/// tree), hence no free blocks or inodes.
struct Space {
    uint64_t bsize = 4096, frsize = 4096, blocks = 0, files = 0;
};
Space space(const Library& lib);

}  // namespace wm
