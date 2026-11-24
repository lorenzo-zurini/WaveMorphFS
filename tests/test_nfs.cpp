// SPDX-License-Identifier: GPL-3.0-or-later
// The NFS server, through a minimal NFSv3 client speaking to it over TCP.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <thread>

#include "library.hpp"
#include "nfs.hpp"
#include "test.hpp"
#include "writeback.hpp"
#include "xdr.hpp"

using namespace wm;

namespace {

constexpr uint32_t NFS = 100003, MOUNT = 100005;
enum : uint32_t { GETATTR = 1, SETATTR = 2, LOOKUP = 3, ACCESS = 4, READ = 6, WRITE = 7, CREATE = 8, REMOVE = 12, RENAME = 14, READDIR = 16, READDIRPLUS = 17, FSINFO = 19, COMMIT = 21 };
enum : uint32_t { OK = 0, NOENT = 2, ACCES = 13, NOTDIR = 20, ROFS = 30, STALE = 70, BADHANDLE = 10001 };

struct Reply {
    uint32_t accept_stat = 0;
    Bytes body;
};

/// A connection speaking ONC RPC to the server.
class Client {
public:
    explicit Client(uint16_t port) {
        fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        CHECK(::connect(fd_, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0);
    }
    ~Client() { ::close(fd_); }
    Client(const Client&) = delete;

    /// Send a raw record; false if the connection is gone.
    bool send_record(const Bytes& rec) {
        Bytes out;
        put_be32(out, 0x8000'0000u | uint32_t(rec.size()));
        append(out, rec);
        return ::send(fd_, out.data(), out.size(), MSG_NOSIGNAL) == ssize_t(out.size());
    }
    bool send_raw(const Bytes& b) { return ::send(fd_, b.data(), b.size(), MSG_NOSIGNAL) == ssize_t(b.size()); }
    /// Read a whole record; nullopt when the server closed the connection.
    std::optional<Bytes> recv_record() {
        Bytes msg;
        for (bool last = false; !last;) {
            uint8_t h[4];
            if (!full(h, 4)) return std::nullopt;
            uint32_t m = be32(h);
            last = m >> 31;
            size_t at = msg.size();
            msg.resize(at + (m & 0x7fff'ffff));
            if (!full(msg.data() + at, msg.size() - at)) return std::nullopt;
        }
        return msg;
    }
    Bytes call_record(uint32_t prog, uint32_t proc, const Bytes& args, uint32_t uid, uint32_t vers = 3, uint32_t flavor = 1) {
        xdr::Out o;
        o.u32(++xid_);
        o.u32(0);  // CALL
        o.u32(2);
        o.u32(prog);
        o.u32(vers);
        o.u32(proc);
        o.u32(flavor);
        if (flavor == 1) {
            xdr::Out c;
            c.u32(0);
            c.str("test");
            c.u32(uid);
            c.u32(uid);
            c.u32(0);
            o.opaque(c.b);
        } else {
            o.opaque(Bytes{});
        }
        o.u32(0);  // verifier
        o.opaque(Bytes{});
        append(o.b, args);
        return o.b;
    }
    /// A call answered as accepted; its accept status and result.
    Reply call(uint32_t prog, uint32_t proc, const Bytes& args, uint32_t uid = ::getuid(), uint32_t vers = 3) {
        CHECK(send_record(call_record(prog, proc, args, uid, vers)));
        auto rec = recv_record();
        CHECK_MSG(rec.has_value(), "connection closed");
        xdr::In in(*rec);
        CHECK(in.u32() == xid_);
        CHECK(in.u32() == 1);  // REPLY
        CHECK(in.u32() == 0);  // MSG_ACCEPTED
        in.u32();
        in.opaque(400);
        Reply r;
        r.accept_stat = in.u32();
        auto rest = in.rest();
        r.body.assign(rest.begin(), rest.end());
        return r;
    }
    Bytes nfs(uint32_t proc, const Bytes& args, uint32_t uid = ::getuid()) {
        auto r = call(NFS, proc, args, uid);
        CHECK_MSG(r.accept_stat == 0, "accept_stat {}", r.accept_stat);
        return r.body;
    }

private:
    bool full(uint8_t* p, size_t n) {
        while (n) {
            ssize_t r = ::recv(fd_, p, n, 0);
            if (r <= 0) return false;
            p += r;
            n -= size_t(r);
        }
        return true;
    }
    int fd_;
    uint32_t xid_ = 0;
};

Bytes args(auto&&... parts) {
    xdr::Out o;
    auto one = [&](const auto& p) {
        using T = std::decay_t<decltype(p)>;
        if constexpr (std::is_same_v<T, Bytes>) o.opaque(p);  // a handle
        else if constexpr (std::is_convertible_v<T, std::string_view>) o.str(p);
        else if constexpr (std::is_same_v<T, uint64_t>) o.u64(p);
        else o.u32(uint32_t(p));
    };
    (one(parts), ...);
    return o.b;
}

struct Attr {
    uint32_t type, mode, uid;
    uint64_t size, fileid;
    uint32_t mtime_s, mtime_ns;
};
Attr read_attr(xdr::In& in) {
    Attr a;
    a.type = in.u32();
    a.mode = in.u32();
    in.u32();  // nlink
    a.uid = in.u32();
    in.u32();
    a.size = in.u64();
    in.u64();
    in.u64();  // rdev
    in.u64();  // fsid
    a.fileid = in.u64();
    in.u64();  // atime
    a.mtime_s = in.u32();
    a.mtime_ns = in.u32();
    in.u64();  // ctime
    return a;
}

/// A server on a library in a temporary directory, with a client connected.
struct Served {
    wmtest::TempDir dir{"wm-nfs"};
    fs::path src = dir / "lib";
    std::shared_ptr<Library> lib;
    std::unique_ptr<NfsServer> server;
    std::unique_ptr<Client> client;
    NfsOptions opt;

    Served() {
        fs::create_directories(src);
        opt.listen = "127.0.0.1:0";
        opt.threads = 4;
        opt.insecure = true;  // the test connects from an ordinary port
        opt.quiet = std::chrono::milliseconds(150);
        opt.keep = std::chrono::milliseconds(400);
    }
    ~Served() {
        client.reset();
        if (server) server->stop();
    }
    void start() {
        Config cfg;
        cfg.roots = {{"Lib", src}};
        cfg.tags_dir = dir / "tags";
        cfg.cache_dir = dir / "cache";
        cfg.workers = 0;
        lib = Library::create(std::move(cfg));
        lib->refresh_placements(true);
        server = std::make_unique<NfsServer>(lib, opt);
        server->start();
        client = std::make_unique<Client>(server->port());
    }
    void restart() {
        client.reset();
        server->stop();
        server.reset();
        start();
    }

    Bytes root() {
        auto r = client->call(MOUNT, 1, args(std::string("/")));
        CHECK(r.accept_stat == 0);
        xdr::In in(r.body);
        CHECK(in.u32() == OK);
        auto fh = in.opaque(64);
        return Bytes(fh.begin(), fh.end());
    }
    /// LOOKUP: the status and, if found, the handle.
    std::pair<uint32_t, Bytes> lookup(const Bytes& dir, const std::string& name) {
        auto b = client->nfs(LOOKUP, args(dir, name));
        xdr::In in(b);
        uint32_t st = in.u32();
        if (st != OK) return {st, {}};
        auto fh = in.opaque(64);
        return {st, Bytes(fh.begin(), fh.end())};
    }
    Bytes walk(const std::string& path) {
        Bytes fh = root();
        for (auto& c : split(path, '/')) {
            auto [st, h] = lookup(fh, c);
            CHECK_MSG(st == OK, "lookup {}: {}", c, st);
            fh = h;
        }
        return fh;
    }
    uint32_t status(uint32_t proc, const Bytes& a, uint32_t uid = ::getuid()) {
        auto b = client->nfs(proc, a, uid);
        xdr::In in(b);
        return in.u32();
    }
    Attr getattr(const Bytes& fh) {
        auto b = client->nfs(GETATTR, args(fh));
        xdr::In in(b);
        CHECK(in.u32() == OK);
        return read_attr(in);
    }
    Bytes read_all(const Bytes& fh, uint32_t chunk = 4096) {
        Bytes out;
        for (bool eof = false; !eof;) {
            auto b = client->nfs(READ, args(fh, uint64_t(out.size()), chunk));
            xdr::In in(b);
            CHECK(in.u32() == OK);
            CHECK(in.boolean());
            read_attr(in);
            uint32_t n = in.u32();
            eof = in.boolean();
            auto d = in.opaque(1u << 20);
            CHECK(d.size() == n);
            append(out, d);
            CHECK(n > 0 || eof);
        }
        return out;
    }
    uint32_t write(const Bytes& fh, uint64_t off, std::span<const uint8_t> data, uint32_t uid = ::getuid()) {
        xdr::Out o;
        o.opaque(fh);
        o.u64(off);
        o.u32(uint32_t(data.size()));
        o.u32(0);  // UNSTABLE
        o.opaque(data);
        return status(WRITE, o.b, uid);
    }
    uint32_t truncate(const Bytes& fh, uint64_t size) {
        xdr::Out o;
        o.opaque(fh);
        o.u32(0), o.u32(0), o.u32(0);  // mode, uid, gid unchanged
        o.u32(1), o.u64(size);
        o.u32(0), o.u32(0);  // times unchanged
        o.u32(0);  // no guard
        return status(SETATTR, o.b);
    }
    /// Every name of a directory, paging with cookies through replies of at most `max` bytes.
    std::vector<std::string> list(const Bytes& dir, bool plus, uint32_t max) {
        std::vector<std::string> names;
        uint64_t cookie = 0;
        for (bool eof = false; !eof;) {
            xdr::Out o;
            o.opaque(dir);
            o.u64(cookie);
            o.fixed(Bytes(8, 0));
            if (plus) o.u32(max);
            o.u32(max);
            auto b = client->nfs(plus ? READDIRPLUS : READDIR, o.b);
            xdr::In in(b);
            CHECK(in.u32() == OK);
            if (in.boolean()) read_attr(in);
            in.fixed(8);
            size_t n = 0;
            while (in.boolean()) {
                in.u64();
                names.push_back(in.str(255));
                cookie = in.u64();
                n++;
                if (plus) {
                    if (in.boolean()) read_attr(in);
                    if (in.boolean()) {
                        auto fh = in.opaque(64);
                        if (names.back() != "." && names.back() != "..") CHECK(lookup(dir, names.back()).second == Bytes(fh.begin(), fh.end()));
                    }
                }
            }
            eof = in.boolean();
            CHECK(n > 0 || eof);
        }
        return names;
    }
};

/// A short FLAC file with the given TITLE.
void make_flac(const fs::path& p, const std::string& title) {
    fs::create_directories(p.parent_path());
    auto st = run({"ffmpeg", "-v", "error", "-f", "lavfi", "-i", "sine=f=440:d=1", "-c:a", "flac", p.string()});
    CHECK_MSG(st.ok(), "ffmpeg: {}", st.err);
    st = run({"metaflac", "--set-tag=TITLE=" + title, p.string()});
    CHECK_MSG(st.ok(), "metaflac: {}", st.err);
}

std::string title_of(const Bytes& b) {
    auto t = read_file_tags("flac", [&](uint64_t o, size_t n) { return Bytes(b.begin() + ptrdiff_t(std::min<uint64_t>(o, b.size())), b.begin() + ptrdiff_t(std::min<uint64_t>(o + n, b.size()))); }, b.size());
    return t.get("TITLE") ? *t.get("TITLE") : "";
}

}  // namespace

TEST(nfs_serves_the_tree_and_its_bytes) {
    Served s;
    make_flac(s.src / "A" / "a.flac", "Old");
    atomic_write(s.src / "A" / "notes.txt", "hello");
    atomic_write(s.src / "A" / ".hidden", "x");
    s.start();
    Bytes root = s.root();
    CHECK(s.getattr(root).type == 2);
    CHECK(s.lookup(root, "Nope").first == NOENT);
    Bytes dir = s.walk("Lib/A");
    CHECK(s.lookup(dir, ".hidden").first == NOENT);
    CHECK(s.lookup(dir, "..").second == s.walk("Lib"));
    Bytes fh = s.walk("Lib/A/a.flac");
    auto ve = s.lib->lookup("Lib/A", "a.flac");
    CHECK(ve && ve->e.file);
    Bytes want = ve->e.file->read_at(0, size_t(ve->e.file->size()));
    Attr a = s.getattr(fh);
    CHECK(a.type == 1 && a.mode == 0644 && a.uid == ::getuid());
    CHECK(a.size == want.size());
    CHECK(int64_t(a.mtime_s) * 1'000'000'000 + a.mtime_ns == ve->e.mtime);
    CHECK(s.read_all(fh) == want);
    CHECK(s.read_all(fh, 1u << 20) == want);
    CHECK(s.getattr(s.walk("Lib/A/notes.txt")).mode == 0444);  // not a tagged format: not editable
    CHECK(s.lookup(fh, "x").first == NOTDIR);
    // directory listings, also paged through tiny replies
    std::vector<std::string> names = {".", "..", "a.flac", "notes.txt"};
    CHECK(s.list(dir, false, 4096) == names);
    CHECK(s.list(dir, true, 4096) == names);
    CHECK(s.list(dir, true, 400) == names);
    CHECK(s.list(dir, false, 200) == names);
    // the tree is fixed
    CHECK(s.status(CREATE, args(dir, std::string("new.flac"), 0)) == ROFS);
    CHECK(s.status(REMOVE, args(dir, std::string("a.flac"))) == ROFS);
    CHECK(s.status(RENAME, args(dir, std::string("a.flac"), dir, std::string("b.flac"))) == ROFS);
    auto info = s.client->nfs(FSINFO, args(root));
    xdr::In in(info);
    CHECK(in.u32() == OK);
}

TEST(nfs_lists_large_directories_in_pages) {
    Served s;
    std::vector<std::string> names = {".", ".."};
    fs::create_directories(s.src / "Big");
    for (int i = 0; i < 300; i++) {
        auto n = std::format("file {:03} with a longish name.txt", i);
        atomic_write(s.src / "Big" / n, "x");
        names.push_back(n);
    }
    s.start();
    Bytes dir = s.walk("Lib/Big");
    for (bool plus : {false, true})
        for (uint32_t max : {1024u, 8192u, 65536u}) CHECK(s.list(dir, plus, max) == names);
}

TEST(nfs_handles_survive_a_restart) {
    Served s;
    make_flac(s.src / "A" / "a.flac", "T");
    s.start();
    Bytes fh = s.walk("Lib/A/a.flac");
    Bytes dir = s.walk("Lib/A");
    uint64_t id = s.getattr(fh).fileid;
    s.restart();
    // no MOUNT or LOOKUP first: the old handles work as they are
    CHECK(s.getattr(fh).fileid == id);
    CHECK(s.list(dir, true, 4096).size() == 3);
    Bytes stale = fh;
    stale.back() ^= 1;
    CHECK(s.status(GETATTR, args(stale)) == STALE);
    CHECK(s.status(GETATTR, args(Bytes{1, 2, 3})) == BADHANDLE);
    // a file that went away
    fs::remove(s.src / "A" / "a.flac");
    s.lib->invalidate(s.src / "A");
    CHECK(s.status(GETATTR, args(fh)) == STALE);
}

TEST(nfs_edits_are_stored_once_the_writer_is_quiet) {
    Served s;
    make_flac(s.src / "A" / "a.flac", "Old");
    s.start();
    Bytes fh = s.walk("Lib/A/a.flac");
    Bytes orig = s.read_all(fh);
    // what a tag editor would write: the file with a new title (longer than the padding, so the audio moves)
    fs::path scratch = s.dir / "edited.flac";
    atomic_write(scratch, orig);
    CHECK(run({"metaflac", "--remove-tag=TITLE", "--set-tag=TITLE=" + std::string(40000, 'N'), scratch.string()}).ok());
    Bytes edited = read_file(scratch);
    CHECK(edited.size() != orig.size());
    // only the owner (or root) may edit, and only tagged formats
    CHECK(s.write(fh, 0, edited, ::getuid() + 1) == ACCES);
    atomic_write(s.src / "A" / "notes.txt", "hello");
    s.lib->invalidate(s.src / "A");
    CHECK(s.write(s.walk("Lib/A/notes.txt"), 0, bytes_of("x")) == ROFS);
    // written in pieces, out of order, as a client's write-back does
    size_t half = edited.size() / 2;
    CHECK(s.write(fh, half, std::span(edited).subspan(half)) == OK);
    CHECK(s.write(fh, 0, std::span(edited).first(half)) == OK);
    CHECK(s.status(COMMIT, args(fh, uint64_t(0), 0)) == OK);
    // read back from the session at once, size and all
    CHECK(s.getattr(fh).size == edited.size());
    CHECK(s.read_all(fh) == edited);
    // stored once quiet
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    auto sc = try_read_text(s.dir / "tags" / "Lib" / "A" / SIDECAR_NAME);
    CHECK_MSG(sc && sc->find(std::string(40000, 'N')) != std::string::npos, "sidecar: {}", sc ? *sc : "none");
    // once the session is gone, the generated file carries the new title
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    Bytes now = s.read_all(fh);
    CHECK(title_of(now) == std::string(40000, 'N'));
    auto ve = s.lib->lookup("Lib/A", "a.flac");
    CHECK(s.getattr(fh).size == ve->e.file->size());
}

TEST(nfs_truncating_editor_and_unfinished_edits) {
    Served s;
    make_flac(s.src / "A" / "a.flac", "Old");
    s.start();
    Bytes fh = s.walk("Lib/A/a.flac");
    Bytes orig = s.read_all(fh);
    fs::path scratch = s.dir / "edited.flac";
    atomic_write(scratch, orig);
    CHECK(run({"metaflac", "--set-tag=ARTIST=Someone", scratch.string()}).ok());
    Bytes edited = read_file(scratch);
    // truncated to nothing, then written up to the middle of its metadata: not
    // a complete file, so nothing is stored
    CHECK(s.truncate(fh, 0) == OK);
    CHECK(s.getattr(fh).size == 0);
    CHECK(s.write(fh, 0, std::span(edited).first(60)) == OK);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    CHECK(!fs::exists(s.dir / "tags" / "Lib" / "A" / SIDECAR_NAME));
    // the rest arrives: stored
    CHECK(s.write(fh, 60, std::span(edited).subspan(60)) == OK);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    auto sc = try_read_text(s.dir / "tags" / "Lib" / "A" / SIDECAR_NAME);
    CHECK_MSG(sc && sc->find("Someone") != std::string::npos, "sidecar: {}", sc ? *sc : "none");
}

TEST(nfs_pending_edit_stored_when_the_server_stops) {
    Served s;
    s.opt.quiet = std::chrono::hours(1);
    make_flac(s.src / "A" / "a.flac", "Old");
    s.start();
    Bytes fh = s.walk("Lib/A/a.flac");
    fs::path scratch = s.dir / "edited.flac";
    atomic_write(scratch, s.read_all(fh));
    CHECK(run({"metaflac", "--set-tag=GENRE=Stopped", scratch.string()}).ok());
    CHECK(s.write(fh, 0, read_file(scratch)) == OK);
    s.client.reset();
    s.server->stop();
    auto sc = try_read_text(s.dir / "tags" / "Lib" / "A" / SIDECAR_NAME);
    CHECK(sc && sc->find("Stopped") != std::string::npos);
}

TEST(nfs_access_reports_what_the_caller_may_do) {
    Served s;
    make_flac(s.src / "A" / "a.flac", "T");
    s.start();
    Bytes fh = s.walk("Lib/A/a.flac"), dir = s.walk("Lib/A");
    auto access = [&](const Bytes& h, uint32_t uid) {
        auto b = s.client->nfs(ACCESS, args(h, 0x3f), uid);
        xdr::In in(b);
        CHECK(in.u32() == OK);
        if (in.boolean()) read_attr(in);
        return in.u32();
    };
    CHECK(access(fh, ::getuid()) == (0x01 | 0x04 | 0x08));  // read, modify, extend
    CHECK(access(fh, 0) == (0x01 | 0x04 | 0x08));
    CHECK(access(fh, ::getuid() + 1) == 0x01);
    CHECK(access(dir, ::getuid() + 1) == (0x01 | 0x02 | 0x20));  // read, lookup, execute
}

TEST(nfs_rpc_errors_and_garbage) {
    Served s;
    s.start();
    Bytes root = s.root();
    CHECK(s.client->call(100227, 0, {}).accept_stat == 1);  // NFS ACL: PROG_UNAVAIL
    CHECK(s.client->call(NFS, 0, {}, ::getuid(), 4).accept_stat == 2);  // version 4: PROG_MISMATCH
    CHECK(s.client->call(NFS, 99, {}).accept_stat == 3);  // PROC_UNAVAIL
    CHECK(s.client->call(NFS, GETATTR, Bytes{0, 0}).accept_stat == 4);  // GARBAGE_ARGS
    CHECK(s.client->call(NFS, LOOKUP, args(root, 70000u)).accept_stat == 4);  // name longer than the message
    // another authentication flavor (RPCSEC_GSS): denied
    {
        Client c(s.server->port());
        CHECK(c.send_record(c.call_record(NFS, 0, {}, 0, 3, 6)));
        auto rec = c.recv_record();
        CHECK(rec && rec->size() >= 12 && be32(rec->data() + 8) == 1);  // MSG_DENIED
    }
    // not RPC at all, and a record mark promising more than the server takes: dropped
    {
        Client c(s.server->port());
        CHECK(c.send_record(Bytes{1, 2, 3, 4, 5, 6, 7, 8, 9}));
        CHECK(!c.recv_record());
    }
    {
        Client c(s.server->port());
        Bytes huge;
        put_be32(huge, 0x8000'0000u | 0x7fff'ffffu);
        CHECK(c.send_raw(huge));
        CHECK(!c.recv_record());
    }
    // the server is unharmed
    CHECK(s.getattr(root).type == 2);
    Client other(s.server->port());
    CHECK(other.call(NFS, 0, {}).accept_stat == 0);
}

TEST(nfs_refuses_unreserved_ports) {
    Served s;
    s.opt.insecure = false;
    s.start();  // connects from an ordinary port
    CHECK(s.client->send_record(s.client->call_record(NFS, 0, {}, 0)));
    CHECK(!s.client->recv_record());
}

TEST(nfs_cover_embedded_by_a_tag_editor) {
    Served s;
    make_flac(s.src / "A" / "a.flac", "Song");
    s.start();
    fs::path png = s.dir / "green.png";
    CHECK(run({"ffmpeg", "-v", "error", "-f", "lavfi", "-i", "color=c=green:s=40x30", "-frames:v", "1", png.string()}).ok());
    Bytes green = read_file(png);
    Bytes fh = s.walk("Lib/A/a.flac");
    fs::path scratch = s.dir / "edited.flac";
    atomic_write(scratch, s.read_all(fh));
    CHECK(run({"metaflac", "--import-picture-from=" + png.string(), scratch.string()}).ok());
    CHECK(s.write(fh, 0, read_file(scratch)) == OK);
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));  // stored, and the session gone
    auto sc = try_read_text(s.dir / "tags" / "Lib" / "A" / SIDECAR_NAME);
    CHECK_MSG(sc && sc->find("\"_cover\": \"./.covers/") != std::string::npos, "sidecar: {}", sc ? *sc : "none");
    CHECK_MSG(!fs::exists(s.src / "A" / ".covers"), "nothing is written next to the source");
    Bytes served = s.read_all(fh);
    auto cover = read_front_cover("flac", [&](uint64_t o, size_t n) { return Bytes(served.begin() + ptrdiff_t(std::min<uint64_t>(o, served.size())), served.begin() + ptrdiff_t(std::min<uint64_t>(o + n, served.size()))); }, served.size());
    CHECK_MSG(cover == green, "the generated file carries the cover the editor embedded");
}

TEST(nfs_attributes_while_edit_sessions_come_and_go) {
    // a session stored and dropped under concurrent GETATTR/READ (one was once
    // freed under a concurrent stat)
    Served s;
    s.opt.quiet = std::chrono::milliseconds(5);
    s.opt.keep = std::chrono::milliseconds(10);
    make_flac(s.src / "A" / "a.flac", "Song");
    s.start();
    Bytes fh = s.walk("Lib/A/a.flac");
    Bytes orig = s.read_all(fh);
    std::atomic<bool> stop = false;
    std::atomic<int> bad = 0, calls = 0;
    std::thread reader([&] {
        Client c(s.server->port());
        while (!stop) {
            try {
                int n = calls++;
                auto r = n % 2 ? c.call(NFS, GETATTR, args(fh)) : c.call(NFS, READ, args(fh, uint64_t(0), 4096u));
                if (r.accept_stat != 0 || r.body.size() < 4 || be32(r.body.data()) != OK) bad++;
            } catch (...) {
                bad++;
                return;
            }
        }
    });
    for (int i = 0; i < 150; i++) {
        CHECK(s.write(fh, 0, std::span(orig).first(4096)) == OK);  // an editor rewriting what is there
        std::this_thread::sleep_for(std::chrono::milliseconds(i % 7));
    }
    stop = true;
    reader.join();
    CHECK_MSG(bad == 0 && calls > 100, "{} of {} calls failed", bad.load(), calls.load());
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(s.read_all(fh) == orig);  // nothing changed, nothing stored
    CHECK(!fs::exists(s.dir / "tags" / "Lib" / "A" / SIDECAR_NAME));
}
