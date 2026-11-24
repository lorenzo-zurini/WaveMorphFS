// SPDX-License-Identifier: GPL-3.0-or-later
#include "nfs.hpp"

#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <thread>
#include <unordered_map>

#include "md5.hpp"
#include "vtree.hpp"
#include "writeback.hpp"
#include "xdr.hpp"

namespace wm {

namespace {

// ONC RPC (RFC 5531)
constexpr uint32_t RPC_CALL = 0, RPC_REPLY = 1;
constexpr uint32_t MSG_ACCEPTED = 0, MSG_DENIED = 1;
constexpr uint32_t SUCCESS = 0, PROG_UNAVAIL = 1, PROG_MISMATCH = 2, PROC_UNAVAIL = 3, GARBAGE_ARGS = 4;
constexpr uint32_t RPC_MISMATCH = 0, AUTH_ERROR = 1, AUTH_BADCRED = 1;
constexpr uint32_t AUTH_NONE = 0, AUTH_SYS = 1;
constexpr uint32_t PROG_NFS = 100003, PROG_MOUNT = 100005;
constexpr uint32_t NOBODY = 65534;

// NFS version 3 (RFC 1813) and MOUNT version 3 (its appendix I)
enum Proc : uint32_t {
    NULLPROC, GETATTR, SETATTR, LOOKUP, ACCESS, READLINK, READ, WRITE, CREATE, MKDIR, SYMLINK,
    MKNOD, REMOVE, RMDIR, RENAME, LINK, READDIR, READDIRPLUS, FSSTAT, FSINFO, PATHCONF, COMMIT,
};
enum Status : uint32_t {
    NFS3_OK = 0, ERR_NOENT = 2, ERR_IO = 5, ERR_ACCES = 13, ERR_NOTDIR = 20, ERR_ISDIR = 21, ERR_INVAL = 22,
    ERR_FBIG = 27, ERR_ROFS = 30, ERR_NAMETOOLONG = 63, ERR_STALE = 70, ERR_BADHANDLE = 10001, ERR_TOOSMALL = 10005,
};
enum MountProc : uint32_t { MNT_NULL, MNT, MNT_DUMP, MNT_UMNT, MNT_UMNTALL, MNT_EXPORT };
constexpr uint32_t ACCESS_READ = 0x01, ACCESS_LOOKUP = 0x02, ACCESS_MODIFY = 0x04, ACCESS_EXTEND = 0x08, ACCESS_EXECUTE = 0x20;
constexpr uint32_t UNSTABLE = 0, FILE_SYNC = 2;
constexpr uint32_t FSF3_HOMOGENEOUS = 0x08, FSF3_CANSETTIME = 0x10;

constexpr uint32_t MAX_IO = 1u << 20;              // read and write size offered to clients
constexpr size_t MAX_RECORD = MAX_IO + 64 * 1024;  // a WRITE call with its header
constexpr size_t MAX_INFLIGHT = 64;                // requests of one connection queued or in progress
constexpr size_t MAX_CONNECTIONS = 128;
constexpr size_t MAX_NAME = 255, MAX_PATH = 1024;
constexpr uint64_t MAX_GROWTH = 1ull << 30;  // how far an edit may write past the file's end
constexpr auto ABANDON = std::chrono::minutes(10);  // an edit that never became a complete file
constexpr uint64_t FSID = 0x5761'7665'4d6f'7270;

/// A request answered with an NFS error status.
struct NfsError {
    Status status;
};

using Hash = std::array<uint8_t, 16>;
struct HashKey {
    size_t operator()(const Hash& h) const { return size_t(be64(h.data())); }
};
Hash path_hash(std::string_view vpath) {
    Md5 m;
    m.update(vpath);
    return m.finish();
}
uint64_t fileid_of(std::string_view vpath) { return be64(path_hash(vpath).data()) | 1; }  // never 0

constexpr size_t FH_SIZE = 20;
using Fh = std::array<uint8_t, FH_SIZE>;

/// File handles: "WM", a format version, 0, then the MD5 of the mount path. The
/// paths behind the handles handed out are appended to a file in the cache
/// directory, so that handles stay valid when the server restarts.
class Handles {
public:
    explicit Handles(fs::path file) : file_(std::move(file)) {
        paths_.emplace(path_hash(""), "");
        load();
    }
    ~Handles() {
        if (fd_ >= 0) ::close(fd_);
    }
    Fh handle(const std::string& vpath) {
        Hash h = path_hash(vpath);
        Fh fh{'W', 'M', 1, 0};
        std::memcpy(fh.data() + 4, h.data(), h.size());
        {
            std::shared_lock g(mu_);
            if (paths_.contains(h)) return fh;
        }
        std::lock_guard g(mu_);
        if (paths_.emplace(h, vpath).second && fd_ >= 0) {
            Bytes rec;
            put_be32(rec, uint32_t(vpath.size()));
            append(rec, vpath);
            // one write with O_APPEND: whole records even with concurrent writers
            if (::write(fd_, rec.data(), rec.size()) != ssize_t(rec.size())) warn("{}: {}", file_.string(), std::strerror(errno));
        }
        return fh;
    }
    /// The path behind `fh`; nullopt for a handle this server never handed out
    /// (stale). Throws NfsError for something that is not one of its handles.
    std::optional<std::string> path(std::span<const uint8_t> fh) const {
        if (fh.size() != FH_SIZE || fh[0] != 'W' || fh[1] != 'M' || fh[2] != 1) throw NfsError{ERR_BADHANDLE};
        Hash h;
        std::memcpy(h.data(), fh.data() + 4, h.size());
        std::shared_lock g(mu_);
        auto it = paths_.find(h);
        if (it == paths_.end()) return std::nullopt;
        return it->second;
    }

private:
    void load() {
        std::error_code ec;
        fs::create_directories(file_.parent_path(), ec);
        if (auto b = try_read_file(file_)) {
            size_t p = 0, n = 0;
            while (p + 4 <= b->size()) {
                size_t len = be32(b->data() + p);
                if (len > MAX_PATH * 4 || p + 4 + len > b->size()) break;  // a torn last record
                std::string v(str_of(std::span(*b).subspan(p + 4, len)));
                paths_.emplace(path_hash(v), std::move(v));
                p += 4 + len;
                n++;
            }
            if (p != b->size()) {
                warn("{}: ignoring a damaged end after {} handles", file_.string(), n);
                if (::truncate(file_.c_str(), off_t(p)) != 0) warn("{}: {}", file_.string(), std::strerror(errno));
            }
        }
        fd_ = ::open(file_.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
        if (fd_ < 0) warn("{}: {} (file handles will not survive a restart)", file_.string(), std::strerror(errno));
    }
    fs::path file_;
    int fd_ = -1;
    mutable std::shared_mutex mu_;
    std::unordered_map<Hash, std::string, HashKey> paths_;
};

/// A tag edit in progress (see nfs.hpp).
struct Edit {
    Node node;
    std::shared_ptr<WriteSession> ws;
    int64_t changed = 0;  // last write, ns; reported as the file's mtime
    int64_t used = 0;     // last read or write, ns
    uint64_t failed = UINT64_MAX;  // change count at which storing it failed
};

/// What the caller claims to be (AUTH_SYS), trusted because it comes from a
/// reserved port.
struct Cred {
    uint32_t uid = NOBODY, gid = NOBODY;
};

struct Conn {
    explicit Conn(int f) : fd(f) {}
    ~Conn() { ::close(fd); }
    int fd;
    std::mutex write_mu;
    std::mutex mu;
    std::condition_variable cv;
    size_t inflight = 0;
};

struct Request {
    std::shared_ptr<Conn> conn;
    Bytes msg;
};

/// A reply: `head`, then `data` padded (the payload of a READ, kept apart to
/// avoid copying it).
struct Reply {
    xdr::Out head;
    Bytes data;
};

bool read_full(int fd, uint8_t* p, size_t n) {
    while (n) {
        ssize_t r = ::read(fd, p, n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return false;
        p += r;
        n -= size_t(r);
    }
    return true;
}

timespec ts(int64_t ns) { return {time_t(ns / 1'000'000'000), long(ns % 1'000'000'000)}; }

struct Attr {
    bool dir = false, writable = false;
    uint64_t size = 0;
    int64_t mtime = 0;
    uint64_t fileid = 0;
};

void wcc_none(xdr::Out& o) {
    o.boolean(false);  // pre_op_attr
    o.boolean(false);  // post_op_attr
}

/// The result of a failed call: its status and whatever the procedure's failure
/// arm carries, left empty.
void fail_body(uint32_t proc, Status s, xdr::Out& o) {
    o.u32(s);
    switch (proc) {
    case GETATTR: break;
    case SETATTR: case WRITE: case CREATE: case MKDIR: case SYMLINK: case MKNOD: case REMOVE: case RMDIR: case COMMIT:
        wcc_none(o);
        break;
    case RENAME:
        wcc_none(o);
        wcc_none(o);
        break;
    case LINK:
        o.boolean(false);
        wcc_none(o);
        break;
    default: o.boolean(false);  // post_op_attr
    }
}

std::string parent_of(const std::string& vpath) {
    auto slash = vpath.rfind('/');
    return slash == std::string::npos ? "" : vpath.substr(0, slash);
}
std::string child_of(const std::string& dir, std::string_view name) { return dir.empty() ? std::string(name) : dir + "/" + std::string(name); }

}  // namespace

struct NfsServer::Impl {
    Impl(std::shared_ptr<Library> l, NfsOptions o)
        : lib(std::move(l)), opt(std::move(o)), handles(lib->cfg.cache_dir / "nfs-handles"), uid(::getuid()), gid(::getgid()), started(now_ns()) {
        uint64_t v = uint64_t(started);
        for (int i = 0; i < 8; i++) verf[size_t(i)] = uint8_t(v >> (8 * i));
    }

