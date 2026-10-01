// SPDX-License-Identifier: GPL-3.0-or-later
#include "writeback.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

#include "flac.hpp"
#include "id3.hpp"
#include "mp4.hpp"

namespace wm {

Tags read_file_tags(const std::string& ext, const ByteReader& read, uint64_t size) {
    Tags t;
    if (ext == "flac") {
        Bytes h = read(0, 4);
        if (h.size() < 4 || std::memcmp(h.data(), "fLaC", 4) != 0) return t;
        uint64_t pos = 4;
        while (pos + 4 <= size) {
            Bytes bh = read(pos, 4);
            if (bh.size() < 4) break;
            bool last = bh[0] & 0x80;
            size_t len = size_t(bh[1]) << 16 | size_t(bh[2]) << 8 | bh[3];
            if ((bh[0] & 0x7F) == flac::BLOCK_VORBIS)
                for (auto& [k, v] : flac::parse_vorbis(read(pos + 4, len))) t.add(k, v);
            pos += 4 + len;
            if (last) break;
        }
        return t;
    }
    auto id3_at = [&](uint64_t pos) {
        Bytes h = read(pos, 10);
        if (h.size() < 10 || std::memcmp(h.data(), "ID3", 3) != 0) return Tags{};
        uint64_t sz = 0;
        for (int i = 6; i < 10; i++) sz = sz << 7 | (h[size_t(i)] & 0x7F);
        return id3::read_tags(read(pos, size_t(10 + sz)));
    };
    if (ext == "dsf") {
        Bytes h = read(0, 28);
        if (h.size() < 28 || std::memcmp(h.data(), "DSD ", 4) != 0) return t;
        uint64_t meta = le64(&h[20]);
        return meta ? id3_at(meta) : t;
    }
    if (ext == "mp3") return id3_at(0);
    if (ext == "m4a") return mp4::read_tags(read, size);
    return t;
}

Tags tag_changes(const Tags& before, const Tags& after) {
    struct Field {
        std::string name;
        std::vector<std::string> values;  // de-duplicated across synonyms, in order
    };
    auto group = [](const Tags& t) {
        std::map<std::string, Field> g;
        for (auto& [k, v] : t.m) {
            auto& f = g[canonical_field(k)];
            if (f.name.empty()) f.name = k;
            for (auto& x : v)
                if (std::find(f.values.begin(), f.values.end(), x) == f.values.end()) f.values.push_back(x);
        }
        return g;
    };
    // values editors write for "unknown" (beets writes BPM 0, DISC 0, ORIGINALDATE 0000...)
    auto placeholder = [](const std::vector<std::string>& vs) {
        return std::all_of(vs.begin(), vs.end(), [](const std::string& v) {
            return v.empty() || v.find_first_not_of("0/-") == std::string::npos;
        });
    };
    auto b = group(before), a = group(after);
    auto year_of = [&](const char* date_field) -> std::string {
        for (auto* g : {&a, &b})
            if (auto it = g->find(date_field); it != g->end() && !it->second.values.empty()) return it->second.values[0].substr(0, 4);
        return "";
    };
    Tags out;
    for (auto& [c, f] : a) {
        auto it = b.find(c);
        if (it != b.end()) {
            auto x = it->second.values, y = f.values;
            std::sort(x.begin(), x.end());
            std::sort(y.begin(), y.end());
            if (x == y) continue;
        } else {
            if (placeholder(f.values)) continue;
            // a YEAR that only repeats DATE's year is redundant, not an edit
            if ((c == "YEAR" && f.values.size() == 1 && f.values[0] == year_of("DATE")) ||
                (c == "ORIGINALYEAR" && f.values.size() == 1 && f.values[0] == year_of("ORIGINALDATE")))
                continue;
        }
        out.m.emplace(f.name, f.values);
    }
    for (auto& [c, f] : b)
        if (!a.contains(c)) out.m.emplace(f.name, std::vector<std::string>{});
    return out;
}

WriteSession::WriteSession(VFilePtr original, const fs::path& scratch_dir) : original_(std::move(original)) {
    size_ = original_limit_ = original_->size();
    fs::create_directories(scratch_dir);
    std::string tmpl = (scratch_dir / "edit.XXXXXX").string();
    fd_ = ::mkstemp(tmpl.data());
    if (fd_ < 0) fail("scratch file in {}: {}", scratch_dir.string(), std::strerror(errno));
    ::unlink(tmpl.c_str());  // anonymous: gone when closed
}

WriteSession::~WriteSession() {
    if (fd_ >= 0) ::close(fd_);
}

uint64_t WriteSession::size() const {
    std::lock_guard g(mu_);
    return size_;
}

bool WriteSession::dirty() const {
    std::lock_guard g(mu_);
    return dirty_;
}

void WriteSession::mark_clean() {
    std::lock_guard g(mu_);
    dirty_ = false;
}

Bytes WriteSession::read(uint64_t off, size_t len) const {
    std::lock_guard g(mu_);
    return read_locked(off, len);
}

Bytes WriteSession::read_locked(uint64_t off, size_t len) const {
    Bytes out;
    if (off >= size_) return out;
    uint64_t end = off + std::min<uint64_t>(len, size_ - off);
    out.reserve(size_t(end - off));
    uint64_t pos = off;
    while (pos < end) {
        // a written range covering pos?
        auto it = written_.upper_bound(pos);
        if (it != written_.begin() && std::prev(it)->second > pos) {
            auto w = std::prev(it);
            uint64_t stop = std::min(end, w->second);
            size_t n = size_t(stop - pos), old = out.size();
            out.resize(old + n);
            ssize_t r = ::pread(fd_, out.data() + old, n, off_t(pos));
            if (r != ssize_t(n)) fail("scratch read: {}", std::strerror(errno));
            pos = stop;
            continue;
        }
        uint64_t stop = it == written_.end() ? end : std::min(end, it->first);
        // original bytes below the limit, zeros beyond
        if (pos < original_limit_) {
            uint64_t s2 = std::min(stop, original_limit_);
            Bytes b = original_->read_at(pos, size_t(s2 - pos));
            if (b.size() != s2 - pos) fail("short read of the original");
            out.insert(out.end(), b.begin(), b.end());
            pos = s2;
        } else {
            out.insert(out.end(), size_t(stop - pos), 0);
            pos = stop;
        }
    }
    return out;
}

void WriteSession::write(uint64_t off, std::span<const uint8_t> data) {
    if (data.empty()) return;
    std::lock_guard g(mu_);
    size_t done = 0;
    while (done < data.size()) {
        ssize_t r = ::pwrite(fd_, data.data() + done, data.size() - done, off_t(off + done));
        if (r < 0) {
            if (errno == EINTR) continue;
            fail("scratch write: {}", std::strerror(errno));
        }
        done += size_t(r);
    }
    // merge [off, end) into the written ranges
    uint64_t s = off, e = off + data.size();
    auto it = written_.upper_bound(s);
    if (it != written_.begin() && std::prev(it)->second >= s) --it;
    while (it != written_.end() && it->first <= e) {
        s = std::min(s, it->first);
        e = std::max(e, it->second);
        it = written_.erase(it);
    }
    written_[s] = e;
    size_ = std::max(size_, e);
    dirty_ = true;
}

void WriteSession::truncate(uint64_t n) {
    std::lock_guard g(mu_);
    for (auto it = written_.begin(); it != written_.end();) {
        if (it->first >= n) {
            it = written_.erase(it);
        } else {
            it->second = std::min(it->second, n);
            ++it;
        }
    }
    original_limit_ = std::min(original_limit_, n);
    if (::ftruncate(fd_, off_t(n)) != 0) fail("scratch truncate: {}", std::strerror(errno));
    size_ = n;
    dirty_ = true;
}

}  // namespace wm
