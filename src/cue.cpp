// SPDX-License-Identifier: GPL-3.0-or-later
#include "cue.hpp"

#include "charset.hpp"
#include "util.hpp"

namespace wm {

// (first word, rest trimmed); words are separated by Unicode white space
static std::pair<std::string, std::string> split_word(const std::string& in) {
    std::string s = trim(in);
    for (size_t i = 0, len; i < s.size(); i += len) {
        if (ws_at(s, i, len)) return {s.substr(0, i), trim(s.substr(i))};
    }
    return {s, ""};
}

static std::string unquote(const std::string& in) {
    std::string s = trim(in);
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = s.substr(1, s.size() - 2);
    return trim(s);
}

uint64_t parse_msf(const std::string& s) {
    auto p = split(trim(s), ':');
    if (p.size() != 3) fail("bad timestamp \"{}\"", s);
    auto m = parse_u64(p[0]), sec = parse_u64(p[1]), f = parse_u64(p[2]);
    if (!m || !sec || !f || *sec >= 60 || *f >= 75) fail("bad timestamp \"{}\"", s);
    return (*m * 60 + *sec) * 75 + *f;
}

CueSheet CueSheet::read(const std::filesystem::path& p) { return parse_bytes(read_file(p)); }

CueSheet CueSheet::parse_bytes(std::span<const uint8_t> raw) {
    auto d = decode_text(raw);
    auto sheet = parse(d.text);
    sheet.encoding = d.encoding;
    return sheet;
}

CueSheet CueSheet::parse(const std::string& text) {
    CueSheet sheet;
    std::optional<CueTrack> cur;
    for (auto& raw_line : split(text, '\n')) {
        std::string line = raw_line;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        line = trim(line);
        if (line.empty()) continue;
        auto [cmd0, rest] = split_word(line);
        std::string cmd = upper(cmd0);
        if (cmd == "FILE") {
            // FILE "name with spaces.flac" WAVE  |  FILE name.flac WAVE
            std::string name;
            if (!rest.empty() && rest[0] == '"') {
                std::string r = rest.substr(1);
                auto q = r.rfind('"');
                name = q == std::string::npos ? r : r.substr(0, q);
            } else {
                size_t cut = std::string::npos;
                for (size_t i = 0, len; i < rest.size(); i += len)
                    if (ws_at(rest, i, len)) cut = i;
                name = cut == std::string::npos ? rest : rest.substr(0, cut);
            }
            sheet.files.push_back(trim(name));
        } else if (cmd == "TRACK") {
            if (cur) sheet.tracks.push_back(std::move(*cur));
            auto [num, kind] = split_word(rest);
            CueTrack t;
            auto n = parse_u64(num);
            t.number = n && *n <= UINT32_MAX ? uint32_t(*n) : uint32_t(sheet.tracks.size() + 1);
            // only audio tracks matter (data tracks on enhanced CDs are skipped)
            if (!iequals(kind, "AUDIO")) t.fields["__NONAUDIO"] = kind;
            cur = std::move(t);
        } else if (cmd == "INDEX") {
            auto [idx, pos] = split_word(rest);
            if (!cur) continue;
            uint64_t frames = parse_msf(pos);
            auto i = parse_u64(idx);
            if (i && *i == 0) cur->index00 = frames;
            else if (i && *i == 1) cur->index01 = frames;
        } else if (cmd == "REM") {
            auto [k0, v0] = split_word(rest);
            std::string k = upper(k0);
            if (k.empty()) continue;
            std::string v = unquote(v0);
            (cur ? cur->fields : sheet.fields)[k] = v;
        } else if (cmd == "TITLE" || cmd == "PERFORMER" || cmd == "SONGWRITER" || cmd == "ISRC" || cmd == "CATALOG" || cmd == "COMPOSER" ||
                   cmd == "ARRANGER") {
            std::string v = unquote(rest);
            if (v.empty()) continue;
            (cur ? cur->fields : sheet.fields)[cmd] = v;
        }
    }
    if (cur) sheet.tracks.push_back(std::move(*cur));
    std::erase_if(sheet.tracks, [](auto& t) { return t.fields.contains("__NONAUDIO"); });
    if (sheet.tracks.empty()) fail("no audio tracks");
    // INDEX positions are relative to each FILE, so ordering only applies to single-file sheets
    if (sheet.files.size() == 1)
        for (size_t i = 1; i < sheet.tracks.size(); i++)
            if (sheet.tracks[i].index01 <= sheet.tracks[i - 1].index01)
                fail("track {} INDEX 01 is not after track {}", sheet.tracks[i].number, sheet.tracks[i - 1].number);
    return sheet;
}

}  // namespace wm