    std::shared_ptr<Library> lib;
    NfsOptions opt;
    Handles handles;
    uint32_t uid, gid;
    int64_t started;
    std::array<uint8_t, 8> verf{};  // write verifier: changes when the server restarts

    int listen_fd = -1;
    uint16_t port = 0;
    std::atomic<bool> stopping{false};
    bool started_threads = false, stopped = false;
    std::thread acceptor, committer;
    std::vector<std::thread> workers;

    std::mutex conns_mu;
    std::condition_variable conns_cv;
    std::vector<std::weak_ptr<Conn>> conns;
    size_t readers = 0;
    std::set<std::string> refused;  // peers told off once about their port

    std::mutex q_mu;
    std::condition_variable q_cv;
    std::deque<Request> queue;

    std::mutex edits_mu;
    std::condition_variable edits_cv;
    std::map<std::string, std::shared_ptr<Edit>> edits;  // by mount path

    // ---- connections

    void start() {
        auto [host, svc] = split_listen(opt.listen);
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;
        addrinfo* res = nullptr;
        int rc = ::getaddrinfo(host.empty() ? nullptr : host.c_str(), svc.c_str(), &hints, &res);
        if (rc != 0) fail("listen address {}: {}", opt.listen, ::gai_strerror(rc));
        std::string err = "no address";
        for (auto* a = res; a && listen_fd < 0; a = a->ai_next) {
            int fd = ::socket(a->ai_family, a->ai_socktype | SOCK_CLOEXEC, a->ai_protocol);
            if (fd < 0) continue;
            int one = 1;
            ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
            if (::bind(fd, a->ai_addr, a->ai_addrlen) == 0 && ::listen(fd, 64) == 0) {
                listen_fd = fd;
            } else {
                err = std::strerror(errno);
                ::close(fd);
            }
        }
        ::freeaddrinfo(res);
        if (listen_fd < 0) fail("listening on {}: {}", opt.listen, err);
        sockaddr_storage sa{};
        socklen_t len = sizeof sa;
        ::getsockname(listen_fd, reinterpret_cast<sockaddr*>(&sa), &len);
        port = port_of(sa);
        started_threads = true;
        for (size_t i = 0; i < std::max<size_t>(opt.threads, 2); i++) workers.emplace_back([this] { work(); });
        committer = std::thread([this] { commit_loop(); });
        acceptor = std::thread([this] { accept_loop(); });
        info("serving NFS on port {}{}", port, opt.insecure ? " (clients on any port)" : "");
    }

