// SPDX-License-Identifier: GPL-3.0-or-later
// Shared helpers: errors, byte order, strings, files, logging, child processes.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace wm {

using Bytes = std::vector<uint8_t>;
/// Random-access read of a (virtual) file: up to `len` bytes at `off`.
using ByteReader = std::function<Bytes(uint64_t off, size_t len)>;

struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

template <class... A>
[[noreturn]] void fail(std::format_string<A...> f, A&&... a) {
    throw Error(std::format(f, std::forward<A>(a)...));
}

#define WM_ENSURE(cond, ...)                \
    do {                                    \
        if (!(cond)) ::wm::fail(__VA_ARGS__); \
    } while (0)

/// Run `f`; if it throws, rethrow with `ctx: ` prepended (like anyhow's context).
template <class F>
auto with_context(const std::string& ctx, F&& f) -> decltype(f()) {
    try {
        return f();
    } catch (const std::exception& e) {
        throw Error(ctx + ": " + e.what());
    }
}

// ---- byte order
inline uint16_t be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
inline uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
inline uint64_t be64(const uint8_t* p) { return uint64_t(be32(p)) << 32 | be32(p + 4); }
inline uint32_t le32(const uint8_t* p) { return uint32_t(p[3]) << 24 | uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0]; }
inline uint64_t le64(const uint8_t* p) { return uint64_t(le32(p + 4)) << 32 | le32(p); }
inline void put_be16(Bytes& b, uint16_t v) { b.push_back(v >> 8); b.push_back(uint8_t(v)); }
inline void put_be24(Bytes& b, uint32_t v) { b.push_back(v >> 16); b.push_back(v >> 8); b.push_back(uint8_t(v)); }
inline void put_be32(Bytes& b, uint32_t v) { b.push_back(v >> 24); b.push_back(v >> 16); b.push_back(v >> 8); b.push_back(uint8_t(v)); }
inline void put_be64(Bytes& b, uint64_t v) { put_be32(b, uint32_t(v >> 32)); put_be32(b, uint32_t(v)); }
inline void put_le16(Bytes& b, uint16_t v) { b.push_back(uint8_t(v)); b.push_back(v >> 8); }
inline void put_le32(Bytes& b, uint32_t v) { for (int i = 0; i < 4; i++) b.push_back(uint8_t(v >> (8 * i))); }
inline void put_le64(Bytes& b, uint64_t v) { for (int i = 0; i < 8; i++) b.push_back(uint8_t(v >> (8 * i))); }
inline void append(Bytes& b, std::span<const uint8_t> s) { b.insert(b.end(), s.begin(), s.end()); }
inline void append(Bytes& b, std::string_view s) { b.insert(b.end(), s.begin(), s.end()); }
inline std::span<const uint8_t> bytes_of(std::string_view s) { return {reinterpret_cast<const uint8_t*>(s.data()), s.size()}; }
inline std::string_view str_of(std::span<const uint8_t> b) { return {reinterpret_cast<const char*>(b.data()), b.size()}; }

// ---- strings (ASCII case rules, as tag names and file extensions use)
std::string lower(std::string_view s);
std::string upper(std::string_view s);
bool iequals(std::string_view a, std::string_view b);
bool starts_with(std::string_view s, std::string_view p);
bool ends_with(std::string_view s, std::string_view p);
bool iends_with(std::string_view s, std::string_view p);
/// Trim Unicode white space (UTF-8) at both ends.
std::string trim(std::string_view s);
std::string trim_end(std::string_view s);
/// True if the UTF-8 sequence at s[i] is white space; sets `len` to its byte length.
bool ws_at(std::string_view s, size_t i, size_t& len);
std::vector<std::string> split(std::string_view s, char sep);
std::string hex(std::span<const uint8_t> b);
std::optional<uint64_t> parse_u64(std::string_view s);
std::string lossy_utf8(std::span<const uint8_t> b);
/// Append the UTF-8 encoding of code point `c`.
void put_utf8(std::string& out, char32_t c);
/// Lower-case extension without the dot ("" if none).
std::string ext_lower(const fs::path& p);
std::string path_str(const fs::path& p);

