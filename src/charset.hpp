// SPDX-License-Identifier: GPL-3.0-or-later
// Text of unknown charset (CUE sheets) and named legacy charsets (SACD text).
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace wm {

/// Decode bytes in a named charset to UTF-8 (iconv names, plus "windows-1252"
/// with WHATWG semantics). nullopt if the bytes are invalid in that charset.
std::optional<std::string> decode_as(std::span<const uint8_t> raw, const std::string& charset);

/// windows-1252 as browsers decode it (never fails; undefined bytes map to C1 controls).
std::string decode_1252(std::span<const uint8_t> raw);

struct Decoded {
    std::string text;
    std::string encoding;
};

/// UTF-8/UTF-16 BOMs are honoured; valid UTF-8 is taken as is; anything else is
/// matched against the legacy code pages common in rips (Windows-125x, KOI8-R,
/// Shift_JIS, GBK, EUC-KR, Big5) by scoring how plausible each decoding reads.
Decoded decode_text(std::span<const uint8_t> raw);

}  // namespace wm