    static std::pair<std::string, std::string> split_listen(const std::string& s) {
        if (starts_with(s, "[")) {
            auto close = s.find("]:");
            if (close == std::string::npos) fail("listen address {}: expected [ADDRESS]:PORT", s);
            return {s.substr(1, close - 1), s.substr(close + 2)};
        }
        auto colon = s.rfind(':');
        if (colon == std::string::npos) return {"", s};
        return {s.substr(0, colon), s.substr(colon + 1)};
    }

    static uint16_t port_of(const sockaddr_storage& sa) {
        if (sa.ss_family == AF_INET) return ntohs(reinterpret_cast<const sockaddr_in&>(sa).sin_port);
        if (sa.ss_family == AF_INET6) return ntohs(reinterpret_cast<const sockaddr_in6&>(sa).sin6_port);
        return 0;
    }

    static std::string addr_of(const sockaddr_storage& sa) {
        char host[NI_MAXHOST] = "?";
        ::getnameinfo(reinterpret_cast<const sockaddr*>(&sa), sizeof sa, host, sizeof host, nullptr, 0, NI_NUMERICHOST);
        return host;
    }

    void accept_loop() {
        while (!stopping) {
            sockaddr_storage sa{};
            socklen_t len = sizeof sa;
            int fd = ::accept4(listen_fd, reinterpret_cast<sockaddr*>(&sa), &len, SOCK_CLOEXEC);
            if (fd < 0) {
                if (stopping) break;
                if (errno != EINTR && errno != ECONNABORTED) {
                    warn("NFS accept: {}", std::strerror(errno));
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));  // e.g. out of descriptors
                }
                continue;
            }
            std::string peer = addr_of(sa);
            if (!opt.insecure && port_of(sa) >= 1024) {
                std::lock_guard g(conns_mu);
                if (refused.insert(peer).second)
                    warn("NFS: refusing {} port {}: clients must connect from a reserved port (as the kernel's NFS client does)", peer, port_of(sa));
                ::close(fd);
                continue;
            }
            int one = 1;
            ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
            ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);
            auto c = std::make_shared<Conn>(fd);
            {
                std::lock_guard g(conns_mu);
                std::erase_if(conns, [](auto& w) { return w.expired(); });
                if (conns.size() >= MAX_CONNECTIONS || stopping) {
                    warn("NFS: refusing {}: too many connections", peer);
                    continue;  // c closes it
                }
                conns.push_back(c);
                readers++;
            }
            debug("NFS: connection from {}", peer);
            std::thread([this, c, peer] { read_loop(c, peer); }).detach();
        }
    }

