// SPDX-License-Identifier: GPL-3.0-or-later
#include "retag.hpp"

#include <cstring>

#include "flac.hpp"
#include "id3.hpp"
#include "mp4.hpp"

namespace wm {

std::shared_ptr<Spliced> retag_file(const fs::path& p, const std::string& ext, const Tags& overlay) {
    uint64_t len = File(p).size();
    if (ext == "flac") {
        auto meta = flac::FlacMeta::read(p);
        Tags tags = Tags::from_pairs(meta.vorbis_comments());
        tags.overlay(overlay);
        std::vector<flac::HeaderBlock> blocks = {{flac::BLOCK_VORBIS, flac::build_vorbis("WaveMorphFS", tags.to_pairs())}};
        // keep every other block (SEEKTABLE offsets are relative to the first frame, so
        // still valid), read from the source rather than held: pictures can be large
        for (auto& b : meta.blocks)
            if (b.kind != flac::BLOCK_VORBIS) blocks.push_back({b.kind, SrcRange{b.offset, b.data.size()}});
        blocks.push_back({flac::BLOCK_PADDING, Zeros{flac::EDIT_PADDING}});
        // min/max frame size fields are informational; keep them unknown rather than re-derive
        auto segs = flac::header_segments(meta.streaminfo.encode(0, 0), blocks);
        segs.push_back(SrcRange{meta.audio_start, len - meta.audio_start});
        return std::make_shared<Spliced>(p, "retagged-flac", std::move(segs));
    }
    if (ext == "mp3") {
        auto [tag, start] = id3::retag_mp3(p, overlay);
        return std::make_shared<Spliced>(p, "retagged-mp3", std::vector<Seg>{std::move(tag), SrcRange{start, len - start}});
    }
    if (ext == "m4a") {
        auto r = mp4::retag_m4a(p, overlay);
        return std::make_shared<Spliced>(p, "retagged-m4a",
                                         std::vector<Seg>{SrcRange{0, r.moov_pos}, std::move(r.moov), SrcRange{r.moov_pos + r.moov_len, len - r.moov_pos - r.moov_len}});
    }
    if (ext == "dsf") {
        // DSD chunk: "DSD ", chunk size 28, total file size, offset of the ID3v2 tag
        // (0 = none), which follows the data chunk. The audio is kept as is; the
        // tag is rebuilt (frames the overlay does not name are kept) and the two
        // size fields are patched.
        File f(p);
        uint8_t h[28];
        f.read_exact_at(h, 28, 0);
        WM_ENSURE(std::memcmp(h, "DSD ", 4) == 0 && le64(h + 4) == 28, "not a DSF file");
        uint8_t fmt[12], data[12];
        f.read_exact_at(fmt, 12, 28);
        WM_ENSURE(std::memcmp(fmt, "fmt ", 4) == 0, "no fmt chunk");
        uint64_t data_pos = 28 + le64(fmt + 4);
        f.read_exact_at(data, 12, data_pos);
        WM_ENSURE(std::memcmp(data, "data", 4) == 0, "no data chunk");
        uint64_t audio_end = data_pos + le64(data + 4);
        WM_ENSURE(audio_end <= len, "data chunk runs past the end of the file");
        uint64_t meta = le64(h + 20);
        Bytes tag = id3::retag_at(f, meta ? meta : audio_end, overlay).first;
        Bytes dsd(h, h + 12);
        put_le64(dsd, audio_end + tag.size());
        put_le64(dsd, audio_end);
        return std::make_shared<Spliced>(p, "retagged-dsf", std::vector<Seg>{std::move(dsd), SrcRange{28, audio_end - 28}, std::move(tag)});
    }
    fail("cannot retag .{} files", ext);
}

}  // namespace wm
