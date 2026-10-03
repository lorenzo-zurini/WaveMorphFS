// SPDX-License-Identifier: GPL-3.0-or-later
#include "fs.hpp"

#include <fuse.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <map>
#include <mutex>

#include "writeback.hpp"

namespace wm {

namespace {

const double TTL = 1.0;
const char* XATTR_SOURCE = "user.wavemorph.source";

std::shared_ptr<Library> g_lib;
uid_t g_uid;
gid_t g_gid;
int64_t g_started;

struct Node {
    enum Kind { Root, Dir, File } kind;
    fs::path dir;  // Dir: a source folder behind it (empty if purely virtual); File: the source folder it belongs to
    VFilePtr file;
    int64_t mtime = 0;
    std::string name, tag_section, tag_key, tag_ext;  // File
    std::string vpath;                                // Dir: its path in the mount
    bool editable() const { return !tag_section.empty(); }
};

/// An open file: its contents, plus a write session when opened for writing.
struct Handle {
    Node node;
    std::string path;
    std::unique_ptr<WriteSession> session;
};

// sessions by mount path, so stat() by name reports the size being written
std::mutex g_sessions_mu;
std::map<std::string, WriteSession*> g_sessions;

WriteSession* session_for(const std::string& path) {
    std::lock_guard g(g_sessions_mu);
    auto it = g_sessions.find(path);
    return it == g_sessions.end() ? nullptr : it->second;
}

timespec ts(int64_t ns) { return {time_t(ns / 1'000'000'000), long(ns % 1'000'000'000)}; }

/// Resolve a mount path ("/Music/Artist/Album/01 - x.flac") to a node through
/// the parent directory's listing (directories may be placed anywhere by sidecar
/// "_target"s, so the path says nothing about the source folders behind it).
std::optional<Node> resolve(const char* path) {
    std::string_view p(path);
    if (p == "/" || p.empty()) return Node{Node::Root, {}, nullptr, g_started};
    std::string parent, vpath;
    std::string last;
    for (auto& c : split(p.substr(1), '/')) {
        if (c.empty()) continue;
        if (starts_with(c, ".")) return std::nullopt;
        if (!last.empty()) parent = vpath;
        vpath = vpath.empty() ? c : vpath + "/" + c;
        last = c;
    }
    if (last.empty()) return Node{Node::Root, {}, nullptr, g_started};
    auto* ve = g_lib->list_vdir(parent)->find(last);
    if (!ve) return std::nullopt;
    auto& e = ve->e;
    if (e.is_dir) {
        Node n{Node::Dir, e.dir, nullptr, e.mtime};
        n.vpath = vpath;
        return n;
    }
    return Node{Node::File, ve->src_dir, e.file, e.mtime, e.name, e.tag_section, e.tag_key, e.tag_ext};
}

template <class F>
int guarded(const char* what, const char* path, F&& f) {
    try {
        return f();
    } catch (const std::exception& e) {
        warn("{} {}: {}", what, path, e.what());
        return -EIO;
    }
}

void fill_dir(struct stat* st, int64_t mtime) {
    std::memset(st, 0, sizeof *st);
    st->st_mode = S_IFDIR | 0555;
    st->st_nlink = 2;
    st->st_size = 4096;
    st->st_blocks = 8;
    st->st_blksize = 131072;
    st->st_uid = g_uid;
    st->st_gid = g_gid;
    st->st_mtim = st->st_atim = st->st_ctim = ts(mtime);
}

void fill_file(struct stat* st, uint64_t size, int64_t mtime, bool writable) {
    std::memset(st, 0, sizeof *st);
    st->st_mode = S_IFREG | (writable ? 0644 : 0444);
    st->st_nlink = 1;
    st->st_size = off_t(size);
    st->st_blocks = blkcnt_t((size + 511) / 512);
    st->st_blksize = 131072;
    st->st_uid = g_uid;
    st->st_gid = g_gid;
    st->st_mtim = st->st_atim = st->st_ctim = ts(mtime);
}

void* op_init(struct fuse_conn_info* conn, struct fuse_config* cfg) {
    cfg->entry_timeout = TTL;
    cfg->attr_timeout = TTL;
    cfg->negative_timeout = TTL;
    // keep the kernel's page cache across opens unless size or mtime changed
    // (both change when the content does: sidecar edits, new processing results)
    cfg->auto_cache = 1;
    // 1 MiB requests and read-ahead instead of 128 KiB: fewer round trips
    conn->max_write = 1u << 20;
    conn->max_readahead = 1u << 20;
    return nullptr;
}

int op_getattr(const char* path, struct stat* st, struct fuse_file_info* fi) {
    return guarded("getattr", path, [&] {
        if (fi && fi->fh) {
            auto* h = reinterpret_cast<Handle*>(fi->fh);
            uint64_t size = h->session ? h->session->size() : h->node.file->size();
            fill_file(st, size, h->node.mtime, h->node.editable());
            return 0;
        }
        auto n = resolve(path);
        if (!n) return -ENOENT;
        switch (n->kind) {
        case Node::Root: fill_dir(st, g_started); break;
        case Node::Dir: fill_dir(st, g_lib->list_vdir(n->vpath)->mtime); break;
        case Node::File: {
            auto* ws = session_for(path);
            fill_file(st, ws ? ws->size() : n->file->size(), n->mtime, n->editable());
            break;
        }
        }
        return 0;
    });
}

int op_readdir(const char* path, void* buf, fuse_fill_dir_t filler, off_t, struct fuse_file_info*, enum fuse_readdir_flags) {
    return guarded("readdir", path, [&] {
        auto n = resolve(path);
        if (!n) return -ENOENT;
        if (n->kind == Node::File) return -ENOTDIR;
        struct stat st;
        fill_dir(&st, 0);
        filler(buf, ".", &st, 0, fuse_fill_dir_flags(0));
        filler(buf, "..", &st, 0, fuse_fill_dir_flags(0));
        auto l = g_lib->list_vdir(n->kind == Node::Root ? std::string() : n->vpath);
        for (auto& ve : l->entries) {
            std::memset(&st, 0, sizeof st);
            st.st_mode = ve.e.is_dir ? S_IFDIR : S_IFREG;
            if (filler(buf, ve.e.name.c_str(), &st, 0, fuse_fill_dir_flags(0))) break;
        }
        return 0;
    });
}

/// Store what the editor changed in the sidecar. Returns 0 or -errno.
int commit(Handle& h) {
    if (!h.session || !h.session->dirty()) return 0;
    try {
        auto& n = h.node;
        auto& ws = *h.session;
        Tags before = read_file_tags(n.tag_ext, [&](uint64_t o, size_t l) { return n.file->read_at(o, l); }, n.file->size());
        Tags after = read_file_tags(n.tag_ext, [&](uint64_t o, size_t l) { return ws.read(o, l); }, ws.size());
        Tags changes = tag_changes(before, after);
        if (changes.empty()) {
            info("edit of {}: no tag changes", h.path);
        } else {
            g_lib->apply_edit(n.dir, n.name, n.tag_section, n.tag_key, changes);
            std::string what;
            for (auto& [k, v] : changes.m) what += (what.empty() ? "" : ", ") + k + (v.empty() ? " (removed)" : "");
            info("edit of {}: {} -> sidecar {}[{}]", h.path, what, n.tag_section, n.tag_key);
        }
        ws.mark_clean();
        return 0;
    } catch (const std::exception& e) {
        warn("edit of {} not stored: {}", h.path, e.what());
        return -EIO;
    }
}

int op_open(const char* path, struct fuse_file_info* fi) {
    return guarded("open", path, [&] {
        auto n = resolve(path);
        if (!n) return -ENOENT;
        if (n->kind != Node::File) return -EISDIR;
        bool writing = (fi->flags & O_ACCMODE) != O_RDONLY;
        if (writing && !n->editable()) return -EROFS;
        // the open file keeps the contents it was opened with
        auto h = std::make_unique<Handle>();
        h->node = *n;
        h->path = path;
        if (writing) {
            h->session = std::make_unique<WriteSession>(n->file, g_lib->cfg.cache_dir / "edits");
            if (fi->flags & O_TRUNC) h->session->truncate(0);
            std::lock_guard g(g_sessions_mu);
            g_sessions[path] = h->session.get();
            fi->direct_io = 1;  // sizes change while editing: bypass the page cache
        }
        fi->fh = reinterpret_cast<uint64_t>(h.release());
        return 0;
    });
}

int op_read(const char* path, char* buf, size_t size, off_t off, struct fuse_file_info* fi) {
    auto* h = reinterpret_cast<Handle*>(fi->fh);
    if (!h) return -EBADF;
    try {
        Bytes b = h->session ? h->session->read(uint64_t(off), size) : h->node.file->read_at(uint64_t(off), size);
        std::memcpy(buf, b.data(), b.size());
        return int(b.size());
    } catch (const std::exception& e) {
        warn("read {} @{}+{}: {}", h->node.file->describe(), off, size, e.what());
        return -EIO;
    }
}

int op_write(const char* path, const char* buf, size_t size, off_t off, struct fuse_file_info* fi) {
    auto* h = reinterpret_cast<Handle*>(fi->fh);
    if (!h || !h->session) return -EBADF;
    try {
        h->session->write(uint64_t(off), {reinterpret_cast<const uint8_t*>(buf), size});
        return int(size);
    } catch (const std::exception& e) {
        warn("write {}: {}", path, e.what());
        return -EIO;
    }
}

int op_truncate(const char* path, off_t size, struct fuse_file_info* fi) {
    WriteSession* ws = fi && fi->fh ? reinterpret_cast<Handle*>(fi->fh)->session.get() : session_for(path);
    if (!ws) {
        auto n = resolve(path);
        return n && n->kind == Node::File && n->editable() ? -EBUSY : -EROFS;  // only while open for writing
    }
    try {
        ws->truncate(uint64_t(size));
        return 0;
    } catch (const std::exception& e) {
        warn("truncate {}: {}", path, e.what());
        return -EIO;
    }
}

int op_flush(const char*, struct fuse_file_info* fi) {
    auto* h = reinterpret_cast<Handle*>(fi->fh);
    return h ? commit(*h) : 0;
}

int op_fsync(const char*, int, struct fuse_file_info* fi) { return op_flush(nullptr, fi); }

int op_release(const char*, struct fuse_file_info* fi) {
    auto* h = reinterpret_cast<Handle*>(fi->fh);
    if (!h) return 0;
    commit(*h);
    if (h->session) {
        std::lock_guard g(g_sessions_mu);
        if (auto it = g_sessions.find(h->path); it != g_sessions.end() && it->second == h->session.get()) g_sessions.erase(it);
    }
    delete h;
    fi->fh = 0;
    return 0;
}

// editors may restore times or modes after saving: accept and ignore
int op_utimens(const char*, const struct timespec*, struct fuse_file_info*) { return 0; }
int op_chmod(const char*, mode_t, struct fuse_file_info*) { return 0; }
int op_chown(const char*, uid_t, gid_t, struct fuse_file_info*) { return 0; }
// the tree itself is fixed
int op_create(const char*, mode_t, struct fuse_file_info*) { return -EROFS; }
int op_unlink(const char*) { return -EROFS; }
int op_rename(const char*, const char*, unsigned int) { return -EROFS; }
int op_mkdir(const char*, mode_t) { return -EROFS; }

int op_statfs(const char*, struct statvfs* out) {
    std::memset(out, 0, sizeof *out);
    out->f_bsize = out->f_frsize = 4096;
    out->f_namemax = 255;
    if (g_lib->cfg.roots.empty()) return 0;
    struct statvfs s;
    if (::statvfs(g_lib->cfg.roots[0].path.c_str(), &s) == 0) {
        out->f_bsize = s.f_bsize;
        out->f_frsize = s.f_frsize;
        out->f_blocks = s.f_blocks;
        out->f_files = s.f_files;
    }
    return 0;
}

int reply_bytes(const std::string& v, char* value, size_t size) {
    if (size == 0) return int(v.size());
    if (size < v.size()) return -ERANGE;
    std::memcpy(value, v.data(), v.size());
    return int(v.size());
}

int op_getxattr(const char* path, const char* name, char* value, size_t size) {
    if (std::strcmp(name, XATTR_SOURCE) != 0) return -ENODATA;
    return guarded("getxattr", path, [&] {
        auto n = resolve(path);
        if (!n) return -ENOENT;
        if (n->kind == Node::Root) return -ENODATA;
        if (n->kind == Node::File) return reply_bytes(n->file->describe(), value, size);
        return reply_bytes(n->dir.empty() ? "virtual:" + n->vpath : "dir:" + n->dir.string(), value, size);
    });
}

int op_listxattr(const char*, char* list, size_t size) { return reply_bytes(std::string(XATTR_SOURCE) + '\0', list, size); }

}  // namespace

int mount_fs(std::shared_ptr<Library> lib, const MountOptions& opt) {
    g_lib = std::move(lib);
    g_uid = ::getuid();
    g_gid = ::getgid();
    g_started = now_ns();

    struct fuse_operations ops = {};
    ops.init = op_init;
    ops.getattr = op_getattr;
    ops.readdir = op_readdir;
    ops.open = op_open;
    ops.read = op_read;
    ops.write = op_write;
    ops.truncate = op_truncate;
    ops.flush = op_flush;
    ops.fsync = op_fsync;
    ops.release = op_release;
    ops.utimens = op_utimens;
    ops.chmod = op_chmod;
    ops.chown = op_chown;
    ops.create = op_create;
    ops.unlink = op_unlink;
    ops.rename = op_rename;
    ops.mkdir = op_mkdir;
    ops.statfs = op_statfs;
    ops.getxattr = op_getxattr;
    ops.listxattr = op_listxattr;

    // writable: tag edits are captured into sidecars (see writeback.hpp)
    std::string o = "noatime,fsname=wavemorphfs,subtype=wavemorphfs";
    if (opt.allow_other) o += ",allow_other";
    std::vector<std::string> args = {"wavemorphfs", "-o", o};
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    struct fuse_args fargs = FUSE_ARGS_INIT(int(argv.size()), argv.data());
    struct fuse* f = fuse_new(&fargs, &ops, sizeof ops, nullptr);
    if (!f) {
        warn("fuse_new failed");
        return 1;
    }
    if (fuse_mount(f, opt.mountpoint.c_str()) != 0) {
        warn("mounting at {} failed", opt.mountpoint);
        fuse_destroy(f);
        return 1;
    }
    fuse_set_signal_handlers(fuse_get_session(f));
    info("mounted at {}", opt.mountpoint);
    struct fuse_loop_config* lc = fuse_loop_cfg_create();
    fuse_loop_cfg_set_max_threads(lc, unsigned(opt.threads));
    fuse_loop_cfg_set_idle_threads(lc, unsigned(std::min<size_t>(opt.threads, 4)));
    int rc = fuse_loop_mt(f, lc);
    fuse_loop_cfg_destroy(lc);
    fuse_remove_signal_handlers(fuse_get_session(f));
    fuse_unmount(f);
    fuse_destroy(f);
    info("unmounted");
    return rc == 0 ? 0 : 1;
}

}  // namespace wm
