// SPDX-License-Identifier: GPL-3.0-or-later
// FUSE glue (libfuse3 high-level API): a read-only filesystem whose top level
// lists the configured roots and whose directories come from Library::list_dir.
#pragma once

#include <memory>
#include <string>

#include "library.hpp"

namespace wm {

struct MountOptions {
    std::string mountpoint;
    size_t threads = 8;
    bool allow_other = false;
};

/// Mount and serve until unmounted or interrupted. Returns the exit code.
int mount_fs(std::shared_ptr<Library> lib, const MountOptions& opt);

}  // namespace wm
