// Ranges of blocks of device memory, which the backends that place their resources share.
//
// A backend allocates device memory in blocks and places many resources in each. This is the bookkeeping: which bytes
// of which block are taken. It allocates no device memory and calls no API, so a backend adds a block when a take
// fails, and frees a block once all of it is free.
//
// The free ranges of every block are kept in bins by size, two levels deep: a level for each power of two, divided
// evenly into kGpuSharedRangeSubdivisions (two-level segregated fit). A take searches bins, not ranges, so its cost
// does not grow with the number of ranges. It takes from the smallest bin whose ranges all hold the request, which
// keeps large ranges whole for large requests. A range that is given back merges with its free neighbors at once, so
// two free ranges are never adjacent, and a block whose resources are all gone is one free range.

#pragma once

#include "pr/base.h"
#include "pr/raw_array.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// A range's index in GpuSharedRanges::ranges, which names a taken range until it is given back.
typedef uint32_t GpuSharedRangeIndex;
constexpr GpuSharedRangeIndex kGpuSharedRangeNone = UINT32_MAX;

// The bins: a level for each bit length of a size, and the subdivisions of each level.
constexpr uint32_t kGpuSharedRangeSubdivisionBits = 4;
constexpr uint32_t kGpuSharedRangeSubdivisions = 1u << kGpuSharedRangeSubdivisionBits;
constexpr uint32_t kGpuSharedRangeLevels = 64 - kGpuSharedRangeSubdivisionBits + 1;

// Bytes of a block that are taken or free.
struct GpuSharedRange {
    uint64_t offset;            // in the block
    uint64_t size;
    uint32_t block;             // the backend's name for the block

    // The ranges before and after this one in its block, by offset, or None at the block's ends.
    GpuSharedRangeIndex before;
    GpuSharedRangeIndex after;

    // The neighbors of a free range in its bin's list, or None. `nextFree` of an entry that holds no range links the
    // entries to reuse.
    GpuSharedRangeIndex previousFree;
    GpuSharedRangeIndex nextFree;

    bool free;
};

// The ranges of a set of blocks. Its owner guards it.
struct GpuSharedRanges {
    RawArray<GpuSharedRange> ranges;        // by GpuSharedRangeIndex
    GpuSharedRangeIndex spare;              // the first entry of `ranges` to reuse, or None
    uint64_t freeBytes;

    // The first free range of each bin, or None, and the bins that hold one: bit `s` of `subdivisionsUsed[l]` for the
    // bin at level `l` and subdivision `s`, and bit `l` of `levelsUsed` if any bit of `subdivisionsUsed[l]` is set.
    GpuSharedRangeIndex bins[kGpuSharedRangeLevels][kGpuSharedRangeSubdivisions];
    uint16_t subdivisionsUsed[kGpuSharedRangeLevels];
    uint64_t levelsUsed;
};

// Makes `ranges`, which is zeroed, hold no block, or frees its storage.
void gpuSharedRangesInit(GpuSharedRanges* ranges);
void gpuSharedRangesReset(GpuSharedRanges* ranges);

// Adds a block of `size` bytes, all free, which `block` names in the ranges taken from it.
void gpuSharedRangesAddBlock(GpuSharedRanges* ranges, uint32_t block, uint64_t size);

// Takes `size` bytes at a multiple of `alignment`, which is a power of two, from any block. Returns the range, whose
// entry gives its block and offset, or None if no free range holds the bytes. `size` is not zero.
GpuSharedRangeIndex gpuSharedRangesTake(GpuSharedRanges* ranges, uint64_t size, uint64_t alignment);

// Gives back a range that was taken, and returns the free range that now holds its bytes.
GpuSharedRangeIndex gpuSharedRangesGive(GpuSharedRanges* ranges, GpuSharedRangeIndex range);

// Returns true if the range is the whole of its block.
bool gpuSharedRangeIsBlock(const GpuSharedRanges* ranges, GpuSharedRangeIndex range);

// Removes a block, all of which is the free range `range`.
void gpuSharedRangesRemoveBlock(GpuSharedRanges* ranges, GpuSharedRangeIndex range);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
