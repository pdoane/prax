// Case mapping in context: the conditional mappings of SpecialCasing.txt, which unicode_data.cpp's unconditional
// mappings leave out.

#include "pr/unicode.h"

#include "pr/locale_full.h"
#include "pr/utf8.h"

namespace pr_impl {

namespace {

// Combining classes that end the reach of a casing context (The Unicode Standard, Table 3-17): a starter, and a mark
// above.
constexpr uint8_t kClassAbove = 230;

// Returns true if `codePoint` ends the search of a casing context that looks past combining marks.
bool endsContext(uint32_t codePoint) {
    uint8_t c = prUnicodeCombiningClass(codePoint);
    return c == 0 || c == kClassAbove;
}

// Decodes the code point that ends at byte `end` of `text`, writes it to `*codePoint`, and returns the byte at which it
// starts. `end` must be past the first byte.
size_t decodeBefore(PrStringView text, size_t end, uint32_t* codePoint) {
    size_t start = end - 1;
    while (start > 0 && end - start < kPrUtf8MaxBytes && (uint8_t(text.data[start]) & 0xc0) == 0x80)
        --start;
    prUtf8Decode(text, start, codePoint);
    return start;
}

// Final_Sigma: a cased letter, then any case-ignorable code points, come before the code point at `at`, which ends at
// `end`, and no case-ignorable code points, then a cased letter, come after it.
bool isFinalSigma(PrStringView text, size_t at, size_t end) {
    bool cased = false;
    for (size_t i = at; i > 0 && !cased;) {
        uint32_t c = 0;
        i = decodeBefore(text, i, &c);
        PrUnicodeFlags flags = prUnicodeFlags(c);
        if ((flags & PrUnicodeFlag_CaseIgnorable) != 0)
            continue;
        if ((flags & PrUnicodeFlag_Cased) == 0)
            return false;
        cased = true;
    }
    if (!cased)
        return false;
    for (size_t i = end; i < text.size;) {
        uint32_t c = 0;
        i += prUtf8Decode(text, i, &c);
        PrUnicodeFlags flags = prUnicodeFlags(c);
        if ((flags & PrUnicodeFlag_CaseIgnorable) == 0)
            return (flags & PrUnicodeFlag_Cased) == 0;
    }
    return true;
}

// After_Soft_Dotted, if `soft` is true, or After_I otherwise: a soft-dotted code point, or an I, comes before the code
// point at `at`, with no starter or mark above between them.
bool isAfter(PrStringView text, size_t at, bool soft) {
    for (size_t i = at; i > 0;) {
        uint32_t c = 0;
        i = decodeBefore(text, i, &c);
        if (soft ? (prUnicodeFlags(c) & PrUnicodeFlag_SoftDotted) != 0 : c == 'I')
            return true;
        if (endsContext(c))
            return false;
    }
    return false;
}

// More_Above, if `dot` is false, or Before_Dot otherwise: a mark above, or U+0307 COMBINING DOT ABOVE, comes after the
// code point that ends at byte `end`, with no starter or mark above between them.
bool isBefore(PrStringView text, size_t end, bool dot) {
    for (size_t i = end; i < text.size;) {
        uint32_t c = 0;
        i += prUtf8Decode(text, i, &c);
        if (dot ? c == 0x0307 : prUnicodeCombiningClass(c) == kClassAbove)
            return true;
        if (endsContext(c))
            return false;
    }
    return false;
}

// The length that a tailoring returns for a code point that it leaves to the default mapping.
constexpr uint32_t kUntailored = 0xffffffffu;

// Writes `count` code points to `out`, and returns `count`.
uint32_t written(uint32_t* out, uint32_t count, uint32_t a, uint32_t b = 0, uint32_t c = 0) {
    out[0] = a;
    out[1] = b;
    out[2] = c;
    return count;
}

// Writes the Lithuanian mapping of `codePoint` to `out` and returns its length, or returns kUntailored. Lowercase keeps
// the dot of an i, j or į that an accent above it would hide, and uppercase and titlecase remove the dot after a
// soft-dotted letter.
uint32_t lithuanian(PrStringView text, size_t at, size_t end, uint32_t codePoint, PrUnicodeCase target, uint32_t* out) {
    if (target != PrUnicodeCase_Lower)
        return codePoint == 0x0307 && isAfter(text, at, true) ? 0 : kUntailored;
    switch (codePoint) {
        case 0x0049: return isBefore(text, end, false) ? written(out, 2, 0x0069, 0x0307) : kUntailored;
        case 0x004a: return isBefore(text, end, false) ? written(out, 2, 0x006a, 0x0307) : kUntailored;
        case 0x012e: return isBefore(text, end, false) ? written(out, 2, 0x012f, 0x0307) : kUntailored;
        case 0x00cc: return written(out, 3, 0x0069, 0x0307, 0x0300);
        case 0x00cd: return written(out, 3, 0x0069, 0x0307, 0x0301);
        case 0x0128: return written(out, 3, 0x0069, 0x0307, 0x0303);
        default: return kUntailored;
    }
}

// Writes the Turkish and Azerbaijani mapping of `codePoint` to `out` and returns its length, or returns kUntailored.
// Dotted and dotless i are distinct letters, each with its own capital.
uint32_t turkic(PrStringView text, size_t at, size_t end, uint32_t codePoint, PrUnicodeCase target, uint32_t* out) {
    if (target != PrUnicodeCase_Lower)
        return codePoint == 0x0069 ? written(out, 1, 0x0130) : kUntailored;
    switch (codePoint) {
        case 0x0130: return written(out, 1, 0x0069);
        case 0x0307: return isAfter(text, at, false) ? 0 : kUntailored;
        case 0x0049: return written(out, 1, isBefore(text, end, true) ? 0x0069 : 0x0131);
        default: return kUntailored;
    }
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

uint32_t prUnicodeMapCase(PrStringView text, size_t at, PrUnicodeCase target, PrLanguage language,
                          uint32_t out[kPrUnicodeCaseMaxCodePoints]) {
    uint32_t codePoint = 0;
    size_t end = at + prUtf8Decode(text, at, &codePoint);
    uint32_t count = kUntailored;
    if (language == PrLanguage_Lt)
        count = lithuanian(text, at, end, codePoint, target, out);
    else if (language == PrLanguage_Tr || language == PrLanguage_Az)
        count = turkic(text, at, end, codePoint, target, out);
    if (count != kUntailored)
        return count;
    switch (target) {
        case PrUnicodeCase_Lower:
            if (codePoint == 0x03a3 && isFinalSigma(text, at, end))
                return written(out, 1, 0x03c2);
            return prUnicodeLowercase(codePoint, out);
        case PrUnicodeCase_Upper: return prUnicodeUppercase(codePoint, out);
        default: return prUnicodeTitlecase(codePoint, out);
    }
}
