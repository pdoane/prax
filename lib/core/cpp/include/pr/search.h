// Searches over sorted arrays.

#pragma once

#include "pr/base.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr {

// Returns the first index from `first` to `end` for which `before` is false, or `end` if it is true for every index.
// The range must be partitioned: `before` is true for each index before that one and false for each after.
template <typename Before>
uint32_t partitionPoint(uint32_t first, uint32_t end, Before before) {
    uint32_t count = end - first;
    while (count != 0) {
        uint32_t half = count / 2;
        if (before(first + half)) {
            first += half + 1;
            count -= half + 1;
        } else {
            count = half;
        }
    }
    return first;
}

// Returns the index of the first of the `count` items that is not less than `key`, or `count` if every item is less.
// The items must be in ascending order under `<`.
template <typename T, typename Key>
uint32_t lowerBound(const T* PR_NONNULL items, uint32_t count, const Key& key) {
    return partitionPoint(0, count, [&](uint32_t i) { return items[i] < key; });
}

// Returns the index of the first of the `count` items that is greater than `key`, or `count` if no item is. The items
// must be in ascending order under `<`.
template <typename T, typename Key>
uint32_t upperBound(const T* PR_NONNULL items, uint32_t count, const Key& key) {
    return partitionPoint(0, count, [&](uint32_t i) { return !(key < items[i]); });
}

} // namespace pr

PR_ASSUME_NONNULL_END
