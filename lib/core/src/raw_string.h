// Owned strings: what core's sources that make one share.

#pragma once

#include "pr/assert.h"
#include "pr/raw_string.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Returns an owned string over `data`, which holds `size` bytes in an allocation of `capacity` from prMemoryAlloc. The
// string frees the allocation.
inline PrRawString rawStringAdopt(char* data, uint32_t size, uint32_t capacity) {
    PR_ASSERT(capacity >= size);
    PrRawString string = {};
    string.heap.data = data;
    string.heap.size = size;
    string.heap.capacity = capacity;
    string.heap.tag = PrRawStringKind_Owned;
    return string;
}

} // namespace pr_impl

PR_ASSUME_NONNULL_END
