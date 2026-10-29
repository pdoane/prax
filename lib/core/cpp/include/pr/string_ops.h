// String views: comparing, searching and trimming text.

#pragma once

#include "pr/ascii.h"
#include "pr/assert.h"
#include "pr/base.h"
#include "pr/mem.h"
#include "pr/string.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr {

// Returns the `count` bytes of `text` starting at `start`. `text.data` must not be null, and the range must be inside
// `text`.
inline PrStringView stringSlice(PrStringView text, size_t start, size_t count) {
    return PrStringView{text.data + start, count};
}

// Returns true if `text` starts with `prefix`.
inline bool stringStartsWith(PrStringView text, PrStringView prefix) {
    return text.size >= prefix.size
        && (prefix.size == 0 || memEquals(text.data, prefix.data, prefix.size));
}

// Returns true if `text` ends with `suffix`.
inline bool stringEndsWith(PrStringView text, PrStringView suffix) {
    return text.size >= suffix.size
        && (suffix.size == 0
            || memEquals(text.data + text.size - suffix.size, suffix.data, suffix.size));
}

// Returns the bytes of `text`, for a reader that takes bytes rather than text.
inline const uint8_t* PR_NULLABLE stringBytes(PrStringView text) {
    return reinterpret_cast<const uint8_t*>(text.data);
}

// Returns the index of the first `byte` in `text`, or `text.size` if there is none.
inline size_t stringFind(PrStringView text, char byte) {
    size_t index = 0;
    while (index < text.size && text.data[index] != byte)
        ++index;
    return index;
}

// Returns the index of the last `byte` in `text`, or `text.size` if there is none.
inline size_t stringFindLast(PrStringView text, char byte) {
    for (size_t index = text.size; index-- > 0;) {
        if (text.data[index] == byte)
            return index;
    }
    return text.size;
}

// Returns the index of the first `needle` in `text`, or `text.size` if there is none. The empty needle is at 0.
inline size_t stringFind(PrStringView text, PrStringView needle) {
    for (size_t at = 0; at + needle.size <= text.size; ++at) {
        if (memEquals(text.data + at, needle.data, needle.size))
            return at;
    }
    return text.size;
}

// Returns true if `needle` appears anywhere in `text`. The empty needle is in every text.
inline bool stringContains(PrStringView text, PrStringView needle) {
    return needle.size == 0 || stringFind(text, needle) != text.size;
}

// Orders `a` and `b` by their bytes. A prefix orders before the longer text. Returns negative, zero or positive.
inline int stringCompare(PrStringView a, PrStringView b) {
    size_t common = a.size < b.size ? a.size : b.size;
    int order = common == 0 ? 0 : memCompare(a.data, b.data, common);
    if (order != 0)
        return order;
    return a.size < b.size ? -1 : a.size > b.size ? 1 : 0;
}

// Returns `text` without the bytes at either end for which `isTrimmed` returns true.
template <typename Predicate>
PrStringView stringTrim(PrStringView text, Predicate isTrimmed) {
    size_t start = 0;
    size_t end = text.size;
    while (start < end && isTrimmed(text.data[start]))
        ++start;
    while (end > start && isTrimmed(text.data[end - 1]))
        --end;
    return stringSlice(text, start, end - start);
}

// Returns `text` without ASCII whitespace at either end.
inline PrStringView stringTrim(PrStringView text) {
    return stringTrim(text, asciiIsSpace);
}

// Returns an owned string over `data`, which holds `size` bytes in an allocation of `capacity` from prMemoryAlloc. The
// string frees the allocation.
inline PrString stringAdopt(char* data, uint32_t size, uint32_t capacity) {
    PR_ASSERT(capacity >= size);
    PrString string = {};
    string.heap.data = data;
    string.heap.size = size;
    string.heap.capacity = capacity;
    string.heap.tag = PrStringKind_Owned;
    return string;
}

} // namespace pr

PR_ASSUME_NONNULL_END
