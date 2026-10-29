// Sorting and reordering in place.

#pragma once

#include "pr/base.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr {

// Sorts the `count` items in place by `less`, which must be a strict weak order. The sort is not stable. It is a
// heapsort, which takes n log n time for any input order, and it uses an insertion sort for a few items.
template <typename T, typename Less>
void sortItems(T* PR_NULLABLE items, uint32_t count, Less less) {
    if (items == nullptr || count < 2)
        return;
    if (count <= 16) {
        for (uint32_t i = 1; i < count; ++i) {
            T item = items[i];
            uint32_t j = i;
            for (; j > 0 && less(item, items[j - 1]); --j)
                items[j] = items[j - 1];
            items[j] = item;
        }
        return;
    }
    auto sift = [&](uint32_t root, uint32_t end) {
        while (2 * root + 1 < end) {
            uint32_t child = 2 * root + 1;
            if (child + 1 < end && less(items[child], items[child + 1]))
                ++child;
            if (!less(items[root], items[child]))
                return;
            T swap = items[root];
            items[root] = items[child];
            items[child] = swap;
            root = child;
        }
    };
    for (uint32_t i = count / 2; i-- > 0;)
        sift(i, count);
    for (uint32_t end = count - 1; end > 0; --end) {
        T swap = items[0];
        items[0] = items[end];
        items[end] = swap;
        sift(0, end);
    }
}

// Reverses the order of the `count` items in place.
template <typename T>
void reverseItems(T* PR_NULLABLE items, uint32_t count) {
    for (uint32_t lo = 0, hi = count; lo + 1 < hi; ++lo, --hi) {
        T swap = items[lo];
        items[lo] = items[hi - 1];
        items[hi - 1] = swap;
    }
}

} // namespace pr

PR_ASSUME_NONNULL_END
