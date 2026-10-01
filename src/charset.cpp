// SPDX-License-Identifier: GPL-3.0-or-later
#include "charset.hpp"

#include <iconv.h>

#include <cerrno>
#include <string_view>
#include <vector>

#include "util.hpp"

namespace wm {

// windows-1252 0x80..0x9F (WHATWG: undefined bytes map to the C1 control of the same value)
static const char16_t W1252_HIGH[32] = {0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
                                        0x2039, 0x0152, 0x008D, 0x017D, 0x008F, 0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
                                        0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178};

std::string decode_1252(std::span<const uint8_t> raw) {
    std::string o;
    o.reserve(raw.size() + raw.size() / 4);
    for (uint8_t b : raw) put_utf8(o, b >= 0x80 && b < 0xA0 ? char32_t(W1252_HIGH[b - 0x80]) : char32_t(b));
    return o;
}

/// iconv to UTF-8. With `lenient`, an invalid byte becomes U+FFFD and decoding
/// continues; otherwise the first invalid sequence fails the whole conversion.
static std::optional<std::string> iconv_decode(std::span<const uint8_t> raw, const char* from, bool lenient) {
    iconv_t cd = iconv_open("UTF-8", from);
    if (cd == iconv_t(-1)) return std::nullopt;
    std::string out;
    std::vector<char> buf(raw.size() * 4 + 16);
    char* in = const_cast<char*>(reinterpret_cast<const char*>(raw.data()));
    size_t inleft = raw.size();
    bool ok = true;
    while (inleft > 0) {
        char* o = buf.data();
        size_t oleft = buf.size();
        size_t r = iconv(cd, &in, &inleft, &o, &oleft);
        out.append(buf.data(), size_t(o - buf.data()));
        if (r != size_t(-1)) continue;
        if (errno == E2BIG) continue;
        if (!lenient) {
            ok = false;
            break;
        }
        // EILSEQ / EINVAL: replace one byte and resync
        put_utf8(out, 0xFFFD);
        in++;
        inleft--;
        iconv(cd, nullptr, nullptr, nullptr, nullptr);
    }
    iconv_close(cd);
    if (!ok) return std::nullopt;
    return out;
}

std::optional<std::string> decode_as(std::span<const uint8_t> raw, const std::string& charset) {
    if (iequals(charset, "windows-1252")) return decode_1252(raw);
    return iconv_decode(raw, charset.c_str(), true);
}

// ------------------------------------------------------------------ detection

namespace {

struct Candidate {
    const char* name;   // reported (WHATWG) name
    const char* iconv;  // iconv name; nullptr = built-in windows-1252
    bool multibyte;
    /// non-ASCII letters (lower case) of each language commonly written in it
    std::vector<std::u32string> languages;
};

const std::u32string FRENCH = U"éèêëàâîïôûùüÿçœæ", GERMAN = U"äöüß", SPANISH = U"áéíóúñü", PORTUGUESE = U"áâãàçéêíóôõú",
                     ITALIAN = U"àèéìòùíóú", DUTCH = U"ëïéèöüáó", DANISH = U"åæøé", SWEDISH = U"åäöé", ICELANDIC = U"áéíóúýðþæö",
                     CATALAN = U"àèéíïòóúüç", CZECH = U"áčďéěíňóřšťúůýž", SLOVAK = U"áäčďéíĺľňóôŕšťúýž", POLISH = U"ąćęłńóśźż",
                     HUNGARIAN = U"áéíóöőúüű", ROMANIAN = U"ăâîșşțţ", CROATIAN = U"čćđšž", TURKISH = U"çğıöşüâîû",
                     LATVIAN = U"āčēģīķļņšūž", LITHUANIAN = U"ąčęėįšųūž", ESTONIAN = U"äöüõšž";

std::vector<Candidate> candidates() {
    std::vector<std::u32string> west = {FRENCH, GERMAN, SPANISH, PORTUGUESE, ITALIAN, DUTCH, DANISH, SWEDISH, ICELANDIC, CATALAN};
    auto turkish = west;
    turkish.push_back(TURKISH);
    return {
        {"windows-1252", nullptr, false, west},
        {"windows-1250", "CP1250", false, {CZECH, SLOVAK, POLISH, HUNGARIAN, ROMANIAN, CROATIAN, GERMAN}},
        {"windows-1251", "CP1251", false, {}},
        {"windows-1254", "CP1254", false, turkish},
        {"windows-1257", "CP1257", false, {LATVIAN, LITHUANIAN, ESTONIAN}},
        {"windows-1253", "CP1253", false, {}},
        {"KOI8-R", "KOI8-R", false, {}},
        {"Shift_JIS", "CP932", true, {}},
        {"GBK", "GB18030", true, {}},
        {"EUC-KR", "CP949", true, {}},
        {"Big5", "BIG5-HKSCS", true, {}},
    };
}

std::u32string to_u32(std::string_view s) {
    std::u32string o;
    for (size_t i = 0; i < s.size();) {
        auto b = uint8_t(s[i]);
        int n = b < 0x80 ? 1 : b >= 0xF0 ? 4 : b >= 0xE0 ? 3 : b >= 0xC0 ? 2 : 1;
        char32_t c = n == 1 ? b : b & (0x3F >> (n - 1));
        for (int k = 1; k < n && i + k < s.size(); k++) c = c << 6 | (uint8_t(s[i + k]) & 0x3F);
        o.push_back(c);
        i += size_t(n);
    }
    return o;
}

enum Cls { Sep, AsciiLetter, Latin, Cyr, Greek, Cjk, HalfKana, Symbol, Bad };

Cls classify(char32_t c) {
    if (c < 0x80) return ((c | 0x20) >= 'a' && (c | 0x20) <= 'z') ? AsciiLetter : Sep;
    if ((c >= 0x80 && c < 0xA0) || c == 0xFFFD || (c >= 0xE000 && c <= 0xF8FF)) return Bad;
    if (c == 0xA0 || c == 0x3000 || (c >= 0x2000 && c <= 0x200A)) return Sep;
    if ((c >= 0xC0 && c <= 0x24F && c != 0xD7 && c != 0xF7) || (c >= 0x1E00 && c <= 0x1EFF)) return Latin;
    if (c >= 0x400 && c <= 0x4FF) return Cyr;
    if (c >= 0x370 && c <= 0x3FF) return Greek;
    if (c >= 0xFF61 && c <= 0xFF9F) return HalfKana;
    if ((c >= 0x3040 && c <= 0x30FF) || (c >= 0x3400 && c <= 0x9FFF) || (c >= 0xAC00 && c <= 0xD7AF) || (c >= 0xF900 && c <= 0xFAFF) ||
        (c >= 0xFF01 && c <= 0xFF5E))
        return Cjk;
    // typographic punctuation is common in titles and neutral
    static const std::u32string neutral = U"©®°«»–—‘’‚“”„…•·¡¿†‡‰‹›™€";
    if (neutral.find(c) != std::u32string::npos) return Sep;
    return Symbol;
}

char32_t to_lower(char32_t c) {
    if ((c >= 0xC0 && c <= 0xDE && c != 0xD7)) return c + 0x20;
    if (c >= 0x100 && c <= 0x17F) return (c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E) ? (c % 2 ? c + 1 : c) : (c % 2 ? c : c + 1);
    if (c >= 0x218 && c <= 0x21B) return c % 2 ? c : c + 1;
    if (c == 0x178) return 0xFF;
    return c;
}

bool is_upper_cyr_greek(char32_t c) { return (c >= 0x400 && c <= 0x42F) || (c >= 0x391 && c <= 0x3AB); }

double score(std::string_view text, const Candidate& cand) {
    auto u = to_u32(text);
    double s = 0;
    std::u32string latin;  // non-ASCII Latin letters, lower case
    size_t i = 0;
    while (i < u.size()) {
        Cls c = classify(u[i]);
        if (c == Bad) {
            s -= 10;
            i++;
            continue;
        }
        if (c == Sep) {
            i++;
            continue;
        }
        size_t j = i;
        int n[Bad + 1] = {};
        while (j < u.size()) {
            Cls k = classify(u[j]);
            if (k == Sep || k == Bad) break;
            n[k]++;
            j++;
        }
        int letters = n[AsciiLetter] + n[Latin];
        int scripts = (letters > 0) + (n[Cyr] > 0) + (n[Greek] > 0) + (n[Cjk] + n[HalfKana] > 0);
        if (scripts > 1) s -= 5;
        bool has_letters = scripts > 0;
        bool prev_lower = false;
        for (size_t k = i; k < j; k++) {
            char32_t ch = u[k];
            switch (classify(ch)) {
            case Latin: latin.push_back(to_lower(ch)); break;
            case Cyr:
            case Greek: {
                bool up = is_upper_cyr_greek(ch);
                s += up ? 0.3 : 1;
                if (up && prev_lower) s -= 2;
                prev_lower = !up;
                continue;
            }
            case Cjk: s += scripts == 1 ? 2 : 0; break;
            case HalfKana: s -= 1; break;
            case Symbol: s -= has_letters ? 2 : 1; break;
            default: break;
            }
            prev_lower = classify(ch) == AsciiLetter && ch >= 'a';
        }
        if (n[Latin] > 0 && letters >= 3 && n[Latin] * 2 > letters) s -= 3;
        i = j;
    }
    // accented letters must be explainable by one language written in this charset
    double best = -double(latin.size());
    for (auto& lang : cand.languages) {
        double ls = 0;
        for (char32_t c : latin) ls += lang.find(c) != std::u32string::npos ? 1 : -1;
        best = std::max(best, ls);
    }
    return s + best;
}

}  // namespace

Decoded decode_text(std::span<const uint8_t> raw) {
    if (raw.size() >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF) return {lossy_utf8(raw.subspan(3)), "UTF-8"};
    if (raw.size() >= 2 && raw[0] == 0xFF && raw[1] == 0xFE) return {iconv_decode(raw.subspan(2), "UTF-16LE", true).value_or(""), "UTF-16LE"};
    if (raw.size() >= 2 && raw[0] == 0xFE && raw[1] == 0xFF) return {iconv_decode(raw.subspan(2), "UTF-16BE", true).value_or(""), "UTF-16BE"};
    if (auto s = iconv_decode(raw, "UTF-8", false); s && *s == str_of(raw)) return {*s, "UTF-8"};

    Decoded best{decode_1252(raw), "windows-1252"};
    double best_score = -1e300;
    for (auto& c : candidates()) {
        std::optional<std::string> t = c.iconv ? iconv_decode(raw, c.iconv, !c.multibyte) : decode_1252(raw);
        if (!t) continue;
        double sc = score(*t, c);
        if (sc > best_score) {
            best_score = sc;
            best = {*t, c.name};
        }
    }
    return best;
}

}  // namespace wm
