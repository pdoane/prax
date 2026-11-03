// ASCII: classification and case folding of single bytes of text, and scans and comparisons of text built on them.
//
// Classify a decoded code point through prAsciiFromCodePoint, because narrowing a code point such as U+0130 to a byte
// would give '0'. What counts as whitespace or an identifier differs between contexts, so this file holds only what
// ASCII itself defines.

#pragma once

#include "pr/base.h"
#include "pr/mem.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Returns the byte of an ASCII code point, or 0 for every code point above ASCII.
PR_CONSTEXPR char prAsciiFromCodePoint(uint32_t codePoint) {
    return codePoint < 0x80 ? PR_VALUE_CAST(char, codePoint) : '\0';
}

// Returns true if `c` is an ASCII digit, hexadecimal digit or letter.
PR_CONSTEXPR bool prAsciiIsDigit(char c) {
    return c >= '0' && c <= '9';
}
PR_CONSTEXPR bool prAsciiIsHexDigit(char c) {
    return prAsciiIsDigit(c) || ((c | 0x20) >= 'a' && (c | 0x20) <= 'f');
}
PR_CONSTEXPR bool prAsciiIsLetter(char c) {
    return (c | 0x20) >= 'a' && (c | 0x20) <= 'z';
}

// Returns true if `c` is ASCII whitespace: tab, line feed, form feed, carriage return or space. The test reads a set of
// bits indexed by byte, which holds those five.
PR_CONSTEXPR bool prAsciiIsSpace(char c) {
    return PR_VALUE_CAST(uint8_t, c) <= ' ' && ((0x100003600ull >> PR_VALUE_CAST(uint8_t, c)) & 1) != 0;
}

// Returns true if `c` is a tab or a space, the whitespace that HTTP allows around a field's value.
PR_CONSTEXPR bool prAsciiIsTabOrSpace(char c) {
    return c == ' ' || c == '\t';
}

// Returns true if `c` is whitespace in HTTP: tab, line feed, carriage return or space.
PR_CONSTEXPR bool prAsciiIsHttpSpace(char c) {
    return c == '\t' || c == '\n' || c == '\r' || c == ' ';
}

// Returns true if `c` may appear in an HTTP token: a letter, a digit, or one of !#$%&'*+-.^_`|~.
PR_CONSTEXPR bool prAsciiIsHttpToken(char c) {
    switch (c) {
        case '!': case '#': case '$': case '%': case '&': case '\'': case '*': case '+': case '-': case '.': case '^':
        case '_': case '`': case '|': case '~':
            return true;
        default:
            return prAsciiIsDigit(c) || prAsciiIsLetter(c);
    }
}

// Returns true if `c` is in U+0020 through U+007E, which is space or a visible character.
PR_CONSTEXPR bool prAsciiIsPrintable(char c) {
    return c >= ' ' && c <= '~';
}

// Returns true if `c` is an ASCII uppercase or lowercase letter.
PR_CONSTEXPR bool prAsciiIsUpper(char c) {
    return c >= 'A' && c <= 'Z';
}
PR_CONSTEXPR bool prAsciiIsLower(char c) {
    return c >= 'a' && c <= 'z';
}

// Returns `c` folded to lowercase. A byte that is not an ASCII uppercase letter is unchanged.
PR_CONSTEXPR char prAsciiLower(char c) {
    return prAsciiIsUpper(c) ? PR_VALUE_CAST(char, c + ('a' - 'A')) : c;
}

// Returns `c` folded to uppercase. A byte that is not an ASCII lowercase letter is unchanged.
PR_CONSTEXPR char prAsciiUpper(char c) {
    return prAsciiIsLower(c) ? PR_VALUE_CAST(char, c - ('a' - 'A')) : c;
}

// Folds each ASCII uppercase letter of the `size` bytes at `text` to lowercase, in place.
PR_CONSTEXPR void prAsciiLowerText(char* text, size_t size) {
    for (size_t i = 0; i < size; ++i)
        text[i] = prAsciiLower(text[i]);
}

