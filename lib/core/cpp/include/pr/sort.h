// Sorting and reordering in place.

#pragma once

#include "pr/base.h"
#include "pr/span.h"
#include "pr/type_traits.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr {

// Exchanges the values of `a` and `b`.
template <typename T>
void swapValues(T& a, T& b) {
    T moving(move(a));
    a = move(b);
    b = move(moving);
}

// Sorts `items` in place by `less`, which must be a strict weak order. The sort is not stable. It is a heapsort, which
// takes n log n time for any input order, and it uses an insertion sort for a few items.
template <typename T, typename Less>
void sortItems(Span<T> items, Less less) {
    uint32_t count = items.count();
    if (count < 2)
        return;
    T* data = items.data();
    if (count <= 16) {
        for (uint32_t i = 1; i < count; ++i) {
            T item(move(data[i]));
            uint32_t j = i;
            for (; j > 0 && less(item, data[j - 1]); --j)
                data[j] = move(data[j - 1]);
            data[j] = move(item);
        }
        return;
    }
    auto sift = [&](uint32_t root, uint32_t end) {
        while (2 * root + 1 < end) {
            uint32_t child = 2 * root + 1;
            if (child + 1 < end && less(data[child], data[child + 1]))
                ++child;
            if (!less(data[root], data[child]))
                return;
            swapValues(data[root], data[child]);
            root = child;
        }
    };
    for (uint32_t i = count / 2; i-- > 0;)
        sift(i, count);
    for (uint32_t end = count - 1; end > 0; --end) {
        swapValues(data[0], data[end]);
        sift(0, end);
    }
}

// Reverses the order of `items` in place.
template <typename T>
void reverseItems(Span<T> items) {
    for (uint32_t lo = 0, hi = items.count(); lo + 1 < hi; ++lo, --hi)
        swapValues(items[lo], items[hi - 1]);
}

} // namespace pr

PR_ASSUME_NONNULL_END
