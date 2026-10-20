// SPDX-License-Identifier: GPL-3.0-or-later
#include "writeback.hpp"

#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>

#include "apetag.hpp"
#include "flac.hpp"
#include "id3.hpp"
#include "mp4.hpp"

namespace wm {

namespace {

/// The top-level MP4 boxes include moov and none runs past the end (a file cut
/// short while being written has one that does).
bool mp4_complete(const ByteReader& read, uint64_t size) {
    bool moov = false;
    uint64_t pos = 0;
    while (pos + 8 <= size) {
        Bytes h = read(pos, 16);
        if (h.size() < 8) return false;
        uint64_t sz = be32(h.data());
        if (sz == 1) sz = h.size() >= 16 ? be64(&h[8]) : 0;
        else if (sz == 0) sz = size - pos;  // extends to the end
        if (sz < 8 || sz > size - pos) return false;
        moov |= std::memcmp(&h[4], "moov", 4) == 0;
        pos += sz;
    }
    return moov;
}

}  // namespace

Tags read_file_tags(const std::string& ext, const ByteReader& read, uint64_t size) {
    auto magic = [&](uint64_t pos, std::string_view m) {
        Bytes h = read(pos, m.size());
        return h.size() == m.size() && std::memcmp(h.data(), m.data(), m.size()) == 0;
    };
    // an ID3v2 tag at `pos` that ends within the file
    auto id3_at = [&](uint64_t pos) {
        auto len = id3::tag_length(read(pos, 10));
        WM_ENSURE(len.has_value(), "no ID3 tag at {}", pos);
        WM_ENSURE(*len <= size && pos <= size - *len, "ID3 tag runs past the end of the file");
        return id3::read_tags(read(pos, size_t(*len)));
    };
    if (ext == "flac") {
        WM_ENSURE(magic(0, "fLaC"), "not a FLAC file");
        Tags t;
        for (uint64_t pos = 4;;) {
            Bytes bh = read(pos, 4);
            WM_ENSURE(bh.size() == 4, "FLAC metadata cut short");
            bool last = bh[0] & 0x80;
            size_t len = size_t(bh[1]) << 16 | size_t(bh[2]) << 8 | bh[3];
            WM_ENSURE(pos + 4 + len <= size, "FLAC metadata cut short");
            if ((bh[0] & 0x7F) == flac::BLOCK_VORBIS)
                for (auto& [k, v] : flac::parse_vorbis(read(pos + 4, len))) t.add(k, v);
            pos += 4 + len;
            if (last) return t;
        }
    }
    if (ext == "dsf") {
        Bytes h = read(0, 28);
        WM_ENSURE(h.size() == 28 && std::memcmp(h.data(), "DSD ", 4) == 0, "not a DSF file");
        uint64_t meta = le64(&h[20]);
        return meta ? id3_at(meta) : Tags{};  // no metadata chunk: every tag removed
    }
    if (ext == "mp3") {
        WM_ENSURE(size > 0, "empty file");
        return magic(0, "ID3") ? id3_at(0) : Tags{};  // no tag: every tag removed
    }
    if (ext == "m4a") {
        WM_ENSURE(mp4_complete(read, size), "not a complete MP4 file");
        return mp4::read_tags(read, size);
    }
    if (ext == "wv") {
        WM_ENSURE(magic(0, "wvpk"), "not a WavPack file");
        auto a = ape::read(read, size);
        return a ? a->tags : Tags{};
    }
    fail("no tag reader for .{} files", ext);
}

std::optional<Bytes> read_front_cover(const std::string& ext, const ByteReader& read, uint64_t size) {
    auto id3_at = [&](uint64_t pos) -> std::optional<Bytes> {
        auto len = id3::tag_length(read(pos, 10));
        if (!len || *len > size || pos > size - *len) return std::nullopt;
        return id3::front_picture(read(pos, size_t(*len)));
    };
    if (ext == "flac") {
        std::optional<Bytes> first;
        for (uint64_t pos = 4; pos + 4 <= size;) {
            Bytes bh = read(pos, 4);
            if (bh.size() < 4) break;
            bool last = bh[0] & 0x80;
            size_t len = size_t(bh[1]) << 16 | size_t(bh[2]) << 8 | bh[3];
            if ((bh[0] & 0x7F) == flac::BLOCK_PICTURE) {
                // type, MIME, description, width, height, depth, colours, image
                Bytes b = read(pos + 4, len);
                auto image = [&]() -> std::optional<Bytes> {
                    size_t p = 4;
                    for (int s = 0; s < 2; s++) {  // MIME type, description
                        if (p + 4 > b.size()) return std::nullopt;
                        p += 4 + be32(&b[p]);
                    }
                    p += 16;
                    if (p + 4 > b.size() || be32(&b[p]) > b.size() - p - 4) return std::nullopt;
                    return Bytes(b.begin() + ptrdiff_t(p + 4), b.begin() + ptrdiff_t(p + 4 + be32(&b[p])));
                }();
                if (image && b.size() >= 4 && be32(b.data()) == 3) return image;
                if (image && !first) first = std::move(image);
            }
            pos += 4 + len;
            if (last) break;
        }
        return first;
    }
    if (ext == "dsf") {
        Bytes h = read(0, 28);
        if (h.size() < 28) return std::nullopt;
        uint64_t meta = le64(&h[20]);
        return meta ? id3_at(meta) : std::nullopt;
    }
    if (ext == "mp3") return id3_at(0);
    if (ext == "wv") {
        auto a = ape::read(read, size);
        if (!a || !a->front_cover) return std::nullopt;
        Bytes v = read(a->front_cover->off, size_t(a->front_cover->len));  // file name, NUL, image
        auto nul = std::find(v.begin(), v.end(), 0);
        if (nul == v.end() || nul + 1 == v.end()) return std::nullopt;
        return Bytes(nul + 1, v.end());
    }
    return std::nullopt;
}

std::vector<std::string> read_other_frames(const std::string& ext, const ByteReader& read, uint64_t size) {
    uint64_t pos = 0;
    if (ext == "dsf") {
        Bytes h = read(0, 28);
        if (h.size() < 28 || !(pos = le64(&h[20]))) return {};
    } else if (ext != "mp3") {
        return {};
    }
    auto len = id3::tag_length(read(pos, 10));
    if (!len || *len > size || pos > size - *len) return {};
    return id3::other_frame_names(read(pos, size_t(*len)));
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
            // numbers compare as numbers ("04" == "4")
            if (c == "TRACKNUMBER" || c == "TRACKTOTAL" || c == "DISCNUMBER" || c == "DISCTOTAL")
                for (auto* vs : {&x, &y})
                    for (auto& v : *vs)
                        if (auto n = parse_u64(split(v, '/')[0])) v = std::to_string(*n);
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

uint64_t WriteSession::changes() const {
    std::lock_guard g(mu_);
    return changes_;
}

void WriteSession::mark_stored(uint64_t changes) {
    std::lock_guard g(mu_);
    stored_ = std::max(stored_, changes);
}

bool WriteSession::dirty() const {
    std::lock_guard g(mu_);
    return changes_ != stored_;
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
            for (size_t done = 0; done < n;) {
                ssize_t r = ::pread(fd_, out.data() + old + done, n - done, off_t(pos + done));
                if (r < 0 && errno == EINTR) continue;
                if (r < 0) fail("scratch read: {}", std::strerror(errno));
                WM_ENSURE(r > 0, "scratch read: unexpected end of file");
                done += size_t(r);
            }
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
    changes_++;
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
    changes_++;
}

}  // namespace wm
