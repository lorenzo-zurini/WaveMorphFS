// SPDX-License-Identifier: GPL-3.0-or-later
#include "sacd.hpp"

#include <algorithm>
#include <cstring>

#include "cache.hpp"
#include "charset.hpp"
#include "dst.hpp"
#include "id3.hpp"

namespace wm {

namespace {

constexpr uint64_t SECTOR = 2048;
constexpr uint64_t MASTER_TOC = 510;
constexpr uint64_t DSF_BLOCK = 4096;
constexpr uint8_t DATA_AUDIO = 2;
const char IDX_MAGIC_V1[8] = {'W', 'M', 'S', 'A', 'C', 'D', '0', '1'};  // frame starts only
const char IDX_MAGIC[8] = {'W', 'M', 'S', 'A', 'C', 'D', '0', '2'};     // frame starts + end marker

const auto BITREV = [] {
    std::array<uint8_t, 256> t{};
    for (int i = 0; i < 256; i++) {
        uint8_t b = uint8_t(i), r = 0;
        for (int k = 0; k < 8; k++) r = uint8_t(r << 1 | ((b >> k) & 1));
        t[size_t(i)] = r;
    }
    return t;
}();

uint64_t tc(const uint8_t* b) { return (uint64_t(b[0]) * 60 + b[1]) * 75 + b[2]; }

std::string sacd_text(std::span<const uint8_t> bytes, uint8_t charset) {
    const char* enc = charset == 3 ? "CP932" : charset == 4 ? "CP949" : charset == 5 ? "GB18030" : charset == 6 ? "BIG5-HKSCS" : "windows-1252";
    return trim(decode_as(bytes, enc).value_or(decode_1252(bytes)));
}

std::optional<std::string> cstr_at(std::span<const uint8_t> b, size_t o, uint8_t charset) {
    if (o == 0 || o >= b.size()) return std::nullopt;
    auto it = std::find(b.begin() + ptrdiff_t(o), b.end(), 0);
    auto s = sacd_text(b.subspan(o, size_t(it - b.begin()) - o), charset);
    if (s.empty()) return std::nullopt;
    return s;
}

// Scarlet Book genre codes (as in sacd_extract): 1 = not defined, 2 = Adult Contemporary, ...
const char* const GENRES[] = {"", "", "Adult Contemporary", "Alternative Rock", "Children's Music", "Classical", "Contemporary Christian",
                              "Country", "Dance", "Easy Listening", "Erotic", "Folk", "Gospel", "Hip Hop", "Jazz", "Latin", "Musical",
                              "New Age", "Opera", "Operetta", "Pop Music", "Rap", "Reggae", "Rock Music", "Rhythm & Blues", "Sound Effects",
                              "Soundtrack", "Spoken Word", "World Music", "Blues"};

struct Packet {
    size_t off, len;
    uint8_t data_type;
    bool frame_start;
};

/// One parsed audio sector.
struct AudioSector {
    bool dst = false;
    std::vector<Packet> packets;
    std::vector<uint64_t> frame_tcs;  // time codes of frames starting in this sector
};

AudioSector parse_sector(const uint8_t* s) {
    uint8_t h = s[0];
    size_t npk = h >> 5, nfi = (h >> 2) & 7;
    AudioSector r;
    r.dst = h & 1;
    size_t p = 1;
    struct Info {
        bool fs;
        uint8_t dt;
        size_t len;
    };
    std::vector<Info> pk;
    for (size_t i = 0; i < npk; i++) {
        uint16_t w = be16(s + p);
        p += 2;
        pk.push_back({bool((w >> 15) & 1), uint8_t((w >> 11) & 7), size_t(w & 0x7FF)});
    }
    for (size_t i = 0; i < nfi; i++) {
        r.frame_tcs.push_back(tc(s + p));
        p += r.dst ? 4 : 3;
    }
    for (auto& x : pk) {
        WM_ENSURE(p + x.len <= SECTOR, "packet overruns sector");
        r.packets.push_back({p, x.len, x.dt, x.fs});
        p += x.len;
    }
    return r;
}

using Frames = std::vector<std::pair<uint32_t, uint16_t>>;

/// Record where every audio frame starts. Stops at the start of frame `needed`
/// (one past the last track), which then doubles as the end marker. Some rips
/// end the area with an unparseable sector; that is tolerated once every needed
/// frame is complete, and the bad sector becomes the end marker.
Frames scan_frames(const File& f, uint64_t start, uint64_t end, uint32_t channels, bool dst, uint64_t needed) {
    Frames frames;
    size_t frame_bytes = SACD_FRAME_BYTES * channels;
    size_t acc = 0;  // audio bytes of the current frame seen so far
    constexpr uint64_t CHUNK = 2048;  // sectors per read (4 MiB)
    // a final frame cut short by a broken last sector is accepted (and padded with
    // silence when read): it is well under a millisecond at the very end of the disc
    auto complete = [&] { return frames.size() == needed && (dst || (acc > 0 && acc <= frame_bytes)); };
    Bytes buf;
    for (uint64_t s = start; s <= end;) {
        uint64_t n = std::min(CHUNK, end + 1 - s);
        buf.resize(size_t(n * SECTOR));
        WM_ENSURE(f.read_at(buf.data(), buf.size(), s * SECTOR) == buf.size(), "short read at sector {}", s);
        for (uint64_t i = 0; i < n; i++) {
            uint64_t sec_no = s + i;
            AudioSector p;
            try {
                p = with_context(std::format("sector {}", sec_no), [&] { return parse_sector(&buf[size_t(i * SECTOR)]); });
                WM_ENSURE(p.dst == dst, "sector {}: DST flag {} in a {} area", sec_no, p.dst, dst ? "DST" : "plain DSD");
            } catch (const Error& e) {
                if (!complete()) throw;
                info("{}: ignored, all {} frames already found", e.what(), needed);
                frames.emplace_back(uint32_t(sec_no), 0);
                return frames;
            }
            size_t tci = 0;
            for (auto& pk : p.packets) {
                if (pk.data_type != DATA_AUDIO) continue;
                if (pk.frame_start) {
                    WM_ENSURE(dst || frames.empty() || acc == frame_bytes, "frame {} has {} bytes, expected {}", frames.size() - 1, acc, frame_bytes);
                    uint64_t expect = frames.size();
                    if (tci < p.frame_tcs.size()) {
                        uint64_t t = p.frame_tcs[tci++];
                        WM_ENSURE(t == expect, "sector {}: time code {} where frame {} was expected", sec_no, t, expect);
                    }
                    frames.emplace_back(uint32_t(sec_no), uint16_t(pk.off));
                    acc = 0;
                    if (frames.size() == needed + 1) return frames;  // start of the frame after the last track = end marker
                }
                acc += pk.len;
            }
        }
        s += n;
    }
    WM_ENSURE(complete(), "area ended after {} of {} frames", frames.size(), needed);
    frames.emplace_back(uint32_t(end + 1), 0);
    return frames;
}

std::optional<Frames> load_frames(const fs::path& p, uint64_t area_end) {
    auto b = try_read_file(p);
    if (!b || b->size() < 16) return std::nullopt;
    bool v1 = std::memcmp(b->data(), IDX_MAGIC_V1, 8) == 0;
    if (!v1 && std::memcmp(b->data(), IDX_MAGIC, 8) != 0) return std::nullopt;
    uint64_t n = le64(&(*b)[8]);
    if (b->size() != 16 + n * 6) return std::nullopt;
    Frames fr;
    fr.reserve(size_t(n) + 1);
    for (size_t i = 0; i < n; i++) {
        const uint8_t* c = &(*b)[16 + 6 * i];
        fr.emplace_back(le32(c), uint16_t(c[4] | c[5] << 8));
    }
    // v1 caches were only written for discs whose area ends cleanly
    if (v1) fr.emplace_back(uint32_t(area_end + 1), 0);
    return fr;
}

void store_frames(const fs::path& p, const Frames& fr) {
    Bytes b(IDX_MAGIC, IDX_MAGIC + 8);
    put_le64(b, fr.size());
    for (auto [s, o] : fr) {
        put_le32(b, s);
        put_le16(b, o);
    }
    atomic_write(p, b);
}

}  // namespace

bool is_sacd(const fs::path& p) {
    try {
        File f(p);
        uint8_t b[8];
        f.read_exact_at(b, 8, MASTER_TOC * SECTOR);
        return std::memcmp(b, "SACDMTOC", 8) == 0;
    } catch (...) {
        return false;
    }
}

bool sacd_is_cached(const Cache& c, const fs::path& src, bool multichannel) {
    auto k = SrcKey::try_of(src);
    return k && fs::exists(c.sacd_frames_path(src, *k, multichannel));
}

std::shared_ptr<SacdDisc> SacdDisc::open(const fs::path& path, const Cache* cache, bool multichannel) {
    File f(path);
    uint64_t flen = f.size();
    auto sec = [&](uint64_t n, uint64_t count) {
        Bytes b(size_t(SECTOR * count));
        WM_ENSURE(f.read_at(b.data(), b.size(), n * SECTOR) == b.size(), "short read at sector {}", n);
        return b;
    };
    Bytes m = sec(MASTER_TOC, 1);
    WM_ENSURE(std::memcmp(m.data(), "SACDMTOC", 8) == 0, "no SACD master TOC");
    uint64_t area1 = be32(&m[64]), area2 = be32(&m[72]);
    std::string catalog = trim(lossy_utf8(std::span(m).subspan(24, 16)));
    uint16_t set_size = be16(&m[16]), set_seq = be16(&m[18]), year = be16(&m[120]);
    const char* genre = nullptr;
    if (m[104] == 1) {
        uint16_t g = be16(&m[106]);
        if (g < std::size(GENRES) && *GENRES[g]) genre = GENRES[g];
    }
    uint8_t master_charset = m[138];  // first locale: [lang, lang, charset, reserved] at 136

    // the stereo area, falling back to multichannel if it is the only one
    uint64_t area_start = multichannel ? area2 : area1 ? area1 : area2;
    WM_ENSURE(area_start != 0, "{}", multichannel ? "no multichannel area" : "no audio area");
    Bytes at = sec(area_start, 1);
    WM_ENSURE(std::memcmp(at.data(), "TWOCHTOC", 8) == 0 || std::memcmp(at.data(), "MULCHTOC", 8) == 0, "bad area TOC signature");
    uint64_t toc_size = be16(&at[10]);
    uint8_t frame_format = at[0x15] & 0x0F;
    WM_ENSURE(frame_format == 0 || frame_format == 2 || frame_format == 3, "unknown frame format {}", frame_format);
    bool dst = frame_format == 0;
    WM_ENSURE(at[0x14] == 4, "unsupported sample rate code {}", at[0x14]);
    uint32_t channels = at[0x20];
    WM_ENSURE(channels >= 1 && channels <= 6, "bad channel count {}", channels);
    size_t ntracks = at[0x45];
    uint64_t audio_start = be32(&at[0x48]), audio_end = be32(&at[0x4C]);
    WM_ENSURE(ntracks >= 1 && audio_end > audio_start, "bad track info");
    WM_ENSURE(flen >= (audio_end + 1) * SECTOR, "ISO is truncated (still downloading?)");
    uint8_t area_charset = at[0x5A];

    // locate SACDTRL2 and SACDTTxt inside the area TOC
    Bytes toc = sec(area_start, std::max<uint64_t>(toc_size, 1));
    auto find = [&](const char* sig) -> std::optional<size_t> {
        for (size_t i = 0; i < toc_size; i++)
            if (std::memcmp(&toc[i * SECTOR], sig, 8) == 0) return i * SECTOR;
        return std::nullopt;
    };
    auto trl2 = find("SACDTRL2");
    WM_ENSURE(trl2.has_value(), "no SACDTRL2");
    auto disc = std::make_shared<SacdDisc>();
    for (size_t i = 0; i < ntracks; i++) {
        uint64_t start = tc(&toc[*trl2 + 8 + 4 * i]);
        uint64_t dur = tc(&toc[*trl2 + 8 + 1020 + 4 * i]);
        SacdTrack t;
        t.start = start;
        t.end = start + dur;
        disc->tracks.push_back(t);
    }
    if (auto tt = find("SACDTTxt")) {
        for (size_t i = 0; i < disc->tracks.size(); i++) {
            auto& t = disc->tracks[i];
            size_t off = be16(&toc[*tt + 8 + 2 * i]);
            if (off == 0) continue;
            size_t base = *tt + off;
            if (base + 4 > toc.size()) continue;
            size_t count = toc[base], p = base + 4;
            for (size_t c = 0; c < count; c++) {
                if (p + 2 > toc.size()) break;
                uint8_t kind = toc[p];
                auto it = std::find(toc.begin() + ptrdiff_t(p + 2), toc.end(), 0);
                size_t end = size_t(it - toc.begin());
                std::string text = sacd_text(std::span(toc).subspan(p + 2, end - p - 2), area_charset);
                std::optional<std::string> v = text.empty() ? std::nullopt : std::optional(text);
                switch (kind) {
                case 1: t.title = v; break;
                case 2: t.performer = v; break;
                case 3: t.songwriter = v; break;
                case 4: t.composer = v; break;
                case 5: t.arranger = v; break;
                default: break;
                }
                // items are padded to 4-byte boundaries relative to the entry start
                size_t len = end + 1 - p;
                p += (len + 3) / 4 * 4;
            }
        }
    }

    // album-level tags from the master text (first language)
    Bytes mt = sec(MASTER_TOC + 1, 1);
    Tags& album = disc->album;
    if (std::memcmp(mt.data(), "SACDText", 8) == 0) {
        auto pos = [&](size_t i) { return size_t(be16(&mt[16 + 2 * i])); };
        uint8_t cs = master_charset ? master_charset : area_charset;
        auto pick = [&](size_t a, size_t b) {
            auto v = cstr_at(mt, pos(a), cs);
            return v ? v : cstr_at(mt, pos(b), cs);
        };
        if (auto v = pick(0, 8)) album.set("ALBUM", *v);
        if (auto v = pick(1, 9)) album.set("ALBUMARTIST", *v);
        if (auto v = pick(2, 10)) album.set("LABEL", *v);
        if (auto v = pick(3, 11)) album.set("COPYRIGHT", *v);
    }
    if (!catalog.empty()) album.set("CATALOGNUMBER", catalog);
    if (year >= 1900 && year < 2200) album.set("DATE", std::to_string(year));
    if (genre) album.set("GENRE", genre);
    if (set_size > 1 && set_seq >= 1) {
        album.set("DISCNUMBER", std::to_string(set_seq));
        album.set("DISCTOTAL", std::to_string(set_size));
    }
    album.set("MEDIA", "SACD");

    uint64_t needed = disc->tracks.back().end;
    auto key = SrcKey::of(path);
    std::optional<Frames> frames;
    fs::path cp;
    if (cache) {
        cp = cache->sacd_frames_path(path, key, multichannel);
        frames = load_frames(cp, audio_end);
    }
    if (!frames) {
        frames = scan_frames(f, audio_start, audio_end, channels, dst, needed);
        if (cache) store_frames(cp, *frames);
    }
    // frames ends with one extra entry: where the last needed frame ends
    WM_ENSURE(needed < frames->size(), "track list ends at frame {} but only {} frames were found", needed, frames->size() ? frames->size() - 1 : 0);
    disc->path = path;
    disc->channels = channels;
    disc->dst = dst;
    disc->frames_ = std::move(*frames);
    return disc;
}

Tags SacdDisc::track_tags(size_t i) const {
    const auto& t = tracks[i];
    Tags tg = album;
    if (t.title) tg.set("TITLE", *t.title);
    if (t.performer) tg.set("ARTIST", *t.performer);
    else if (auto aa = tg.get_all("ALBUMARTIST")) tg.set_many("ARTIST", *aa);
    if (t.composer) tg.set("COMPOSER", *t.composer);
    else if (t.songwriter) tg.set("COMPOSER", *t.songwriter);
    if (t.arranger) tg.set("ARRANGER", *t.arranger);
    return tg;
}

std::vector<Bytes> SacdDisc::coded_frames(uint64_t f0, uint64_t f1) const {
    auto start = frames_[size_t(f0)];
    auto end = frames_[size_t(f1)];  // always present: the list ends with an end marker
    uint64_t s0 = start.first, s1 = end.first;
    uint64_t count = s1 - s0 + 1;
    File file(path);
    Bytes buf(size_t(count * SECTOR));
    size_t n = file.read_at(buf.data(), buf.size(), s0 * SECTOR);
    buf.resize(n - n % SECTOR);
    std::vector<Bytes> out;
    out.reserve(size_t(f1 - f0));
    for (uint64_t i = 0; i < buf.size() / SECTOR; i++) {
        uint64_t sec_no = s0 + i;
        // the end marker's own sector is only part of the range if the next frame
        // starts inside it; never parse beyond (e.g. the area's backup TOC)
        if (sec_no > end.first || (sec_no == end.first && end.second == 0)) break;
        const uint8_t* sb = &buf[size_t(i * SECTOR)];
        auto parsed = parse_sector(sb);
        bool done = false;
        for (auto& pk : parsed.packets) {
            std::pair<uint32_t, uint16_t> pos{uint32_t(sec_no), uint16_t(pk.off)};
            if (pos < start) continue;
            if (pos >= end) {
                done = true;
                break;
            }
            if (pk.data_type != DATA_AUDIO) continue;
            if (pk.frame_start || out.empty()) {
                out.emplace_back();
                out.back().reserve(SACD_FRAME_BYTES * channels);
            }
            out.back().insert(out.back().end(), sb + pk.off, sb + pk.off + pk.len);
        }
        if (done) break;
    }
    WM_ENSURE(out.size() == f1 - f0, "expected {} frames, found {}", f1 - f0, out.size());
    return out;
}

std::vector<Bytes> SacdDisc::read_frames(uint64_t f0, uint64_t f1) const {
    size_t ch = channels, per_frame = SACD_FRAME_BYTES * ch;
    auto coded = coded_frames(f0, f1);
    std::vector<Bytes> out(ch);
    for (auto& o : out) o.reserve(SACD_FRAME_BYTES * coded.size());
    std::unique_ptr<DstDecoder> dec;
    if (dst) dec = std::make_unique<DstDecoder>(ch);
    Bytes raw(per_frame);
    for (size_t i = 0; i < coded.size(); i++) {
        const Bytes* inter = &coded[i];
        if (dec) {
            raw.resize(per_frame);
            with_context(std::format("frame {}", f0 + i), [&] { dec->decode(coded[i], raw); });
            inter = &raw;
        } else if (coded[i].size() != per_frame) {
            bool last = f0 + i + 2 == frames_.size();  // frames_ ends with the end marker
            WM_ENSURE(last && coded[i].size() < per_frame, "frame {} has {} bytes", f0 + i, coded[i].size());
            raw.assign(coded[i].begin(), coded[i].end());
            raw.resize(per_frame, 0x69);  // DSD silence
            inter = &raw;
        }
        // de-interleave (byte-interleaved channels)
        for (size_t j = 0; j < inter->size(); j++) out[j % ch].push_back((*inter)[j]);
    }
    return out;
}

std::pair<uint64_t, std::shared_ptr<const std::vector<Bytes>>> SacdDisc::channel_bytes(uint64_t f_first, uint64_t f_last) const {
    {
        std::lock_guard g(mu_);
        if (recent_ && recent_->first <= f_first && recent_->first + (*recent_->second)[0].size() / SACD_FRAME_BYTES >= f_last) return *recent_;
    }
    auto v = std::make_shared<const std::vector<Bytes>>(read_frames(f_first, f_last));
    std::lock_guard g(mu_);
    recent_ = {f_first, v};
    return *recent_;
}

DsfTrack::DsfTrack(std::shared_ptr<const SacdDisc> disc, size_t track, const Tags& tags) : disc_(std::move(disc)) {
    const auto& t = disc_->tracks[track];
    uint64_t ch = disc_->channels;
    f0_ = t.start;
    bytes_per_ch_ = (t.end - t.start) * SACD_FRAME_BYTES;
    data_len_ = (bytes_per_ch_ + DSF_BLOCK - 1) / DSF_BLOCK * DSF_BLOCK * ch;
    id3_ = id3::build(tags);
    uint64_t total = 28 + 52 + 12 + data_len_ + id3_.size();
    Bytes& h = header_;
    append(h, std::string_view("DSD "));
    put_le64(h, 28);
    put_le64(h, total);
    put_le64(h, 28 + 52 + 12 + data_len_);  // metadata pointer
    append(h, std::string_view("fmt "));
    put_le64(h, 52);
    put_le32(h, 1);  // format version
    put_le32(h, 0);  // DSD raw
    uint32_t ch_type = ch <= 4 ? uint32_t(ch) : ch == 5 ? 6 : 7;
    put_le32(h, ch_type);
    put_le32(h, uint32_t(ch));
    put_le32(h, 2'822'400);
    put_le32(h, 1);                    // bits per sample: 1 = LSB first
    put_le64(h, bytes_per_ch_ * 8);    // sample count per channel
    put_le32(h, uint32_t(DSF_BLOCK));
    put_le32(h, 0);
    append(h, std::string_view("data"));
    put_le64(h, 12 + data_len_);
}

void DsfTrack::read_data(uint64_t a, uint64_t b, Bytes& out) const {
    uint64_t ch = disc_->channels, group = DSF_BLOCK * ch;
    uint64_t g0 = a / group, g1 = (b - 1) / group;
    // channel byte range needed
    uint64_t j0 = g0 * DSF_BLOCK, j1 = std::min((g1 + 1) * DSF_BLOCK, bytes_per_ch_);
    std::pair<uint64_t, std::shared_ptr<const std::vector<Bytes>>> frames{0, nullptr};
    if (j0 < j1) frames = disc_->channel_bytes(f0_ + j0 / SACD_FRAME_BYTES, f0_ + (j1 + SACD_FRAME_BYTES - 1) / SACD_FRAME_BYTES);
    for (uint64_t pos = a; pos < b;) {
        uint64_t g = pos / group, c = (pos % group) / DSF_BLOCK, i = pos % DSF_BLOCK;
        uint64_t run = std::min(DSF_BLOCK - i, b - pos);
        uint64_t j = g * DSF_BLOCK + i;  // channel byte index
        for (uint64_t k = 0; k < run; k++) {
            uint64_t jj = j + k;
            if (jj < bytes_per_ch_) {
                uint64_t idx = (f0_ * SACD_FRAME_BYTES + jj) - frames.first * SACD_FRAME_BYTES;
                out.push_back(BITREV[(*frames.second)[size_t(c)][size_t(idx)]]);
            } else {
                out.push_back(0);
            }
        }
        pos += run;
    }
}

Bytes DsfTrack::read_at(uint64_t off, size_t len) const {
    uint64_t sz = size();
    Bytes out;
    if (off >= sz) return out;
    len = size_t(std::min<uint64_t>(len, sz - off));
    out.reserve(len);
    uint64_t end = off + len, h = header_.size();
    copy_overlap(out, header_, 0, off, len);
    if (off < h + data_len_ && end > h) read_data(std::max(off, h) - h, std::min(end, h + data_len_) - h, out);
    copy_overlap(out, id3_, h + data_len_, off, len);
    WM_ENSURE(out.size() == len, "internal: produced {} of {}", out.size(), len);
    return out;
}

std::string DsfTrack::describe() const {
    return std::format("sacd:{}#frames={}+{}", disc_->path.string(), f0_, bytes_per_ch_ / SACD_FRAME_BYTES);
}

}  // namespace wm
