// SPDX-License-Identifier: GPL-3.0-or-later
#include "util.hpp"

#include <fcntl.h>
#include <malloc.h>
#include <sys/mman.h>
#include <poll.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <mutex>
#include <thread>

extern char** environ;

namespace wm {

// ---------------------------------------------------------------- strings

std::string lower(std::string_view s) {
    std::string o(s);
    for (auto& c : o)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return o;
}

std::string upper(std::string_view s) {
    std::string o(s);
    for (auto& c : o)
        if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
    return o;
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = char(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = char(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

bool starts_with(std::string_view s, std::string_view p) { return s.substr(0, p.size()) == p; }
bool ends_with(std::string_view s, std::string_view p) { return s.size() >= p.size() && s.substr(s.size() - p.size()) == p; }
bool iends_with(std::string_view s, std::string_view p) { return s.size() >= p.size() && iequals(s.substr(s.size() - p.size()), p); }

// Unicode White_Space (what Rust's str::trim uses)
static bool is_ws_cp(char32_t c) {
    return c == ' ' || (c >= 0x09 && c <= 0x0D) || c == 0x85 || c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) ||
           c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
}

// decode one UTF-8 sequence at s[i]; returns code point (0xFFFD on error) and its length
static char32_t utf8_at(std::string_view s, size_t i, size_t& len) {
    auto b = uint8_t(s[i]);
    len = 1;
    if (b < 0x80) return b;
    int n = b >= 0xF0 ? 4 : b >= 0xE0 ? 3 : b >= 0xC0 ? 2 : 0;
    if (n == 0 || i + n > s.size()) return 0xFFFD;
    char32_t c = b & (0x3F >> (n - 1));
    for (int k = 1; k < n; k++) {
        auto x = uint8_t(s[i + k]);
        if ((x & 0xC0) != 0x80) return 0xFFFD;
        c = c << 6 | (x & 0x3F);
    }
    len = n;
    return c;
}

bool ws_at(std::string_view s, size_t i, size_t& len) { return is_ws_cp(utf8_at(s, i, len)); }

std::string trim_end(std::string_view s) {
    size_t end = s.size();
    while (end > 0) {
        size_t st = end - 1;
        while (st > 0 && (uint8_t(s[st]) & 0xC0) == 0x80) st--;
        size_t len;
        if (!ws_at(s, st, len) || st + len != end) break;
        end = st;
    }
    return std::string(s.substr(0, end));
}

std::string trim(std::string_view s) {
    size_t i = 0, len;
    while (i < s.size() && ws_at(s, i, len)) i += len;
    return trim_end(s.substr(i));
}

std::vector<std::string> split(std::string_view s, char sep) {
    std::vector<std::string> out;
    size_t a = 0;
    while (true) {
        size_t b = s.find(sep, a);
        out.emplace_back(s.substr(a, b == std::string_view::npos ? std::string_view::npos : b - a));
        if (b == std::string_view::npos) break;
        a = b + 1;
    }
    return out;
}

std::string hex(std::span<const uint8_t> b) {
    static const char* d = "0123456789abcdef";
    std::string o;
    o.reserve(b.size() * 2);
    for (auto x : b) {
        o.push_back(d[x >> 4]);
        o.push_back(d[x & 15]);
    }
    return o;
}

std::optional<uint64_t> parse_u64(std::string_view s) {
    if (!s.empty() && s[0] == '+') s.remove_prefix(1);
    if (s.empty()) return std::nullopt;
    uint64_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return std::nullopt;
        if (v > (UINT64_MAX - (c - '0')) / 10) return std::nullopt;
        v = v * 10 + uint64_t(c - '0');
    }
    return v;
}

void put_utf8(std::string& o, char32_t c) {
    if (c < 0x80) {
        o.push_back(char(c));
    } else if (c < 0x800) {
        o.push_back(char(0xC0 | c >> 6));
        o.push_back(char(0x80 | (c & 0x3F)));
    } else if (c < 0x10000) {
        o.push_back(char(0xE0 | c >> 12));
        o.push_back(char(0x80 | (c >> 6 & 0x3F)));
        o.push_back(char(0x80 | (c & 0x3F)));
    } else {
        o.push_back(char(0xF0 | c >> 18));
        o.push_back(char(0x80 | (c >> 12 & 0x3F)));
        o.push_back(char(0x80 | (c >> 6 & 0x3F)));
        o.push_back(char(0x80 | (c & 0x3F)));
    }
}

std::string lossy_utf8(std::span<const uint8_t> b) {
    std::string_view s = str_of(b);
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        size_t len;
        char32_t c = utf8_at(s, i, len);
        if (c == 0xFFFD && !(len == 3 && s.substr(i, 3) == "\xEF\xBF\xBD"))
            put_utf8(o, 0xFFFD);
        else
            o.append(s.substr(i, len));
        i += len;
    }
    return o;
}

std::string ext_lower(const fs::path& p) {
    auto e = p.extension().string();
    return e.empty() ? std::string() : lower(e.substr(1));
}

std::string path_str(const fs::path& p) { return p.string(); }

// ---------------------------------------------------------------- files

File::File(const fs::path& p) : name_(p.string()) {
    fd_ = ::open(p.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd_ < 0) fail("open {}: {}", name_, std::strerror(errno));
}

File::~File() {
    if (fd_ >= 0) ::close(fd_);
}

size_t File::read_at(uint8_t* buf, size_t len, uint64_t off) const {
    size_t done = 0;
    while (done < len) {
        ssize_t n = ::pread(fd_, buf + done, len - done, off_t(off + done));
        if (n == 0) break;
        if (n < 0) {
            if (errno == EINTR) continue;
            fail("read {}: {}", name_, std::strerror(errno));
        }
        done += size_t(n);
    }
    return done;
}

void File::read_exact_at(uint8_t* buf, size_t len, uint64_t off) const {
    if (read_at(buf, len, off) != len) fail("{}: unexpected end of file at {}", name_, off);
}

Bytes File::read_vec(uint64_t off, size_t len) const {
    Bytes b(len);
    b.resize(read_at(b.data(), len, off));
    return b;
}

uint64_t File::size() const {
    struct stat st;
    if (::fstat(fd_, &st) != 0) fail("stat {}: {}", name_, std::strerror(errno));
    return uint64_t(st.st_size);
}

Bytes read_file(const fs::path& p) {
    File f(p);
    return f.read_vec(0, f.size());
}

std::optional<Bytes> try_read_file(const fs::path& p) {
    try {
        return read_file(p);
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<std::string> try_read_text(const fs::path& p) {
    auto b = try_read_file(p);
    if (!b) return std::nullopt;
    return std::string(b->begin(), b->end());
}

fs::path unique_tmp(const fs::path& p) {
    auto tid = std::hash<std::thread::id>{}(std::this_thread::get_id());
    auto name = p.filename().string() + std::format(".tmp.{}.{}", ::getpid(), tid);
    return p.parent_path() / name;
}

void atomic_write(const fs::path& p, std::span<const uint8_t> data) {
    auto tmp = unique_tmp(p);
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) fail("write {}: {}", tmp.string(), std::strerror(errno));
    size_t done = 0;
    while (done < data.size()) {
        ssize_t n = ::write(fd, data.data() + done, data.size() - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            int e = errno;
            ::close(fd);
            ::unlink(tmp.c_str());
            fail("write {}: {}", tmp.string(), std::strerror(e));
        }
        done += size_t(n);
    }
    ::close(fd);
    if (::rename(tmp.c_str(), p.c_str()) != 0) {
        int e = errno;
        ::unlink(tmp.c_str());
        fail("rename {}: {}", p.string(), std::strerror(e));
    }
}

std::optional<int64_t> mtime_ns(const fs::path& p) {
    struct stat st;
    if (::stat(p.c_str(), &st) != 0) return std::nullopt;
    return int64_t(st.st_mtim.tv_sec) * 1'000'000'000 + st.st_mtim.tv_nsec;
}

int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

SrcKey SrcKey::of(const fs::path& p) {
    struct stat st;
    if (::stat(p.c_str(), &st) != 0) fail("stat {}: {}", p.string(), std::strerror(errno));
    return {uint64_t(st.st_size), int64_t(st.st_mtim.tv_sec) * 1'000'000'000 + st.st_mtim.tv_nsec};
}

std::optional<SrcKey> SrcKey::try_of(const fs::path& p) {
    struct stat st;
    if (::stat(p.c_str(), &st) != 0) return std::nullopt;
    return SrcKey{uint64_t(st.st_size), int64_t(st.st_mtim.tv_sec) * 1'000'000'000 + st.st_mtim.tv_nsec};
}

// ---------------------------------------------------------------- worker pool

namespace {
struct Pool {
    std::mutex mu;
    std::condition_variable cv;
    std::deque<std::function<void()>> q;
    size_t n;
    Pool() : n(std::max(2u, std::thread::hardware_concurrency())) {
        for (size_t i = 0; i < n; i++)
            std::thread([this] {
                while (true) {
                    std::function<void()> t;
                    {
                        std::unique_lock g(mu);
                        cv.wait(g, [&] { return !q.empty(); });
                        t = std::move(q.front());
                        q.pop_front();
                    }
                    try {
                        t();
                    } catch (const std::exception& e) {
                        warn("background task: {}", e.what());
                    }
                }
            }).detach();
    }
};
Pool& pool(Lane lane = Lane::Decode) {
    // never destroyed: workers wait on it until the process ends (destroying a
    // condition variable with waiters blocks exit)
    static Pool* decode = new Pool;
    static Pool* encode = new Pool;
    return lane == Lane::Decode ? *decode : *encode;
}
}  // namespace

void submit(std::function<void()> task, Lane lane) {
    auto& p = pool(lane);
    {
        std::lock_guard g(p.mu);
        p.q.push_back(std::move(task));
    }
    p.cv.notify_one();
}

size_t pool_size() { return pool().n; }

// idle objects are trimmed after this long; the sweeper looks this often
constexpr auto TRIM_IDLE = std::chrono::seconds(15);
constexpr auto TRIM_EVERY = std::chrono::seconds(5);

struct Sweeper {
    std::mutex mu;
    std::vector<std::weak_ptr<const Trimmable>> items;

    static Sweeper& get() {
        // leaked on purpose like the pools: the thread runs until exit
        static Sweeper* s = [] {
            auto* sw = new Sweeper;
            std::thread([sw] {
                while (true) {
                    std::this_thread::sleep_for(TRIM_EVERY);
                    sw->sweep(TRIM_IDLE);
                }
            }).detach();
            return sw;
        }();
        return *s;
    }

    size_t sweep(std::chrono::nanoseconds idle) {
        std::vector<std::shared_ptr<const Trimmable>> live;
        {
            std::lock_guard g(mu);
            std::erase_if(items, [&](auto& w) {
                auto p = w.lock();
                if (!p) return true;
                live.push_back(std::move(p));
                return false;
            });
        }
        int64_t idle_before = now_ns() - idle.count();
        size_t trimmed = 0;
        for (auto& t : live) {
            int64_t used = t->last_use_.load();
            if (used > idle_before || t->trimmed_at_.load() == used) continue;
            t->drop_cached();
            t->trimmed_at_ = used;
            trimmed++;
        }
        live.clear();  // the last owner of a removed object frees it here, before trimming the heap
        if (trimmed) {
            trim_heap();
            debug("released the cached audio of {} idle files", trimmed);
        }
        return trimmed;
    }
};

size_t sweep_idle(std::chrono::nanoseconds idle) { return Sweeper::get().sweep(idle); }

void trim_heap() {
    malloc_trim(0);
    if (log_enabled(Level::Debug)) {
        auto mi = mallinfo2();
        debug("heap: {} MB in use, {} MB free, {} MB mapped", (mi.uordblks + mi.hblkhd) >> 20, mi.fordblks >> 20, mi.hblkhd >> 20);
    }
}

std::optional<MappedFile> map_file(const fs::path& p) {
    int fd = ::open(p.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return std::nullopt;
    struct stat st{};
    void* map = MAP_FAILED;
    if (::fstat(fd, &st) == 0 && st.st_size > 0) map = ::mmap(nullptr, size_t(st.st_size), PROT_READ, MAP_SHARED, fd, 0);
    ::close(fd);  // the mapping keeps the file
    if (map == MAP_FAILED) return std::nullopt;
    size_t len = size_t(st.st_size);
    MappedFile m;
    m.owner = std::shared_ptr<const void>(map, [len](const void* q) { ::munmap(const_cast<void*>(q), len); });
    m.bytes = {static_cast<const uint8_t*>(map), len};
    return m;
}

void Trimmable::touch(const std::shared_ptr<const Trimmable>& self) const {
    last_use_ = now_ns();
    if (self && !registered_.exchange(true)) {
        auto& s = Sweeper::get();
        std::lock_guard g(s.mu);
        s.items.push_back(self);
    }
}

// ---------------------------------------------------------------- logging

static std::atomic<int> g_level{int(Level::Info)};
static std::mutex g_log_mu;

void log_init() {
    if (const char* v = std::getenv("WAVEMORPH_LOG")) {
        std::string s = lower(v);
        if (s == "debug" || s == "trace") g_level = int(Level::Debug);
        else if (s == "warn") g_level = int(Level::Warn);
        else if (s == "error") g_level = int(Level::Error);
        else g_level = int(Level::Info);
    }
}

bool log_enabled(Level l) { return int(l) >= g_level.load(std::memory_order_relaxed); }

void log_line(Level l, const std::string& msg) {
    static const char* names[] = {"DEBUG", "INFO ", "WARN ", "ERROR"};
    time_t t = ::time(nullptr);
    struct tm tm;
    ::gmtime_r(&t, &tm);
    char ts[32];
    std::strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%SZ", &tm);
    std::lock_guard g(g_log_mu);
    std::fprintf(stderr, "[%s %s] %s\n", ts, names[int(l)], msg.c_str());
}

// ---------------------------------------------------------------- processes

static std::vector<char*> cargv(const std::vector<std::string>& argv) {
    std::vector<char*> v;
    for (auto& a : argv) v.push_back(const_cast<char*>(a.c_str()));
    v.push_back(nullptr);
    return v;
}

static int wait_status(pid_t pid) {
    int st;
    while (::waitpid(pid, &st, 0) < 0)
        if (errno != EINTR) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

ProcResult run(const std::vector<std::string>& argv) {
    ProcResult r;
    int po[2], pe[2];
    if (::pipe2(po, O_CLOEXEC) != 0) return r;
    if (::pipe2(pe, O_CLOEXEC) != 0) {
        ::close(po[0]);
        ::close(po[1]);
        return r;
    }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, po[1], 1);
    posix_spawn_file_actions_adddup2(&fa, pe[1], 2);
    pid_t pid;
    auto av = cargv(argv);
    int rc = posix_spawnp(&pid, av[0], &fa, nullptr, av.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    ::close(po[1]);
    ::close(pe[1]);
    if (rc != 0) {
        ::close(po[0]);
        ::close(pe[0]);
        r.err = std::format("cannot run {}: {}", argv[0], std::strerror(rc));
        return r;
    }
    struct pollfd fds[2] = {{po[0], POLLIN, 0}, {pe[0], POLLIN, 0}};
    std::string* sinks[2] = {&r.out, &r.err};
    int open_n = 2;
    char buf[65536];
    while (open_n > 0) {
        if (::poll(fds, 2, -1) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (int i = 0; i < 2; i++) {
            if (fds[i].fd < 0 || !(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            ssize_t n = ::read(fds[i].fd, buf, sizeof buf);
            if (n > 0) {
                sinks[i]->append(buf, size_t(n));
            } else if (n == 0 || errno != EINTR) {
                ::close(fds[i].fd);
                fds[i].fd = -1;
                open_n--;
            }
        }
    }
    for (auto& f : fds)
        if (f.fd >= 0) ::close(f.fd);
    r.status = wait_status(pid);
    return r;
}

int run_inherit(const std::vector<std::string>& argv) {
    pid_t pid;
    auto av = cargv(argv);
    if (posix_spawnp(&pid, av[0], nullptr, nullptr, av.data(), environ) != 0) return -1;
    return wait_status(pid);
}

}  // namespace wm
