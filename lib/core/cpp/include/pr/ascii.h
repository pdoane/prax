// ASCII: classification and case folding of single bytes of text, and scans and comparisons of text built on them.
//
// Classify a decoded code point through asciiOf, because narrowing a code point such as U+0130 to a byte would give
// '0'. What counts as whitespace or an identifier differs between contexts, so this file holds only what ASCII itself
// defines.

#pragma once

#include "pr/base.h"
#include "pr/bit_cast.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr {

// The bytes that asciiIsSpace accepts, as a set of bits indexed by byte.
constexpr uint64_t kAsciiSpaces = 1ull << '\t' | 1ull << '\n' | 1ull << '\f' | 1ull << '\r' | 1ull << ' ';

// Returns the byte of an ASCII code point, or 0 for every code point above ASCII.
constexpr char asciiOf(uint32_t codePoint) {
    return codePoint < 0x80 ? char(codePoint) : '\0';
}

// Returns true if `c` is an ASCII digit, hexadecimal digit or letter.
constexpr bool asciiIsDigit(char c) {
    return c >= '0' && c <= '9';
}
constexpr bool asciiIsHexDigit(char c) {
    return asciiIsDigit(c) || ((c | 0x20) >= 'a' && (c | 0x20) <= 'f');
}
constexpr bool asciiIsLetter(char c) {
    return (c | 0x20) >= 'a' && (c | 0x20) <= 'z';
}

// Returns true if `c` is ASCII whitespace: tab, line feed, form feed, carriage return or space.
constexpr bool asciiIsSpace(char c) {
    return uint8_t(c) <= ' ' && ((kAsciiSpaces >> uint8_t(c)) & 1) != 0;
}

// Returns true if `c` is a tab or a space, the whitespace that HTTP allows around a field's value.
constexpr bool asciiIsTabOrSpace(char c) {
    return c == ' ' || c == '\t';
}

// Returns true if `c` may appear in an HTTP token: a letter, a digit, or one of !#$%&'*+-.^_`|~.
constexpr bool asciiIsHttpToken(char c) {
    switch (c) {
        case '!': case '#': case '$': case '%': case '&': case '\'': case '*': case '+': case '-': case '.': case '^':
        case '_': case '`': case '|': case '~':
            return true;
        default:
            return asciiIsDigit(c) || asciiIsLetter(c);
    }
}

// Returns the index of the first ASCII whitespace byte of `text` at or after `from`, or `text.size` if there is none.
// The scan skips eight bytes at a time while none of them is below '!'.
inline size_t asciiFindSpace(PrStringView text, size_t from) {
    size_t at = from;
    for (; at + 8 <= text.size; at += 8) {
        uint64_t word = loadUnaligned<uint64_t>(text.data + at);
        if (((word - 0x2121212121212121ull) & ~word & 0x8080808080808080ull) != 0)
            break;
    }
    for (; at < text.size; ++at) {
        if (asciiIsSpace(text.data[at]))
            return at;
    }
    return text.size;
}

// Returns true if `c` is in U+0020 through U+007E, which is space or a visible character.
constexpr bool asciiIsPrintable(char c) {
    return c >= ' ' && c <= '~';
}

// Returns true if `c` is an ASCII uppercase or lowercase letter.
constexpr bool asciiIsUpper(char c) {
    return c >= 'A' && c <= 'Z';
}
constexpr bool asciiIsLower(char c) {
    return c >= 'a' && c <= 'z';
}

// Returns `c` folded to lowercase. A byte that is not an ASCII uppercase letter is unchanged.
constexpr char asciiLower(char c) {
    return asciiIsUpper(c) ? char(c + ('a' - 'A')) : c;
}

// Folds each ASCII uppercase letter of `text` to lowercase, in place.
inline void asciiLower(char* text, size_t size) {
    for (size_t i = 0; i < size; ++i)
        text[i] = asciiLower(text[i]);
}

// Returns `c` folded to uppercase. A byte that is not an ASCII lowercase letter is unchanged.
constexpr char asciiUpper(char c) {
    return asciiIsLower(c) ? char(c - ('a' - 'A')) : c;
}

// Folds each ASCII lowercase letter of `text` to uppercase, in place.
inline void asciiUpper(char* text, size_t size) {
    for (size_t i = 0; i < size; ++i)
        text[i] = asciiUpper(text[i]);
}

// Returns the value of the hexadecimal digit `c`, from 0 to 15. `c` must be a hexadecimal digit.
constexpr uint32_t asciiHexValue(char c) {
    return asciiIsDigit(c) ? uint32_t(c - '0') : uint32_t((c | 0x20) - 'a' + 10);
}

// Returns true if `text` equals `other` once the ASCII uppercase letters of `other` are folded to lowercase. `text` is
// not folded.
inline bool asciiEqualsLowered(PrStringView text, PrStringView other) {
    if (text.size != other.size)
        return false;
    for (size_t i = 0; i < text.size; ++i) {
        if (text.data[i] != asciiLower(other.data[i]))
            return false;
    }
    return true;
}

// Returns true if `a` and `b` are equal, with ASCII case folded on both sides.
inline bool asciiEqualsFold(PrStringView a, PrStringView b) {
    if (a.size != b.size)
        return false;
    for (size_t i = 0; i < a.size; ++i) {
        if (asciiLower(a.data[i]) != asciiLower(b.data[i]))
            return false;
    }
    return true;
}

// Orders `a` and `b` by their bytes, with ASCII case folded on both sides. A prefix orders before the longer text.
// Returns negative, zero or positive.
inline int asciiCompareFold(PrStringView a, PrStringView b) {
    size_t common = a.size < b.size ? a.size : b.size;
    for (size_t i = 0; i < common; ++i) {
        char x = asciiLower(a.data[i]);
        char y = asciiLower(b.data[i]);
        if (x != y)
            return uint8_t(x) < uint8_t(y) ? -1 : 1;
    }
    return a.size < b.size ? -1 : a.size > b.size ? 1 : 0;
}

// Returns true if `text` begins with `prefix`, with ASCII case folded on both sides.
inline bool asciiStartsWithFold(PrStringView text, PrStringView prefix) {
    if (text.size < prefix.size)
        return false;
    return asciiEqualsFold(PrStringView{text.data, prefix.size}, prefix);
}

// Returns true if `text` ends with `suffix`, with ASCII case folded on both sides.
inline bool asciiEndsWithFold(PrStringView text, PrStringView suffix) {
    if (text.size < suffix.size)
        return false;
    return asciiEqualsFold(PrStringView{text.data + text.size - suffix.size, suffix.size}, suffix);
}

// Returns true if `needle` appears anywhere in `text`, with ASCII case folded on both sides. The empty needle is in
// every text.
inline bool asciiContainsFold(PrStringView text, PrStringView needle) {
    for (size_t at = 0; at + needle.size <= text.size; ++at) {
        if (asciiEqualsFold(PrStringView{text.data + at, needle.size}, needle))
            return true;
    }
    return false;
}

} // namespace pr

PR_ASSUME_NONNULL_END
