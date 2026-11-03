// Owned strings: what core's sources that make one share.

#pragma once

#include "pr/assert.h"
#include "pr/string.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

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

} // namespace pr_impl

PR_ASSUME_NONNULL_END
