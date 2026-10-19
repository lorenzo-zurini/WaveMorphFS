// SPDX-License-Identifier: GPL-3.0-or-later
// FUSE glue (libfuse3 high-level API). The tree comes from Library::list_vdir:
// the configured roots at the top, folders and files placed by sidecar
// "_target"s. Everything is read-only except the tags of editable files, which
// editors may rewrite in place: the changes go to sidecars (see writeback.hpp).
#pragma once

#include <cstddef>
#include <memory>
#include <string>

namespace wm {

class Library;

struct MountOptions {
    std::string mountpoint;
    size_t threads = 8;
    bool allow_other = false;
};

/// Mount and serve until unmounted or stopped by SIGINT/SIGTERM/SIGHUP (exit
/// code 0 then). Returns the exit code.
int mount_fs(std::shared_ptr<Library> lib, const MountOptions& opt);

}  // namespace wm
