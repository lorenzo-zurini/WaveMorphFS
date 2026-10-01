// SPDX-License-Identifier: GPL-3.0-or-later
// WaveMorphFS - present pristine audio downloads as split, tagged tracks via FUSE.
//
// Copyright (C) 2026 Lorenzo Zurini
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version. It is distributed WITHOUT ANY WARRANTY; see the GNU General Public
// License (LICENSE) for details.

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>

#include "flac.hpp"
#include "fs.hpp"
#include "library.hpp"
#include "util.hpp"

using namespace wm;

namespace {

const char* USAGE = R"USAGE(wavemorphfs - present pristine audio downloads as split, tagged tracks via FUSE

usage: wavemorphfs [global options] <command> [arguments]

commands:
  mount <mountpoint>        mount the filesystem (runs in the foreground)
      --workers N           background workers (default 2)
      --threads N           FUSE threads (default 8)
      --allow-other         let other users (e.g. a container's user) access the mount
      --prescan SECONDS     re-walk the library this often (default 900)
      --sacd-multichannel   also expose SACD multichannel areas as "(Multichannel)" albums
                            (env WAVEMORPH_SACD_MULTICHANNEL; default off)
  ls <dir>                  show what a source directory looks like through the filesystem
  verify <dir>              validate every virtual track of a source directory with the reference decoders
  compare <dir> <split dir> compare the virtual tracks of DIR with already-split files (PCM MD5)
  tags-init <dir> [--force] write a sidecar pre-filled with the current effective tags of DIR

global options:
  --root NAME=PATH          library root (repeatable; default ~/Storage/Music, ~/Storage/Classical Music)
  --tags-dir DIR            sidecar tag trees: <tags-dir>/<root name>/<relative dir>/wavemorph.json
                            (default ~/Storage/WaveMorph/tags)
  --cache-dir DIR           frame indexes, converted images (default ~/Storage/WaveMorph/cache)

environment: WAVEMORPH_LOG=debug|info|warn|error
)USAGE";

fs::path expand(const std::string& p) {
    if (starts_with(p, "~/")) {
        const char* home = std::getenv("HOME");
        return fs::path(home ? home : "") / p.substr(2);
    }
    return p;
}

bool env_true(const char* name) {
    const char* v = std::getenv(name);
    if (!v) return false;
    std::string s = lower(trim(v));
    return !(s.empty() || s == "0" || s == "false" || s == "f" || s == "n" || s == "no" || s == "off");
}

struct Cli {
    std::vector<Root> roots;
    std::string tags_dir = "~/Storage/WaveMorph/tags";
    std::string cache_dir = "~/Storage/WaveMorph/cache";
    std::string cmd;
    std::vector<std::string> pos;
    size_t workers = 2, threads = 8;
    uint64_t prescan = 900;
    bool allow_other = false, sacd_multichannel = false, force = false;
};

[[noreturn]] void usage_error(const std::string& msg) {
    std::fprintf(stderr, "error: %s\n\n%s", msg.c_str(), USAGE);
    std::exit(2);
}

Cli parse_args(int argc, char** argv) {
    Cli c;
    c.sacd_multichannel = env_true("WAVEMORPH_SACD_MULTICHANNEL");
    std::vector<std::string> a(argv + 1, argv + argc);
    for (size_t i = 0; i < a.size(); i++) {
        std::string arg = a[i], val;
        bool has_val = false;
        if (starts_with(arg, "--") && arg.find('=') != std::string::npos) {
            val = arg.substr(arg.find('=') + 1);
            arg = arg.substr(0, arg.find('='));
            has_val = true;
        }
        auto value = [&]() -> std::string {
            if (has_val) return val;
            if (i + 1 >= a.size()) usage_error(arg + " needs a value");
            return a[++i];
        };
        auto number = [&]() -> uint64_t {
            std::string v = value();
            auto n = parse_u64(v);
            if (!n) usage_error(arg + ": not a number: " + v);
            return *n;
        };
        if (arg == "-h" || arg == "--help" || arg == "help") {
            std::fputs(USAGE, stdout);
            std::exit(0);
        } else if (arg == "--root") {
            std::string v = value();
            auto eq = v.find('=');
            if (eq == std::string::npos) usage_error("--root expects NAME=PATH");
            c.roots.push_back({v.substr(0, eq), expand(v.substr(eq + 1))});
        } else if (arg == "--tags-dir") c.tags_dir = value();
        else if (arg == "--cache-dir") c.cache_dir = value();
        else if (arg == "--workers") c.workers = size_t(number());
        else if (arg == "--threads") c.threads = size_t(number());
        else if (arg == "--prescan") c.prescan = number();
        else if (arg == "--allow-other") c.allow_other = true;
        else if (arg == "--sacd-multichannel") c.sacd_multichannel = true;
        else if (arg == "--force") c.force = true;
        else if (starts_with(arg, "-")) usage_error("unknown option " + arg);
        else if (c.cmd.empty()) c.cmd = arg;
        else c.pos.push_back(arg);
    }
    if (c.cmd.empty()) usage_error("no command given");
    return c;
}

std::shared_ptr<Library> library(const Cli& cli, size_t workers) {
    auto roots = cli.roots;
    if (roots.empty()) roots = {{"Music", expand("~/Storage/Music")}, {"Classical Music", expand("~/Storage/Classical Music")}};
    for (auto& r : roots) WM_ENSURE(fs::is_directory(r.path), "root {} does not exist: {}", r.name, r.path.string());
    Config cfg;
    cfg.roots = roots;
    cfg.tags_dir = expand(cli.tags_dir);
    cfg.cache_dir = expand(cli.cache_dir);
    cfg.workers = workers;
    cfg.sacd_multichannel = cli.cmd == "mount" && cli.sacd_multichannel;
    return Library::create(std::move(cfg));
}

fs::path abs_path(const std::string& p) {
    return with_context(p, [&] { return fs::canonical(p); });
}

fs::path tempdir() {
    fs::path d = fs::temp_directory_path() / std::format("wavemorphfs-{}", ::getpid());
    fs::create_directories(d);
    return d;
}

void dump(const VFile& vf, const fs::path& out) {
    FILE* f = std::fopen(out.c_str(), "wb");
    WM_ENSURE(f, "create {}", out.string());
    uint64_t off = 0;
    while (off < vf.size()) {
        Bytes b = vf.read_at(off, 1 << 20);
        if (b.empty()) {
            std::fclose(f);
            fail("short read at {}", off);
        }
        std::fwrite(b.data(), 1, b.size(), f);
        off += b.size();
    }
    std::fclose(f);
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

/// Tags of a (virtual) FLAC file, read from its generated header.
std::optional<Tags> read_tags(const VFile& vf, const std::string& name) {
    if (!iends_with(name, ".flac")) return std::nullopt;
    Bytes head = vf.read_at(0, 1 << 20);
    if (head.size() < 4 || std::memcmp(head.data(), "fLaC", 4) != 0) return std::nullopt;
    size_t p = 4;
    while (p + 4 <= head.size()) {
        bool last = head[p] & 0x80;
        size_t len = size_t(head[p + 1]) << 16 | size_t(head[p + 2]) << 8 | head[p + 3];
        if ((head[p] & 0x7F) == flac::BLOCK_VORBIS) {
            if (p + 4 + len > head.size()) return std::nullopt;
            return Tags::from_pairs(flac::parse_vorbis(std::span(head).subspan(p + 4, len)));
        }
        if (last) return std::nullopt;
        p += 4 + len;
    }
    return std::nullopt;
}

int cmd_ls(const Cli& cli) {
    if (cli.pos.size() != 1) usage_error("ls needs a directory");
    auto lib = library(cli, 0);
    auto l = lib->list_dir(abs_path(cli.pos[0]));
    for (auto& e : l->entries) {
        if (e.is_dir) std::cout << std::format("{:>14}  {}/\n", "<dir>", e.name);
        else std::cout << std::format("{:>14}  {}\n{:>16}{}\n", e.file->size(), e.name, "", e.file->describe());
    }
    return 0;
}

int cmd_verify(const Cli& cli) {
    if (cli.pos.size() != 1) usage_error("verify needs a directory");
    auto lib = library(cli, 0);
    auto l = lib->list_dir(abs_path(cli.pos[0]));
    auto tmp = tempdir();
    int ok = 0, n = 0;
    for (auto& e : l->entries) {
        if (e.is_dir) continue;
        std::string d = e.file->describe();
        bool retag_other = starts_with(d, "retagged-mp3:") || starts_with(d, "retagged-m4a:");
        if (!(starts_with(d, "flac-image:") || starts_with(d, "sacd:") || starts_with(d, "retagged-flac:") || retag_other)) continue;
        n++;
        fs::path out = tmp / e.name;
        dump(*e.file, out);
        bool good;
        if (retag_other) {
            // same compressed audio as the source, and it decodes
            auto a = stream_md5(out);
            good = a && a == stream_md5(d.substr(d.find(':') + 1)) &&
                   run_ok({"ffmpeg", "-v", "error", "-xerror", "-i", out.string(), "-f", "null", "-"}, false);
        } else if (starts_with(d, "sacd:")) {
            good = run_ok({"ffmpeg", "-v", "error", "-xerror", "-i", out.string(), "-f", "null", "-"}, true);
        } else {
            good = run_ok({"flac", "-t", "-s", out.string()}, false);
        }
        if (good) ok++;
        std::cout << std::format("{}  {:>11}  {}\n", good ? "OK  " : "FAIL", e.file->size(), e.name) << std::flush;
        std::error_code ec;
        fs::remove(out, ec);
    }
    std::cout << std::format("{}/{} virtual tracks valid\n", ok, n);
    return ok == n ? 0 : 1;
}

int cmd_compare(const Cli& cli) {
    if (cli.pos.size() != 2) usage_error("compare needs a directory and a split directory");
    auto lib = library(cli, 0);
    auto l = lib->list_dir(abs_path(cli.pos[0]));
    std::vector<const Entry*> virt;
    for (auto& e : l->entries)
        if (!e.is_dir && starts_with(e.file->describe(), "flac-image:")) virt.push_back(&e);
    std::vector<fs::path> split_files;
    for (auto& de : fs::directory_iterator(cli.pos[1]))
        if (de.is_regular_file() && iequals(de.path().extension().string(), ".flac")) split_files.push_back(de.path());
    std::sort(split_files.begin(), split_files.end());
    WM_ENSURE(virt.size() == split_files.size(), "{} virtual tracks vs {} split files", virt.size(), split_files.size());
    auto tmp = tempdir();
    size_t same = 0;
    for (size_t i = 0; i < virt.size(); i++) {
        fs::path out = tmp / virt[i]->name;
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

int cmd_tags_init(const Cli& cli) {
    if (cli.pos.size() != 1) usage_error("tags-init needs a directory");
    auto lib = library(cli, 0);
    fs::path dir = abs_path(cli.pos[0]);
    auto ov = lib->overlay_dir(dir);
    WM_ENSURE(ov.has_value(), "directory is not inside a configured root");
    fs::path target = *ov / SIDECAR_NAME;
    WM_ENSURE(!fs::exists(target) || cli.force, "{} exists (use --force)", target.string());
    auto l = lib->list_dir(dir);
    std::vector<std::pair<std::string, Tags>> per;
    for (auto& e : l->entries)
        if (!e.is_dir)
            if (auto t = read_tags(*e.file, e.name)) per.emplace_back(e.name, *t);
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

}  // namespace

int main(int argc, char** argv) {
    log_init();
    Cli cli = parse_args(argc, argv);
    try {
        if (cli.cmd == "mount") {
            if (cli.pos.size() != 1) usage_error("mount needs a mountpoint");
            auto lib = library(cli, cli.workers);
            lib->start_workers();
            lib->start_prescan(std::chrono::seconds(cli.prescan));
            return mount_fs(lib, {cli.pos[0], cli.threads, cli.allow_other});
        }
        if (cli.cmd == "ls") return cmd_ls(cli);
        if (cli.cmd == "verify") return cmd_verify(cli);
        if (cli.cmd == "compare") return cmd_compare(cli);
        if (cli.cmd == "tags-init") return cmd_tags_init(cli);
        usage_error("unknown command " + cli.cmd);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Error: %s\n", e.what());
        return 1;
    }
}
