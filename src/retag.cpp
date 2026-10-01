// SPDX-License-Identifier: GPL-3.0-or-later
#include "retag.hpp"

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
        std::vector<flac::MetaBlock> blocks = {{flac::BLOCK_VORBIS, flac::build_vorbis("WaveMorphFS", tags.to_pairs())}};
        // keep every other block (SEEKTABLE offsets are relative to the first frame, so still valid)
        for (auto& b : meta.blocks)
            if (b.kind != flac::BLOCK_VORBIS) blocks.push_back(b);
        blocks.push_back({flac::BLOCK_PADDING, Bytes(flac::EDIT_PADDING, 0)});
        // min/max frame size fields are informational; keep them unknown rather than re-derive
        Bytes header = flac::build_header(meta.streaminfo.encode(0, 0), blocks);
        return std::make_shared<Spliced>(p, "retagged-flac", std::vector<Seg>{std::move(header), SrcRange{meta.audio_start, len - meta.audio_start}});
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
    fail("cannot retag .{} files", ext);
}

}  // namespace wm
