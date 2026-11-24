// SPDX-License-Identifier: GPL-3.0-or-later
// The mount tree served over NFS version 3 (RFC 1813), with the MOUNT protocol
// on the same TCP port, so that no privilege is needed anywhere: the kernel's
// NFS client mounts it (for containers, Docker does, as a volume), and this
// process only answers requests.
//
// File handles name mount paths (a hash of the path, kept with the path in the
// cache directory), so they stay valid across restarts: clients of a hard mount
// wait while the server is away and carry on when it is back.
//
// Tag edits: NFS has no open or close. Writes to an editable file go to a write
// session (writeback.hpp) and are read back from it; once the writer has
// been quiet for a moment, the tags are read from the edited bytes and stored in
// the sidecar. The session serves the file until nobody has used it for a while.
// Only the user the server runs as (and root) may edit; clients must connect
// from a reserved port (as the kernel does), so the user they claim is the one
// their kernel vouches for.
#pragma once

#include <chrono>
#include <memory>
#include <string>

#include "library.hpp"

namespace wm {

struct NfsOptions {
    /// "PORT", "ADDRESS:PORT" or "[IPV6]:PORT"
    std::string listen = "2049";
    size_t threads = 8;
    /// accept clients connecting from unreserved ports (>= 1024)
    bool insecure = false;
    /// an edit is stored once its writer has been quiet this long
    std::chrono::milliseconds quiet{1500};
    /// a stored edit's session keeps serving the file until unused this long
    std::chrono::milliseconds keep{30000};
};

class NfsServer {
public:
    NfsServer(std::shared_ptr<Library> lib, NfsOptions opt);
    ~NfsServer();
    NfsServer(const NfsServer&) = delete;
    NfsServer& operator=(const NfsServer&) = delete;
    /// Listen and serve in background threads; throws if the address cannot be bound.
    void start();
    /// The port listened on (useful with port 0).
    uint16_t port() const;
    /// Close every connection and store the edits still pending.
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// Serve until SIGINT, SIGTERM or SIGHUP, which the calling thread must have
/// blocked before any other thread was started. Returns the exit status.
int serve_nfs(std::shared_ptr<Library> lib, const NfsOptions& opt);

}  // namespace wm