    void read_loop(std::shared_ptr<Conn> c, std::string peer) {
        for (;;) {
            Bytes msg;
            bool last = false, ok = true;
            while (!last && ok) {
                uint8_t h[4];
                if (!(ok = read_full(c->fd, h, 4))) break;
                uint32_t mark = be32(h);
                last = mark >> 31;
                size_t n = mark & 0x7fff'ffff, at = msg.size();
                if (at + n > MAX_RECORD) {
                    warn("NFS: {} sent a message over {} bytes; closing the connection", peer, MAX_RECORD);
                    ok = false;
                    break;
                }
                msg.resize(at + n);
                ok = read_full(c->fd, msg.data() + at, n);
            }
            if (!ok) break;
            {
                std::unique_lock g(c->mu);
                c->cv.wait(g, [&] { return c->inflight < MAX_INFLIGHT || stopping; });
                if (stopping) break;
                c->inflight++;
            }
            {
                std::lock_guard g(q_mu);
                queue.push_back({c, std::move(msg)});
            }
            q_cv.notify_one();
        }
        ::shutdown(c->fd, SHUT_RDWR);  // replies still being made are dropped
        debug("NFS: {} disconnected", peer);
        std::lock_guard g(conns_mu);
        readers--;
        conns_cv.notify_all();
    }

    void work() {
        for (;;) {
            Request r;
            {
                std::unique_lock g(q_mu);
                q_cv.wait(g, [&] { return !queue.empty() || stopping; });
                if (queue.empty()) return;
                r = std::move(queue.front());
                queue.pop_front();
            }
            auto reply = handle(r.msg);
            if (reply) send(*r.conn, *reply);
            else ::shutdown(r.conn->fd, SHUT_RDWR);  // not RPC: drop the connection
            std::lock_guard g(r.conn->mu);
            r.conn->inflight--;
            r.conn->cv.notify_one();
        }
    }

