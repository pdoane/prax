// String views: comparing, searching and trimming text.

#pragma once

#include "pr/ascii.h"
#include "pr/base.h"
#include "pr/mem.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Returns the `count` bytes of `text` starting at `start`. `text.data` must not be null, and the range must be inside
// `text`.
PR_CONSTEXPR PrStringView prStringSlice(PrStringView text, size_t start, size_t count) {
    return PR_LITERAL(PrStringView){text.data + start, count};
}

// Returns true if `text` starts with `prefix`.
static inline bool prStringStartsWith(PrStringView text, PrStringView prefix) {
    return text.size >= prefix.size && (prefix.size == 0 || prMemEquals(text.data, prefix.data, prefix.size));
}

// Returns true if `text` ends with `suffix`.
static inline bool prStringEndsWith(PrStringView text, PrStringView suffix) {
    return text.size >= suffix.size &&
        (suffix.size == 0 || prMemEquals(text.data + text.size - suffix.size, suffix.data, suffix.size));
}

// Returns the bytes of `text`, for a reader that takes bytes rather than text.
static inline const uint8_t* PR_NULLABLE prStringBytes(PrStringView text) {
    return PR_POINTER_CAST(const uint8_t*, text.data);
}

// Returns the index of the first `byte` in `text`, or `text.size` if there is none.
PR_CONSTEXPR size_t prStringFindByte(PrStringView text, char byte) {
    size_t index = 0;
    while (index < text.size && text.data[index] != byte)
        ++index;
    return index;
}

// Returns the index of the last `byte` in `text`, or `text.size` if there is none.
PR_CONSTEXPR size_t prStringFindLastByte(PrStringView text, char byte) {
    for (size_t index = text.size; index-- > 0;) {
        if (text.data[index] == byte)
            return index;
    }
    return text.size;
}

// Returns the index of the first `needle` in `text`, or `text.size` if there is none. The empty needle is at 0.
static inline size_t prStringFind(PrStringView text, PrStringView needle) {
    for (size_t at = 0; at + needle.size <= text.size; ++at) {
        if (prMemEquals(text.data + at, needle.data, needle.size))
            return at;
    }
    return text.size;
}

// Returns true if `needle` appears anywhere in `text`. The empty needle is in every text.
static inline bool prStringContains(PrStringView text, PrStringView needle) {
    return needle.size == 0 || prStringFind(text, needle) != text.size;
}

// Orders `a` and `b` by their bytes. A prefix orders before the longer text. Returns negative, zero or positive.
static inline int prStringCompare(PrStringView a, PrStringView b) {
    size_t common = a.size < b.size ? a.size : b.size;
    int order = common == 0 ? 0 : prMemCompare(a.data, b.data, common);
    if (order != 0)
        return order;
    return a.size < b.size ? -1 : a.size > b.size ? 1 : 0;
}

// Returns `text` without ASCII whitespace at either end.
PR_CONSTEXPR PrStringView prStringTrim(PrStringView text) {
    size_t start = 0;
    size_t end = text.size;
    while (start < end && prAsciiIsSpace(text.data[start]))
        ++start;
    while (end > start && prAsciiIsSpace(text.data[end - 1]))
        --end;
    return PR_LITERAL(PrStringView){text.data + start, end - start};
}

// Returns `text` without ASCII whitespace at its end.
PR_CONSTEXPR PrStringView prStringTrimEnd(PrStringView text) {
    size_t end = text.size;
    while (end > 0 && prAsciiIsSpace(text.data[end - 1]))
        --end;
    return PR_LITERAL(PrStringView){text.data, end};
}

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
