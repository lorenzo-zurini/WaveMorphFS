// SPDX-License-Identifier: GPL-3.0-or-later
// WaveMorphFS - present pristine audio files as split, tagged tracks via FUSE.
//
// Copyright (C) 2026 Lorenzo Zurini
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version. It is distributed WITHOUT ANY WARRANTY; see the GNU General Public
// License (LICENSE) for details.

#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <set>

#include <nlohmann/json.hpp>

#include "fs.hpp"
#include "library.hpp"
#include "util.hpp"
#include "version.hpp"
#include "writeback.hpp"

using namespace wm;

namespace {

const char* USAGE = R"USAGE(wavemorphfs - present pristine audio files as split, tagged tracks via FUSE

usage: wavemorphfs [global options] <command> [arguments]
       wavemorphfs --version | --help

commands:
  mount <mountpoint>        mount the filesystem (runs in the foreground)
      --workers N           background workers (default 2)
      --threads N           FUSE threads (default 8)
      --allow-other         let other users (e.g. a container's user) access the mount
      --prescan SECONDS     re-walk the library this often (default 900; 0: never)
      --sacd-multichannel   also expose SACD multichannel areas as "(Multichannel)" albums
                            (env WAVEMORPH_SACD_MULTICHANNEL; default off)
  ls <dir>                  show what a source directory looks like through the filesystem
  verify <dir>              validate every virtual track of a source directory with the reference decoders
  compare <dir> <split dir> compare the virtual tracks of DIR with already-split files (PCM MD5)
  tags-init <dir> [--force] write a sidecar pre-filled with the current effective tags of DIR

global options:
  --root NAME=PATH          library root, shown as folder NAME of the mount (repeatable, required)
  --tags-dir DIR            sidecar tag trees: <tags-dir>/<root name>/<relative dir>/wavemorph.json
                            (env WAVEMORPH_TAGS_DIR; default $XDG_DATA_HOME/wavemorphfs/tags)
  --cache-dir DIR           frame/packet indexes
                            (env WAVEMORPH_CACHE_DIR; default $XDG_CACHE_HOME/wavemorphfs)
  --covers-dir DIR          base of relative sidecar "_cover" paths (env WAVEMORPH_COVERS_DIR)
  --                        end of options (for directory names starting with -)

Directories given to ls, verify, compare and tags-init are matched against the
roots as written, without resolving symbolic links.

environment: WAVEMORPH_LOG=debug|info|warn|error
)USAGE";

/// "~/x" -> $HOME/x; empty if HOME is not set.
std::string expand(const std::string& p) {
    if (!starts_with(p, "~/")) return p;
    const char* home = std::getenv("HOME");
    return home && *home ? (fs::path(home) / p.substr(2)).string() : "";
}

/// $VAR if set, else $XDG_VAR (falling back to ~/FALLBACK) / SUB; empty if none applies.
std::string env_dir(const char* var, const char* xdg, const char* fallback, const char* sub) {
    if (const char* v = std::getenv(var); v && *v) return v;
    const char* x = std::getenv(xdg);
    std::string base = x && *x ? std::string(x) : expand(std::string("~/") + fallback);
    return base.empty() ? "" : (fs::path(base) / sub).string();
}

bool env_true(const char* name) {
    const char* v = std::getenv(name);
    if (!v) return false;
    std::string s = lower(trim(v));
    return !(s.empty() || s == "0" || s == "false" || s == "f" || s == "n" || s == "no" || s == "off");
}

struct Cli {
    std::vector<Root> roots;
    std::string tags_dir = env_dir("WAVEMORPH_TAGS_DIR", "XDG_DATA_HOME", ".local/share", "wavemorphfs/tags");
    std::string cache_dir = env_dir("WAVEMORPH_CACHE_DIR", "XDG_CACHE_HOME", ".cache", "wavemorphfs");
    std::string covers_dir = std::getenv("WAVEMORPH_COVERS_DIR") ? std::getenv("WAVEMORPH_COVERS_DIR") : "";
    std::string cmd;
    int (*run)(const Cli&) = nullptr;
    std::vector<std::string> pos;
    size_t workers = 2, threads = 8;
    uint64_t prescan = 900;
    bool allow_other = false, sacd_multichannel = false, force = false;
};

[[noreturn]] void usage_error(const std::string& msg) {
    std::fprintf(stderr, "error: %s\n\n%s", msg.c_str(), USAGE);
    std::exit(2);
}

int cmd_mount(const Cli& cli);
int cmd_ls(const Cli& cli);
int cmd_verify(const Cli& cli);
int cmd_compare(const Cli& cli);
int cmd_tags_init(const Cli& cli);

struct Command {
    std::string_view name;
    size_t args;
    const char* args_help;  // for "needs ..." errors
    int (*run)(const Cli&);
};
const Command COMMANDS[] = {
    {"mount", 1, "a mountpoint", cmd_mount},
    {"ls", 1, "a directory", cmd_ls},
    {"verify", 1, "a directory", cmd_verify},
    {"compare", 2, "a directory and a split directory", cmd_compare},
    {"tags-init", 1, "a directory", cmd_tags_init},
};

/// Options that only one command takes.
const std::pair<std::string_view, std::string_view> COMMAND_OPTIONS[] = {
    {"--workers", "mount"}, {"--threads", "mount"}, {"--allow-other", "mount"}, {"--prescan", "mount"},
    {"--sacd-multichannel", "mount"}, {"--force", "tags-init"},
};

Cli parse_args(int argc, char** argv) {
    Cli c;
    c.sacd_multichannel = env_true("WAVEMORPH_SACD_MULTICHANNEL");
    std::vector<std::string> a(argv + 1, argv + argc);
    std::set<std::string> given;
    bool options = true;
    for (size_t i = 0; i < a.size(); i++) {
        std::string arg = a[i], val;
        if (!options || arg == "-" || !starts_with(arg, "-")) {
            if (c.cmd.empty()) c.cmd = arg;
            else c.pos.push_back(arg);
            continue;
        }
        if (arg == "--") {
            options = false;
            continue;
        }
        bool has_val = false;
        if (auto eq = arg.find('='); starts_with(arg, "--") && eq != std::string::npos) {
            val = arg.substr(eq + 1);
            arg = arg.substr(0, eq);
            has_val = true;
        }
        given.insert(arg);
        auto value = [&]() -> std::string {
            if (has_val) return val;
            if (i + 1 >= a.size()) usage_error(arg + " needs a value");
            return a[++i];
        };
        auto flag = [&] {
            if (has_val) usage_error(arg + " takes no value");
            return true;
        };
        auto number = [&](uint64_t min) -> uint64_t {
            std::string v = value();
            auto n = parse_u64(v);
            if (!n) usage_error(arg + ": not a number: " + v);
            if (*n < min) usage_error(std::format("{} must be at least {}", arg, min));
            return *n;
        };
        if (arg == "-h" || arg == "--help") {
            std::fputs(USAGE, stdout);
            std::exit(0);
        } else if (arg == "-V" || arg == "--version") {
            std::printf("wavemorphfs %s\n", VERSION);
            std::exit(0);
        } else if (arg == "--root") {
            std::string v = value();
            auto eq = v.find('=');
            if (eq == std::string::npos) usage_error("--root expects NAME=PATH");
            c.roots.push_back({v.substr(0, eq), v.substr(eq + 1)});
        } else if (arg == "--tags-dir") c.tags_dir = value();
        else if (arg == "--cache-dir") c.cache_dir = value();
        else if (arg == "--covers-dir") c.covers_dir = value();
        else if (arg == "--workers") c.workers = size_t(number(1));
        else if (arg == "--threads") c.threads = size_t(number(1));
        else if (arg == "--prescan") c.prescan = number(0);
        else if (arg == "--allow-other") c.allow_other = flag();
        else if (arg == "--sacd-multichannel") c.sacd_multichannel = flag();
        else if (arg == "--force") c.force = flag();
        else usage_error("unknown option " + arg);
    }
    if (c.cmd == "help") {
        std::fputs(USAGE, stdout);
        std::exit(0);
    }
    if (c.cmd.empty()) usage_error("no command given");
    auto cmd = std::find_if(std::begin(COMMANDS), std::end(COMMANDS), [&](auto& k) { return k.name == c.cmd; });
    if (cmd == std::end(COMMANDS)) usage_error("unknown command " + c.cmd);
    c.run = cmd->run;
    if (c.pos.size() != cmd->args) usage_error(std::format("{} needs {}", c.cmd, cmd->args_help));
    for (auto& [opt, only] : COMMAND_OPTIONS)
        if (given.contains(std::string(opt)) && c.cmd != only) usage_error(std::format("{} only applies to {}", opt, only));
    if (c.roots.empty()) usage_error("no --root given");
    return c;
}

/// Absolute and lexically normal, without a trailing separator. Symlinks are
/// not resolved: source paths are cache keys and must stay as configured.
fs::path normal_path(const std::string& p) {
    fs::path n = fs::absolute(p).lexically_normal();
    return n.has_filename() || n == n.root_path() ? n : n.parent_path();
}

/// Roots as the library needs them: distinct, reachable names (a top-level
/// directory of the mount and of the tags tree) and normal, disjoint paths
/// (source folders are matched to their root lexically).
std::vector<Root> checked_roots(const std::vector<Root>& given) {
    std::vector<Root> roots;
    for (auto& g : given) {
        WM_ENSURE(!g.name.empty() && g.name.find('/') == std::string::npos && !starts_with(g.name, "."),
                  "root name '{}': must be non-empty, without '/' and not start with '.'", g.name);
        std::string p = expand(g.path.string());
        WM_ENSURE(!p.empty(), "root {}: HOME is not set for {}", g.name, g.path.string());
        Root r{g.name, normal_path(p)};
        WM_ENSURE(fs::is_directory(r.path), "root {} does not exist: {}", r.name, r.path.string());
        for (auto& o : roots) {
            WM_ENSURE(o.name != r.name, "root name {} given twice", r.name);
            auto inside = [](const fs::path& a, const fs::path& b) {
                auto rel = a.lexically_relative(b);
                return !rel.empty() && *rel.begin() != "..";
            };
            WM_ENSURE(!inside(r.path, o.path) && !inside(o.path, r.path), "roots {} and {} overlap", o.name, r.name);
        }
        roots.push_back(std::move(r));
    }
    return roots;
}

std::shared_ptr<Library> library(const Cli& cli, size_t workers) {
    Config cfg;
    cfg.roots = checked_roots(cli.roots);
    auto dir = [](const std::string& given, const char* what) {
        std::string p = expand(given);
        WM_ENSURE(!p.empty(), "no {} (give --{} or set HOME)", what, what);
        return fs::path(p);
    };
    cfg.tags_dir = dir(cli.tags_dir, "tags-dir");
    cfg.cache_dir = dir(cli.cache_dir, "cache-dir");
    if (!cli.covers_dir.empty()) cfg.covers_dir = dir(cli.covers_dir, "covers-dir");
    cfg.workers = workers;
    cfg.sacd_multichannel = cli.cmd == "mount" && cli.sacd_multichannel;
    return Library::create(std::move(cfg));
}

/// A source directory named on the command line, as the library knows it.
fs::path source_dir(const std::string& p) {
    fs::path d = normal_path(p);
    WM_ENSURE(fs::is_directory(d), "not a directory: {}", p);
    return d;
}

/// A private temporary directory, removed with everything in it.
struct TempDir {
    fs::path path = fs::temp_directory_path() / std::format("wavemorphfs-{}", ::getpid());
    TempDir() { fs::create_directories(path); }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

void dump(const VFile& vf, const fs::path& out) {
    std::unique_ptr<FILE, int (*)(FILE*)> f(std::fopen(out.c_str(), "wb"), std::fclose);
    WM_ENSURE(f, "create {}: {}", out.string(), std::strerror(errno));
    for (uint64_t off = 0; off < vf.size();) {
        Bytes b = vf.read_at(off, 1 << 20);
        WM_ENSURE(!b.empty(), "short read at {}", off);
        WM_ENSURE(std::fwrite(b.data(), 1, b.size(), f.get()) == b.size(), "write {}: {}", out.string(), std::strerror(errno));
        off += b.size();
    }
    WM_ENSURE(std::fclose(f.release()) == 0, "write {}: {}", out.string(), std::strerror(errno));
}

/// Run a checker; with `strict_stderr`, any stderr output counts as failure.
bool run_ok(const std::vector<std::string>& argv, bool strict_stderr) {
    auto r = run(argv);
    return r.ok() && (!strict_stderr || r.err.empty());
}

/// MD5 of a file's audio packets (stream copy, no decoding).
std::optional<std::string> stream_md5(const fs::path& p) {
    auto r = run({"ffmpeg", "-v", "error", "-i", p.string(), "-map", "0:a", "-c", "copy", "-f", "md5", "-"});
    if (!r.ok()) return std::nullopt;
    return trim(r.out);
}

std::string pcm_md5(const fs::path& p) {
    auto r = run({"sh", "-c", "set -o pipefail; flac -d -c -s --force-raw-format --endian=little --sign=signed \"$1\" | md5sum", "sh", p.string()});
    if (!r.ok()) return "";
    auto w = split(trim(r.out), ' ');
    return w.empty() ? "" : w[0];
}

int cmd_ls(const Cli& cli) {
    auto lib = library(cli, 0);
    auto l = lib->list_dir(source_dir(cli.pos[0]));
    for (auto& e : l->entries) {
        if (e.is_dir) std::cout << std::format("{:>14}  {}/\n", "<dir>", e.name);
        else std::cout << std::format("{:>14}  {}\n{:>16}{}\n", e.file->size(), e.name, "", e.file->describe());
    }
    return 0;
}

/// How verify checks a kind of file, by the prefix of VFile::describe().
enum class Check {
    Flac,       // flac -t: every frame's CRC and the stream MD5
    Decode,     // decodes cleanly with ffmpeg
    DecodeCrc,  // decodes with every block's audio CRC checked
    SameAudio,  // retagged: the same compressed audio as the source, and it decodes
};
const std::pair<std::string_view, Check> CHECKS[] = {
    {"flac-image:", Check::Flac},         {"flac-encoded:", Check::Flac},      {"retagged-flac:", Check::Flac},
    {"sacd:", Check::Decode},             {"wavpack-image:", Check::DecodeCrc}, {"retagged-mp3:", Check::SameAudio},
    {"retagged-m4a:", Check::SameAudio},  {"retagged-wv:", Check::SameAudio},  {"retagged-dsf:", Check::SameAudio},
};

bool verify_file(Check check, const fs::path& out, const std::string& describe) {
    auto decodes = [&](bool crc) {
        std::vector<std::string> argv = {"ffmpeg", "-v", "error", "-xerror"};
        if (crc) argv.insert(argv.end(), {"-err_detect", "crccheck"});
        argv.insert(argv.end(), {"-i", out.string(), "-f", "null", "-"});
        return argv;
    };
    switch (check) {
    case Check::Flac: return run_ok({"flac", "-t", "-s", out.string()}, false);
    case Check::Decode: return run_ok(decodes(false), true);
    case Check::DecodeCrc: return run_ok(decodes(true), true);
    case Check::SameAudio: {
        auto a = stream_md5(out);
        return a && a == stream_md5(describe.substr(describe.find(':') + 1)) && run_ok(decodes(false), false);
    }
    }
    return false;
}

int cmd_verify(const Cli& cli) {
    auto lib = library(cli, 0);
    auto l = lib->list_dir(source_dir(cli.pos[0]));
    TempDir tmp;
    int ok = 0, n = 0, skipped = 0;
    for (auto& e : l->entries) {
        if (e.is_dir) continue;
        std::string d = e.file->describe();
        auto c = std::find_if(std::begin(CHECKS), std::end(CHECKS), [&](auto& kc) { return starts_with(d, kc.first); });
        if (c == std::end(CHECKS)) {  // passed through unchanged
            skipped++;
            continue;
        }
        n++;
        fs::path out = tmp.path / e.name;
        dump(*e.file, out);
        bool good = verify_file(c->second, out, d);
        if (good) ok++;
        std::cout << std::format("{}  {:>11}  {}\n", good ? "OK  " : "FAIL", e.file->size(), e.name) << std::flush;
        std::error_code ec;
        fs::remove(out, ec);
    }
    std::cout << std::format("{}/{} virtual tracks valid ({} files passed through unchanged)\n", ok, n, skipped);
    return ok == n ? 0 : 1;
}

int cmd_compare(const Cli& cli) {
    auto lib = library(cli, 0);
    auto l = lib->list_dir(source_dir(cli.pos[0]));
    std::vector<const Entry*> virt;
    for (auto& e : l->entries)
        if (!e.is_dir && (starts_with(e.file->describe(), "flac-image:") || starts_with(e.file->describe(), "flac-encoded:"))) virt.push_back(&e);
    std::vector<fs::path> split_files;
    for (auto& de : fs::directory_iterator(cli.pos[1]))
        if (de.is_regular_file() && iequals(de.path().extension().string(), ".flac")) split_files.push_back(de.path());
    std::sort(split_files.begin(), split_files.end());
    WM_ENSURE(virt.size() == split_files.size(), "{} virtual tracks vs {} split files", virt.size(), split_files.size());
    TempDir tmp;
    size_t same = 0;
    for (size_t i = 0; i < virt.size(); i++) {
        fs::path out = tmp.path / virt[i]->name;
        dump(*virt[i]->file, out);
        std::string a = pcm_md5(out), b = pcm_md5(split_files[i]);
        std::error_code ec;
        fs::remove(out, ec);
        bool eq = !a.empty() && a == b;
        if (eq) same++;
        std::cout << std::format("{}  {}  <->  {}\n", eq ? "IDENTICAL" : "DIFFERENT", virt[i]->name, split_files[i].filename().string()) << std::flush;
    }
    std::cout << std::format("{}/{} identical\n", same, virt.size());
    return same == virt.size() ? 0 : 1;
}

/// Keys starting with "_" anywhere a sidecar may have them ("_target", "_hide",
/// "_cover", "_name", ...): a freshly rendered sidecar would drop them silently.
std::vector<std::string> directives(const std::string& sidecar_text) {
    using json = nlohmann::ordered_json;
    json doc = json::parse(sidecar_text, nullptr, /*allow_exceptions=*/false, /*ignore_comments=*/true);
    std::vector<std::string> out;
    if (!doc.is_object()) return out;
    auto scan = [&](const json& obj, const std::string& where) {
        if (!obj.is_object()) return;
        for (auto& [k, v] : obj.items())
            if (starts_with(k, "_")) out.push_back(where + k);
    };
    scan(doc, "");
    scan(doc.value("album", json()), "album.");
    for (auto section : {"track", "file"})
        if (auto s = doc.find(section); s != doc.end() && s->is_object())
            for (auto& [k, v] : s->items()) scan(v, std::format("{}.\"{}\".", section, k));
    return out;
}

int cmd_tags_init(const Cli& cli) {
    auto lib = library(cli, 0);
    fs::path dir = source_dir(cli.pos[0]);
    auto ov = lib->overlay_dir(dir);
    WM_ENSURE(ov.has_value(), "{} is not inside a configured root", dir.string());
    fs::path target = *ov / SIDECAR_NAME;
    if (auto text = try_read_text(target)) {
        WM_ENSURE(cli.force, "{} exists (use --force)", target.string());
        std::string d;
        for (auto& k : directives(*text)) d += (d.empty() ? "" : ", ") + k;
        WM_ENSURE(d.empty(), "{} has settings a new sidecar would lose ({}): edit it by hand", target.string(), d);
    }
    auto l = lib->list_dir(dir);
    std::vector<std::pair<std::string, Tags>> per;
    for (auto& e : l->entries) {
        if (e.is_dir || e.tag_ext.empty()) continue;
        try {
            per.emplace_back(e.name, read_file_tags(e.tag_ext, [&](uint64_t o, size_t n) { return e.file->read_at(o, n); }, e.file->size()));
        } catch (const std::exception& ex) {
            warn("{}: {}", e.name, ex.what());
        }
    }
    WM_ENSURE(!per.empty(), "no taggable audio files in {}", dir.string());
    Tags album = per[0].second;
    std::erase_if(album.m, [&](auto& kv) {
        if (is_track_specific(kv.first)) return true;
        return !std::all_of(per.begin(), per.end(), [&](auto& pt) {
            auto v = pt.second.get_all(kv.first);
            return v && *v == kv.second;
        });
    });
    for (auto& [n, t] : per) std::erase_if(t.m, [&](auto& kv) { return album.get_all(kv.first) != nullptr; });
    fs::create_directories(*ov);
    atomic_write(target, Sidecar::render(album, {}, per));
    std::cout << "wrote " << target.string() << "\n";
    return 0;
}

int cmd_mount(const Cli& cli) {
    info("WaveMorphFS {}", VERSION);
    auto lib = library(cli, cli.workers);
    lib->start_workers();
    lib->refresh_placements(true);  // the tree is complete from the first request
    if (cli.prescan > 0) lib->start_prescan(std::chrono::seconds(cli.prescan));
    int rc = mount_fs(lib, {cli.pos[0], cli.threads, cli.allow_other});
    // background workers and the prescan are detached and may be mid-job: leave
    // without running static destructors under them. Nothing is lost: every
    // file is written atomically.
    std::fflush(nullptr);
    std::_Exit(rc);
}

}  // namespace

int main(int argc, char** argv) {
    log_init();
    Cli cli = parse_args(argc, argv);
    try {
        return cli.run(cli);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
