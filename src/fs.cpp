// SPDX-License-Identifier: GPL-3.0-or-later
#include "fs.hpp"

#include <fuse.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

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
    fs::path dir;  // Dir: source path
    VFilePtr file;
    int64_t mtime = 0;
};

timespec ts(int64_t ns) { return {time_t(ns / 1'000'000'000), long(ns % 1'000'000'000)}; }

/// Resolve a mount path ("/Music/Artist/Album/01 - x.flac") to a node.
/// Intermediate components are real directories (never hidden unless dotfiles),
/// so only the last component needs the parent's listing.
std::optional<Node> resolve(const char* path) {
    std::string_view p(path);
    if (p == "/" || p.empty()) return Node{Node::Root, {}, nullptr, g_started};
    std::vector<std::string> comps;
    for (auto& c : split(p.substr(1), '/'))
        if (!c.empty()) comps.push_back(c);
    const Root* root = nullptr;
    for (auto& r : g_lib->cfg.roots)
        if (r.name == comps[0]) root = &r;
    if (!root) return std::nullopt;
    fs::path cur = root->path;
    if (comps.size() == 1) return Node{Node::Dir, cur, nullptr, 0};
    for (size_t i = 1; i + 1 < comps.size(); i++) {
        if (starts_with(comps[i], ".")) return std::nullopt;
        cur /= comps[i];
    }
    std::error_code ec;
    if (!fs::is_directory(cur, ec)) return std::nullopt;
    auto l = g_lib->list_dir(cur);
    auto* e = l->find(comps.back());
    if (!e) return std::nullopt;
    if (e->is_dir) return Node{Node::Dir, e->dir, nullptr, e->mtime};
    return Node{Node::File, {}, e->file, e->mtime};
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

void fill_file(struct stat* st, uint64_t size, int64_t mtime) {
    std::memset(st, 0, sizeof *st);
    st->st_mode = S_IFREG | 0444;
    st->st_nlink = 1;
    st->st_size = off_t(size);
    st->st_blocks = blkcnt_t((size + 511) / 512);
    st->st_blksize = 131072;
    st->st_uid = g_uid;
    st->st_gid = g_gid;
    st->st_mtim = st->st_atim = st->st_ctim = ts(mtime);
}

void* op_init(struct fuse_conn_info*, struct fuse_config* cfg) {
    cfg->entry_timeout = TTL;
    cfg->attr_timeout = TTL;
    cfg->negative_timeout = TTL;
    cfg->kernel_cache = 0;
    return nullptr;
}

int op_getattr(const char* path, struct stat* st, struct fuse_file_info*) {
    return guarded("getattr", path, [&] {
        auto n = resolve(path);
        if (!n) return -ENOENT;
        switch (n->kind) {
        case Node::Root: fill_dir(st, g_started); break;
        case Node::Dir: fill_dir(st, g_lib->list_dir(n->dir)->mtime); break;
        case Node::File: fill_file(st, n->file->size(), n->mtime); break;
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
        if (n->kind == Node::Root) {
            for (auto& r : g_lib->cfg.roots) filler(buf, r.name.c_str(), &st, 0, fuse_fill_dir_flags(0));
            return 0;
        }
        auto l = g_lib->list_dir(n->dir);
        for (auto& e : l->entries) {
            std::memset(&st, 0, sizeof st);
            st.st_mode = e.is_dir ? S_IFDIR : S_IFREG;
            if (filler(buf, e.name.c_str(), &st, 0, fuse_fill_dir_flags(0))) break;
        }
        return 0;
    });
}

int op_open(const char* path, struct fuse_file_info* fi) {
    if ((fi->flags & O_ACCMODE) != O_RDONLY) return -EROFS;
    return guarded("open", path, [&] {
        auto n = resolve(path);
        if (!n) return -ENOENT;
        if (n->kind != Node::File) return -EISDIR;
        // the open file keeps the contents it was opened with
        fi->fh = reinterpret_cast<uint64_t>(new VFilePtr(n->file));
        return 0;
    });
}

int op_read(const char* path, char* buf, size_t size, off_t off, struct fuse_file_info* fi) {
    auto* vf = reinterpret_cast<VFilePtr*>(fi->fh);
    if (!vf) return -EBADF;
    try {
        Bytes b = (*vf)->read_at(uint64_t(off), size);
        std::memcpy(buf, b.data(), b.size());
        return int(b.size());
    } catch (const std::exception& e) {
        warn("read {} @{}+{}: {}", (*vf)->describe(), off, size, e.what());
        return -EIO;
    }
}

int op_release(const char*, struct fuse_file_info* fi) {
    delete reinterpret_cast<VFilePtr*>(fi->fh);
    fi->fh = 0;
    return 0;
}

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
        return reply_bytes(n->kind == Node::File ? n->file->describe() : "dir:" + n->dir.string(), value, size);
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
    ops.release = op_release;
    ops.statfs = op_statfs;
    ops.getxattr = op_getxattr;
    ops.listxattr = op_listxattr;

    std::string o = "ro,noatime,fsname=wavemorphfs,subtype=wavemorphfs";
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
