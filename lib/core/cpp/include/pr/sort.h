// Sorting and reordering in place.

#pragma once

#include "pr/base.h"
#include "pr/memory.h"
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

// The number of items up to which the sorts below use an insertion sort.
constexpr uint32_t kSortInsertionMax = 16;

// Sorts `items` in place by `less`, which must be a strict weak order, by insertion, which keeps the order of items
// that compare equal. It takes quadratic time, and suits a few items.
template <typename T, typename Less>
void insertionSortItems(Span<T> items, Less less) {
    T* data = items.data();
    for (uint32_t i = 1; i < items.count(); ++i) {
        T item(move(data[i]));
        uint32_t j = i;
        for (; j > 0 && less(item, data[j - 1]); --j)
            data[j] = move(data[j - 1]);
        data[j] = move(item);
    }
}

// Sorts `items` in place by `less`, which must be a strict weak order. The sort is not stable. It is a heapsort, which
// takes n log n time for any input order, and it uses an insertion sort for a few items.
template <typename T, typename Less>
void sortItems(Span<T> items, Less less) {
    uint32_t count = items.count();
    if (count < 2)
        return;
    T* data = items.data();
    if (count <= kSortInsertionMax) {
        insertionSortItems(items, less);
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

// Sorts `items` in place by `less`, which must be a strict weak order, keeping the order of items that compare equal.
// It is an insertion sort for a few items, and otherwise a merge sort, which takes n log n time and a buffer of the
// items' size. The items are trivially copyable.
template <typename T, typename Less>
void stableSortItems(Span<T> items, Less less) {
    static_assert(isTriviallyCopyable<T>, "stableSortItems copies its items as bytes");
    uint32_t count = items.count();
    if (count < 2)
        return;
    if (count <= kSortInsertionMax) {
        insertionSortItems(items, less);
        return;
    }
    T* data = items.data();
    T* buffer = static_cast<T*>(prMemoryAlloc(size_t(count) * sizeof(T)));
    T* from = data;
    T* to = buffer;
    // Runs of `width` items merge in pairs. A later run's item goes first only if it is less, which keeps equal items
    // in order.
    for (uint32_t width = 1; width < count; width *= 2) {
        for (uint32_t lo = 0; lo < count; lo += 2 * width) {
            uint32_t mid = lo + width < count ? lo + width : count;
            uint32_t hi = lo + 2 * width < count ? lo + 2 * width : count;
            uint32_t a = lo;
            uint32_t b = mid;
            for (uint32_t k = lo; k < hi; ++k)
                to[k] = a < mid && (b >= hi || !less(from[b], from[a])) ? from[a++] : from[b++];
        }
        T* swapped = from;
        from = to;
        to = swapped;
    }
    if (from != data) {
        for (uint32_t i = 0; i < count; ++i)
            data[i] = from[i];
    }
    prMemoryFree(buffer);
}

// Reverses the order of `items` in place.
template <typename T>
void reverseItems(Span<T> items) {
    for (uint32_t lo = 0, hi = items.count(); lo + 1 < hi; ++lo, --hi)
        swapValues(items[lo], items[hi - 1]);
}

} // namespace pr

PR_ASSUME_NONNULL_END
