// Trimming text by a predicate over its bytes. pr/string_ops.h trims ASCII whitespace.

#pragma once

#include "pr/base.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr {

// Returns `text` without the bytes at either end for which `isTrimmed` returns true.
template <typename Predicate>
constexpr PrStringView stringTrimBy(PrStringView text, Predicate isTrimmed) {
    size_t start = 0;
    size_t end = text.size;
    while (start < end && isTrimmed(text.data[start]))
        ++start;
    while (end > start && isTrimmed(text.data[end - 1]))
        --end;
    return PrStringView{text.data + start, end - start};
}

// Returns `text` without the bytes at its end for which `isTrimmed` returns true.
template <typename Predicate>
constexpr PrStringView stringTrimEndBy(PrStringView text, Predicate isTrimmed) {
    size_t end = text.size;
    while (end > 0 && isTrimmed(text.data[end - 1]))
        --end;
    return PrStringView{text.data, end};
}

} // namespace pr

PR_ASSUME_NONNULL_END