// Folds each ASCII lowercase letter of the `size` bytes at `text` to uppercase, in place.
PR_CONSTEXPR void prAsciiUpperText(char* text, size_t size) {
    for (size_t i = 0; i < size; ++i)
        text[i] = prAsciiUpper(text[i]);
}

// Returns the value of the hexadecimal digit `c`, from 0 to 15. `c` must be a hexadecimal digit.
PR_CONSTEXPR uint32_t prAsciiHexValue(char c) {
    return prAsciiIsDigit(c) ? PR_VALUE_CAST(uint32_t, c - '0') : PR_VALUE_CAST(uint32_t, (c | 0x20) - 'a' + 10);
}

// Returns the index of the first ASCII whitespace byte of `text` at or after `from`, or `text.size` if there is none.
// The scan skips eight bytes at a time while none of them is below '!'.
static inline size_t prAsciiFindSpace(PrStringView text, size_t from) {
    size_t at = from;
    for (; at + 8 <= text.size; at += 8) {
        uint64_t word = prMemReadU64(text.data + at);
        if (((word - 0x2121212121212121ull) & ~word & 0x8080808080808080ull) != 0)
            break;
    }
    for (; at < text.size; ++at) {
        if (prAsciiIsSpace(text.data[at]))
            return at;
    }
    return text.size;
}

// Returns true if `text` equals `other` once the ASCII uppercase letters of `other` are folded to lowercase. `text` is
// not folded.
PR_CONSTEXPR bool prAsciiEqualsLowered(PrStringView text, PrStringView other) {
    if (text.size != other.size)
        return false;
    for (size_t i = 0; i < text.size; ++i) {
        if (text.data[i] != prAsciiLower(other.data[i]))
            return false;
    }
    return true;
}

// Returns true if `a` and `b` are equal, with ASCII case folded on both sides.
PR_CONSTEXPR bool prAsciiEqualsFold(PrStringView a, PrStringView b) {
    if (a.size != b.size)
        return false;
    for (size_t i = 0; i < a.size; ++i) {
        if (prAsciiLower(a.data[i]) != prAsciiLower(b.data[i]))
            return false;
    }
    return true;
}

// Orders `a` and `b` by their bytes, with ASCII case folded on both sides. A prefix orders before the longer text.
// Returns negative, zero or positive.
PR_CONSTEXPR int prAsciiCompareFold(PrStringView a, PrStringView b) {
    size_t common = a.size < b.size ? a.size : b.size;
    for (size_t i = 0; i < common; ++i) {
        char x = prAsciiLower(a.data[i]);
        char y = prAsciiLower(b.data[i]);
        if (x != y)
            return PR_VALUE_CAST(uint8_t, x) < PR_VALUE_CAST(uint8_t, y) ? -1 : 1;
    }
    return a.size < b.size ? -1 : a.size > b.size ? 1 : 0;
}

// Returns true if `text` begins with `prefix`, with ASCII case folded on both sides.
PR_CONSTEXPR bool prAsciiStartsWithFold(PrStringView text, PrStringView prefix) {
    if (text.size < prefix.size)
        return false;
    return prAsciiEqualsFold(PR_LITERAL(PrStringView){text.data, prefix.size}, prefix);
}

// Returns true if `text` ends with `suffix`, with ASCII case folded on both sides.
PR_CONSTEXPR bool prAsciiEndsWithFold(PrStringView text, PrStringView suffix) {
    if (text.size < suffix.size)
        return false;
    return prAsciiEqualsFold(PR_LITERAL(PrStringView){text.data + text.size - suffix.size, suffix.size}, suffix);
}

// Returns true if `needle` appears anywhere in `text`, with ASCII case folded on both sides. The empty needle is in
// every text.
PR_CONSTEXPR bool prAsciiContainsFold(PrStringView text, PrStringView needle) {
    for (size_t at = 0; at + needle.size <= text.size; ++at) {
        if (prAsciiEqualsFold(PR_LITERAL(PrStringView){text.data + at, needle.size}, needle))
            return true;
    }
    return false;
}

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