    static void send(Conn& c, const Reply& r) {
        static const uint8_t zeros[4] = {};
        size_t pad = r.data.empty() ? 0 : xdr::padded(r.data.size()) - r.data.size();
        uint8_t mark[4];
        uint32_t total = uint32_t(r.head.b.size() + r.data.size() + pad);
        mark[0] = uint8_t(0x80 | total >> 24), mark[1] = uint8_t(total >> 16), mark[2] = uint8_t(total >> 8), mark[3] = uint8_t(total);
        iovec iov[4] = {{mark, 4}, {const_cast<uint8_t*>(r.head.b.data()), r.head.b.size()},
                        {const_cast<uint8_t*>(r.data.data()), r.data.size()}, {const_cast<uint8_t*>(zeros), pad}};
        std::lock_guard g(c.write_mu);
        msghdr m{};
        m.msg_iov = iov;
        m.msg_iovlen = 4;
        while (m.msg_iovlen) {
            ssize_t n = ::sendmsg(c.fd, &m, MSG_NOSIGNAL);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) {
                ::shutdown(c.fd, SHUT_RDWR);
                return;
            }
            // skip what was sent
            while (m.msg_iovlen && size_t(n) >= m.msg_iov->iov_len) {
                n -= ssize_t(m.msg_iov->iov_len);
                m.msg_iov++;
                m.msg_iovlen--;
            }
            if (m.msg_iovlen) {
                m.msg_iov->iov_base = static_cast<uint8_t*>(m.msg_iov->iov_base) + n;
                m.msg_iov->iov_len -= size_t(n);
            }
        }
    }

    void stop() {
        if (stopped || !started_threads) {
            stopped = true;
            return;
        }
        stopped = true;
        stopping = true;
        ::shutdown(listen_fd, SHUT_RDWR);  // wakes accept()
        acceptor.join();
        ::close(listen_fd);
        {
            std::unique_lock g(conns_mu);
            for (auto& w : conns)
                if (auto c = w.lock()) {
                    ::shutdown(c->fd, SHUT_RDWR);
                    std::lock_guard cg(c->mu);
                    c->cv.notify_all();
                }
            conns_cv.wait(g, [&] { return readers == 0; });
        }
        q_cv.notify_all();
        for (auto& t : workers) t.join();
        edits_cv.notify_all();
        committer.join();
        sweep(true);  // store what is still pending
        info("NFS server stopped");
    }

    // ---- RPC

    std::optional<Reply> handle(const Bytes& msg) {
        xdr::In in(msg);
        Reply r;
        auto& o = r.head;
        uint32_t xid, prog, vers, proc;
        Cred cred;
        bool cred_ok = true;
        try {
            xid = in.u32();
            if (in.u32() != RPC_CALL) return std::nullopt;
            o.u32(xid);
            o.u32(RPC_REPLY);
            if (in.u32() != 2) {
                o.u32(MSG_DENIED);
                o.u32(RPC_MISMATCH);
                o.u32(2);
                o.u32(2);
                return r;
            }
            prog = in.u32(), vers = in.u32(), proc = in.u32();
            uint32_t flavor = in.u32();
            auto body = in.opaque(400);
            in.u32();  // verifier
            in.opaque(400);
            if (flavor == AUTH_SYS) {
                try {
                    xdr::In c(body);
                    c.u32();  // stamp
                    c.opaque(255);  // machine name
                    cred.uid = c.u32();
                    cred.gid = c.u32();
                    uint32_t n = c.u32();
                    if (n > 16) throw xdr::Error("too many groups");
                    for (uint32_t i = 0; i < n; i++) c.u32();
                } catch (const xdr::Error&) {
                    cred_ok = false;
                }
            } else if (flavor != AUTH_NONE) {
                cred_ok = false;
            }
        } catch (const xdr::Error&) {
            return std::nullopt;
        }
        if (!cred_ok) {
            o.u32(MSG_DENIED);
            o.u32(AUTH_ERROR);
            o.u32(AUTH_BADCRED);
            return r;
        }
        o.u32(MSG_ACCEPTED);
        o.u32(AUTH_NONE);  // our verifier
        o.u32(0);
        if (prog != PROG_NFS && prog != PROG_MOUNT) {
            o.u32(PROG_UNAVAIL);
            return r;
        }
        if (vers != 3) {
            o.u32(PROG_MISMATCH);
            o.u32(3);
            o.u32(3);
            return r;
        }
        if (prog == PROG_NFS ? proc > COMMIT : proc > MNT_EXPORT) {
            o.u32(PROC_UNAVAIL);
            return r;
        }
        Reply body;
        try {
            if (prog == PROG_NFS) nfs(proc, in, body, cred);
            else mount(proc, in, body);
        } catch (const xdr::Error&) {
            o.u32(GARBAGE_ARGS);
            return r;
        } catch (const NfsError& e) {
            body = {};
            fail_body(proc, e.status, body.head);
        } catch (const std::exception& e) {
            warn("NFS {}: {}", proc_name(prog, proc), e.what());
            body = {};
            if (prog == PROG_NFS) fail_body(proc, ERR_IO, body.head);
            else body.head.u32(ERR_IO);
        }
        o.u32(SUCCESS);
        append(o.b, body.head.b);
        r.data = std::move(body.data);
        return r;
    }

    static std::string proc_name(uint32_t prog, uint32_t proc) {
        static const char* nfs[] = {"NULL", "GETATTR", "SETATTR", "LOOKUP", "ACCESS", "READLINK", "READ", "WRITE",
                                    "CREATE", "MKDIR", "SYMLINK", "MKNOD", "REMOVE", "RMDIR", "RENAME", "LINK",
                                    "READDIR", "READDIRPLUS", "FSSTAT", "FSINFO", "PATHCONF", "COMMIT"};
        static const char* mnt[] = {"NULL", "MNT", "DUMP", "UMNT", "UMNTALL", "EXPORT"};
        if (prog == PROG_NFS && proc <= COMMIT) return nfs[proc];
        if (prog == PROG_MOUNT && proc <= MNT_EXPORT) return std::string("MOUNT ") + mnt[proc];
        return std::format("{}/{}", prog, proc);
    }

    // ---- the tree

    Node node_of(xdr::In& in) {
        auto p = handles.path(in.opaque(64));
        if (!p) throw NfsError{ERR_STALE};
        auto n = resolve(*lib, *p);
        if (!n) throw NfsError{ERR_STALE};
        return std::move(*n);
    }

    Attr attr(const Node& n) {
        Attr a;
        a.fileid = fileid_of(n.vpath);
        if (n.kind != Node::File) {
            a.dir = true;
            a.size = 4096;
            a.mtime = dir_mtime(*lib, n, started);
            return a;
        }
        a.writable = n.editable();
        {
            std::lock_guard g(edits_mu);
            if (auto it = edits.find(n.vpath); it != edits.end()) {
                a.size = it->second->ws->size();
                a.mtime = it->second->changed;
                return a;
            }
        }
        a.size = n.ve.e.file->size();
        a.mtime = n.ve.e.mtime;
        return a;
    }

    void put_attr(xdr::Out& o, const Attr& a) const {
        o.u32(a.dir ? 2 : 1);  // NF3DIR, NF3REG
        o.u32(a.dir ? 0555 : a.writable ? 0644 : 0444);
        o.u32(a.dir ? 2 : 1);  // links
        o.u32(uid);
        o.u32(gid);
        o.u64(a.size);
        o.u64(a.size);  // used
        o.u32(0);  // rdev
        o.u32(0);
        o.u64(FSID);
        o.u64(a.fileid);
        auto t = ts(a.mtime);
        for (int i = 0; i < 3; i++) {  // atime, mtime, ctime
            o.u32(uint32_t(t.tv_sec));
            o.u32(uint32_t(t.tv_nsec));
        }
    }
    void post_op(xdr::Out& o, const Node& n) {
        o.boolean(true);
        put_attr(o, attr(n));
    }
    void wcc(xdr::Out& o, const Node& n) {
        o.boolean(false);  // no pre-op attributes: clients take the change for their own
        post_op(o, n);
    }

    bool may_edit(const Cred& c) const { return c.uid == uid || c.uid == 0; }

    /// The edit of file `n`, started if need be, for a change about to be made.
    std::shared_ptr<Edit> edit_of(const Node& n, const Cred& c) {
        if (n.kind != Node::File) throw NfsError{ERR_ISDIR};
        if (!n.editable()) throw NfsError{ERR_ROFS};
        if (!may_edit(c)) throw NfsError{ERR_ACCES};
        std::lock_guard g(edits_mu);
        auto& e = edits[n.vpath];
        if (!e) {
            e = std::make_shared<Edit>();
            e->node = n;
            e->ws = std::make_shared<WriteSession>(n.ve.e.file, lib->cfg.cache_dir / "edits");
        }
        // every change moves the mtime, so that other clients drop what they cached
        e->changed = e->used = std::max(now_ns(), e->changed + 1);
        return e;
    }

    std::shared_ptr<WriteSession> session_for_read(const std::string& vpath) {
        std::lock_guard g(edits_mu);
        auto it = edits.find(vpath);
        if (it == edits.end()) return nullptr;
        it->second->used = now_ns();
        return it->second->ws;
    }

    // ---- NFS procedures

    void nfs(uint32_t proc, xdr::In& in, Reply& r, const Cred& cred) {
        auto& o = r.head;
        switch (proc) {
        case NULLPROC: return;
        case GETATTR: {
            Node n = node_of(in);
            o.u32(NFS3_OK);
            put_attr(o, attr(n));
            return;
        }
        case SETATTR: {
            Node n = node_of(in);
            if (in.boolean()) in.u32();  // mode
            if (in.boolean()) in.u32();  // uid
            if (in.boolean()) in.u32();  // gid
            std::optional<uint64_t> size;
            if (in.boolean()) size = in.u64();
            for (int i = 0; i < 2; i++) {  // atime, mtime
                uint32_t how = in.u32();
                if (how == 2) in.fixed(8);
                else if (how > 2) throw xdr::Error("bad time_how");
            }
            if (in.boolean()) in.fixed(8);  // guard
            // modes, owners and times: editors may restore them after saving; accepted and ignored
            if (size) {
                if (n.kind != Node::File) throw NfsError{ERR_ISDIR};
                bool same = !session_for_read(n.vpath) && *size == n.ve.e.file->size();
                if (!same) edit_of(n, cred)->ws->truncate(*size);
            }
            o.u32(NFS3_OK);
            wcc(o, n);
            return;
        }
        case LOOKUP: {
            Node d = node_of(in);
            std::string name = in.str(MAX_PATH);
            if (d.kind == Node::File) throw NfsError{ERR_NOTDIR};
            if (name.size() > MAX_NAME) throw NfsError{ERR_NAMETOOLONG};
            std::optional<Node> c;
            if (name == ".") c = d;
            else if (name == "..") c = resolve(*lib, parent_of(d.vpath));
            else if (!name.empty() && name.find('/') == std::string::npos && name.find('\0') == std::string::npos)
                c = resolve(*lib, child_of(d.vpath, name));
            if (!c) {
                o.u32(ERR_NOENT);
                post_op(o, d);
                return;
            }
            o.u32(NFS3_OK);
            o.opaque(handles.handle(c->vpath));
            post_op(o, *c);
            post_op(o, d);
            return;
        }
        case ACCESS: {
            Node n = node_of(in);
            uint32_t want = in.u32(), can = ACCESS_READ;
            if (n.kind != Node::File) can |= ACCESS_LOOKUP | ACCESS_EXECUTE;
            else if (n.editable() && may_edit(cred)) can |= ACCESS_MODIFY | ACCESS_EXTEND;
            o.u32(NFS3_OK);
            post_op(o, n);
            o.u32(want & can);
            return;
        }
        case READLINK: node_of(in); throw NfsError{ERR_INVAL};
        case READ: {
            Node n = node_of(in);
            uint64_t off = in.u64();
            uint32_t count = std::min(in.u32(), MAX_IO);
            if (n.kind != Node::File) throw NfsError{ERR_ISDIR};
            auto ws = session_for_read(n.vpath);
            uint64_t size = ws ? ws->size() : n.ve.e.file->size();
            if (off < size) r.data = ws ? ws->read(off, count) : n.ve.e.file->read_at(off, count);
            o.u32(NFS3_OK);
            post_op(o, n);
            o.u32(uint32_t(r.data.size()));
            o.boolean(off + r.data.size() >= size);
            o.u32(uint32_t(r.data.size()));  // the opaque data's length; the bytes follow the head
            return;
        }
        case WRITE: {
            Node n = node_of(in);
            uint64_t off = in.u64();
            uint32_t count = in.u32(), stable = in.u32();
            auto data = in.opaque(MAX_IO);
            if (stable > FILE_SYNC) throw xdr::Error("bad stable_how");
            data = data.first(std::min<size_t>(count, data.size()));
            if (n.kind == Node::File && off + data.size() > n.ve.e.file->size() + MAX_GROWTH) throw NfsError{ERR_FBIG};
            edit_of(n, cred)->ws->write(off, data);
            o.u32(NFS3_OK);
            wcc(o, n);
            o.u32(uint32_t(data.size()));
            // nothing is kept beyond this process either way: the edit is stored
            // when the writer is done, and a new verifier after a restart makes
            // clients send unstable writes again
            o.u32(stable == UNSTABLE ? UNSTABLE : FILE_SYNC);
            o.fixed(verf);
            return;
        }
        case COMMIT: {
            Node n = node_of(in);
            in.u64();
            in.u32();
            o.u32(NFS3_OK);
            wcc(o, n);
            o.fixed(verf);
            return;
        }
        case CREATE: case MKDIR: case SYMLINK: case MKNOD: case REMOVE: case RMDIR: case RENAME: case LINK:
            throw NfsError{ERR_ROFS};  // the tree itself is fixed
        case READDIR:
        case READDIRPLUS: readdir(in, o, proc == READDIRPLUS); return;
        case FSSTAT: {
            Node n = node_of(in);
            Space sp = space(*lib);
            o.u32(NFS3_OK);
            post_op(o, n);
            o.u64(sp.blocks * sp.frsize);  // total bytes
            o.u64(0);  // free
            o.u64(0);  // available
            o.u64(sp.files);
            o.u64(0);
            o.u64(0);
            o.u32(0);  // invarsec
            return;
        }
        case FSINFO: {
            Node n = node_of(in);
            o.u32(NFS3_OK);
            post_op(o, n);
            o.u32(MAX_IO);  // rtmax, rtpref, rtmult
            o.u32(MAX_IO);
            o.u32(4096);
            o.u32(MAX_IO);  // wtmax, wtpref, wtmult
            o.u32(MAX_IO);
            o.u32(4096);
            o.u32(64 * 1024);  // dtpref
            o.u64(INT64_MAX);  // maxfilesize
            o.u32(0);  // time_delta: 1 ns
            o.u32(1);
            o.u32(FSF3_HOMOGENEOUS | FSF3_CANSETTIME);
            return;
        }
        case PATHCONF: {
            Node n = node_of(in);
            o.u32(NFS3_OK);
            post_op(o, n);
            o.u32(1);  // linkmax
            o.u32(MAX_NAME);
            o.boolean(true);  // no_trunc
            o.boolean(true);  // chown_restricted
            o.boolean(false);  // case_insensitive
            o.boolean(true);  // case_preserving
            return;
        }
        }
    }

    void readdir(xdr::In& in, xdr::Out& o, bool plus) {
        Node d = node_of(in);
        uint64_t cookie = in.u64();
        in.fixed(8);  // cookie verifier: cookies are positions, checked against the listing's length
        uint32_t dircount = plus ? in.u32() : UINT32_MAX;
        uint32_t maxcount = in.u32();
        if (d.kind == Node::File) throw NfsError{ERR_NOTDIR};
        auto l = lib->list_vdir(d.vpath);
        size_t total = l->entries.size() + 2;  // ".", "..", then the listing
        o.u32(NFS3_OK);
        post_op(o, d);
        o.fixed(std::array<uint8_t, 8>{});
        // the reply's size so far plus the RPC header (24) and the list's end (8)
        size_t size = o.b.size() + 24 + 8, dirbytes = 0, i = cookie, sent = 0;
        for (; i < total; i++) {
            std::string name = i == 0 ? "." : i == 1 ? ".." : l->entries[i - 2].e.name;
            std::string vpath = i == 0 ? d.vpath : i == 1 ? parent_of(d.vpath) : child_of(d.vpath, name);
            xdr::Out e;
            e.boolean(true);  // an entry follows
            e.u64(fileid_of(vpath));
            e.str(name);
            e.u64(i + 1);  // cookie: where the next call starts
            dirbytes += e.b.size() - 4;
            if (plus) {
                if (i < 2) {
                    e.boolean(false);  // "." and "..": the client knows them
                    e.boolean(false);
                } else {
                    auto& ve = l->entries[i - 2];
                    Node c{ve.e.is_dir ? Node::Dir : Node::File, ve, vpath};
                    post_op(e, c);
                    e.boolean(true);
                    e.opaque(handles.handle(vpath));
                }
            }
            if (size + e.b.size() > maxcount || (plus && dirbytes > dircount && sent > 0)) break;
            size += e.b.size();
            append(o.b, e.b);
            sent++;
        }
        if (sent == 0 && i < total) throw NfsError{ERR_TOOSMALL};
        o.boolean(false);  // no more entries in this reply
        o.boolean(i >= total);  // eof
    }

    // ---- MOUNT procedures

    void mount(uint32_t proc, xdr::In& in, Reply& r) {
        auto& o = r.head;
        switch (proc) {
        case MNT: {
            std::string path = in.str(MAX_PATH);
            auto n = resolve(*lib, path);
            if (!n) o.u32(ERR_NOENT);
            else if (n->kind == Node::File) o.u32(ERR_NOTDIR);
            else {
                o.u32(NFS3_OK);
                o.opaque(handles.handle(n->vpath));
                o.u32(1);  // accepted auth flavors
                o.u32(AUTH_SYS);
                info("NFS: {} mounted", path.empty() ? "/" : path);
            }
            return;
        }
        case MNT_DUMP: o.boolean(false); return;  // no list of clients is kept
        case MNT_UMNT: in.str(MAX_PATH); return;
        case MNT_EXPORT:
            o.boolean(true);
            o.str("/");
            o.boolean(false);  // no groups: anyone who reaches the port
            o.boolean(false);
            return;
        default: return;  // NULL, UMNTALL
        }
    }

    // ---- storing edits

    void commit_loop() {
        std::unique_lock g(edits_mu);
        while (!stopping) {
            edits_cv.wait_for(g, std::chrono::milliseconds(200));
            if (stopping) break;
            g.unlock();
            sweep(false);
            g.lock();
        }
    }

    /// Store edits whose writer has gone quiet (all pending ones if `final`) and
    /// forget sessions nobody uses any more.
    void sweep(bool final) {
        std::vector<std::pair<std::string, std::shared_ptr<Edit>>> due;
        {
            std::lock_guard g(edits_mu);
            int64_t now = now_ns();
            auto ns = [](auto d) { return int64_t(std::chrono::nanoseconds(d).count()); };
            for (auto it = edits.begin(); it != edits.end();) {
                auto& e = *it->second;
                bool dirty = e.ws->dirty();
                if (dirty && e.failed != e.ws->changes() && (final || now - e.changed >= ns(opt.quiet))) {
                    due.emplace_back(it->first, it->second);
                } else if (!dirty && now - e.used >= ns(opt.keep)) {
                    it = edits.erase(it);
                    continue;
                } else if (dirty && now - e.changed >= ns(ABANDON)) {
                    warn("edit of /{} given up: it never became a complete file", it->first);
                    it = edits.erase(it);
                    continue;
                }
                ++it;
            }
        }
        for (auto& [path, e] : due) {
            uint64_t changes = e->ws->changes();
            try {
                store_edit(*lib, e->node, *e->ws, "/" + path);
            } catch (const std::exception& ex) {
                // e.g. a file still being rewritten; tried again after its next write
                warn("edit of /{} not stored: {}", path, ex.what());
                std::lock_guard g(edits_mu);
                e->failed = changes;
            }
        }
    }
};

NfsServer::NfsServer(std::shared_ptr<Library> lib, NfsOptions opt) : impl_(std::make_unique<Impl>(std::move(lib), std::move(opt))) {}
NfsServer::~NfsServer() { stop(); }
void NfsServer::start() { impl_->start(); }
uint16_t NfsServer::port() const { return impl_->port; }
void NfsServer::stop() { impl_->stop(); }

int serve_nfs(std::shared_ptr<Library> lib, const NfsOptions& opt) {
    NfsServer server(std::move(lib), opt);
    server.start();
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGHUP);
    int sig = 0;
    sigwait(&set, &sig);
    info("signal {}: stopping", sig);
    server.stop();
    return 0;
}

}  // namespace wm