// ---- files
class File {
public:
    explicit File(const fs::path& p);
    ~File();
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    /// pread until `len` bytes or EOF; returns the count read.
    size_t read_at(uint8_t* buf, size_t len, uint64_t off) const;
    void read_exact_at(uint8_t* buf, size_t len, uint64_t off) const;
    Bytes read_vec(uint64_t off, size_t len) const;
    uint64_t size() const;

private:
    int fd_;
    std::string name_;
};

Bytes read_file(const fs::path& p);
std::optional<Bytes> try_read_file(const fs::path& p);
std::optional<std::string> try_read_text(const fs::path& p);
/// A temp path next to `p` unique to this process and thread.
fs::path unique_tmp(const fs::path& p);
void atomic_write(const fs::path& p, std::span<const uint8_t> data);
inline void atomic_write(const fs::path& p, std::string_view s) { atomic_write(p, bytes_of(s)); }

/// Modification time in ns since the epoch, if the path exists.
std::optional<int64_t> mtime_ns(const fs::path& p);
int64_t now_ns();

/// Identity of a source file version.
struct SrcKey {
    uint64_t size = 0;
    int64_t mtime_ns = 0;
    static SrcKey of(const fs::path& p);
    static std::optional<SrcKey> try_of(const fs::path& p);
    bool operator==(const SrcKey&) const = default;
};

// ---- background work
/// Background worker lanes. Decode jobs never wait on other jobs; encode jobs may
/// wait on decode jobs only. With that rule neither lane can deadlock.
enum class Lane { Decode, Encode };
/// Run `task` on a worker pool (each lane has as many threads as the CPU).
void submit(std::function<void()> task, Lane lane = Lane::Decode);
size_t pool_size();

/// Something that keeps decoded or encoded audio around for the readers of the
/// moment (read-ahead, frames shared by neighbouring reads). Once registered, by
/// its first read, a sweeper calls drop_cached() after it has been idle for a while and
/// hands the freed memory back to the system, so an idle mount holds no audio.
class Trimmable {
public:
    virtual ~Trimmable() = default;
    /// Drop cached data that is complete (never wait on in-flight work).
    virtual void drop_cached() const = 0;

protected:
    /// Note a read of `self` (registers it with the sweeper the first time).
    void touch(const std::shared_ptr<const Trimmable>& self) const;

private:
    friend struct Sweeper;
    mutable std::atomic<int64_t> last_use_{0};
    mutable std::atomic<int64_t> trimmed_at_{0};  // last_use_ when last trimmed
    mutable std::atomic<bool> registered_{false};
};
/// A file mapped read-only into memory: its bytes, kept valid by `owner` (even if
/// the file is replaced or deleted later). Pages are loaded when touched and can
/// be dropped by the kernel again, so mapped data costs memory only while in use.
struct MappedFile {
    std::shared_ptr<const void> owner;
    std::span<const uint8_t> bytes;
};
std::optional<MappedFile> map_file(const fs::path& p);

/// Return freed heap memory to the system (after work that allocated and freed a lot).
void trim_heap();
/// Trim what has been idle for `idle` now (the sweeper does this on its own);
/// returns how many objects were trimmed.
size_t sweep_idle(std::chrono::nanoseconds idle);

// ---- logging (level from WAVEMORPH_LOG: debug, info, warn, error)
enum class Level { Debug, Info, Warn, Error };
void log_init();
bool log_enabled(Level l);
void log_line(Level l, const std::string& msg);
template <class... A> void debug(std::format_string<A...> f, A&&... a) { if (log_enabled(Level::Debug)) log_line(Level::Debug, std::format(f, std::forward<A>(a)...)); }
template <class... A> void info(std::format_string<A...> f, A&&... a) { if (log_enabled(Level::Info)) log_line(Level::Info, std::format(f, std::forward<A>(a)...)); }
template <class... A> void warn(std::format_string<A...> f, A&&... a) { if (log_enabled(Level::Warn)) log_line(Level::Warn, std::format(f, std::forward<A>(a)...)); }

// ---- child processes
struct ProcResult {
    int status = -1;  // exit code, or -1 if it could not run / was killed
    std::string out, err;
    bool ok() const { return status == 0; }
};
/// Run argv[0] (looked up in PATH) capturing stdout and stderr.
ProcResult run(const std::vector<std::string>& argv);
/// Run with inherited stdio; returns the exit code (-1 on failure to run).
int run_inherit(const std::vector<std::string>& argv);

}  // namespace wm
