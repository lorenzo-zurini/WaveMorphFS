// SPDX-License-Identifier: GPL-3.0-or-later
// CUE sheet parsing with charset detection.
//
// Only what matters for splitting and tagging is kept: disc-level fields
// (PERFORMER, TITLE, SONGWRITER, CATALOG, REM *), and per track the number,
// INDEX 00/01 positions (in CD frames of 1/75 s) and track-level fields.
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace wm {

struct CueTrack {
    uint32_t number = 0;
    /// INDEX 01 in CD frames (75 per second) relative to the start of the file
    uint64_t index01 = 0;
    std::optional<uint64_t> index00;
    /// TITLE / PERFORMER / SONGWRITER / ISRC and REM fields, upper-case keys
    std::map<std::string, std::string> fields;
};

struct CueSheet {
    /// FILE entries in order (only single-FILE sheets are used for splitting)
    std::vector<std::string> files;
    std::map<std::string, std::string> fields;
    std::vector<CueTrack> tracks;
    std::string encoding;

    static CueSheet read(const std::filesystem::path& p);
    static CueSheet parse_bytes(std::span<const uint8_t> raw);
    static CueSheet parse(const std::string& text);
    /// Single FILE with several tracks = an image rip that needs splitting.
    bool is_image() const { return files.size() == 1 && tracks.size() > 1; }
};

/// "MM:SS:FF" -> CD frames (1/75 s)
uint64_t parse_msf(const std::string& s);

}  // namespace wm
