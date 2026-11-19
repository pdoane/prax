// Ranges of blocks of device memory (gpu_shared_ranges.h).

#include "gpu/backends/shared/src/gpu_shared_ranges.h"

#include "pr/align.h"
#include "pr/assert.h"
#include "pr/integer.h"

namespace pr_impl {

namespace {

// A bin, by its level and its subdivision of the level.
struct Bin {
    uint32_t level;
    uint32_t subdivision;
};

// Returns the bin that holds free ranges of `size`. Level zero holds the sizes below kGpuSharedRangeSubdivisions, one
// to a bin. Each level after it holds the sizes of one bit length.
Bin binOf(uint64_t size) {
    uint32_t high = prU64HighestBit(size);
    if (high < kGpuSharedRangeSubdivisionBits)
        return Bin{0, uint32_t(size)};
    uint32_t shift = high - kGpuSharedRangeSubdivisionBits;
    return Bin{shift + 1, uint32_t(size >> shift) & (kGpuSharedRangeSubdivisions - 1)};
}

// Returns the smallest bin whose ranges all hold `size` bytes.
Bin binFor(uint64_t size) {
    uint32_t high = prU64HighestBit(size);
    if (high < kGpuSharedRangeSubdivisionBits)
        return Bin{0, uint32_t(size)};
    uint64_t step = uint64_t(1) << (high - kGpuSharedRangeSubdivisionBits);
    return binOf(size + step - 1);
}

// Moves `*bin` to the first bin at or after it that holds a range. Returns false if there is none.
bool nextUsed(const GpuSharedRanges* ranges, Bin* bin) {
    uint32_t level = bin->level;
    if (bin->subdivision < kGpuSharedRangeSubdivisions && level < kGpuSharedRangeLevels) {
        uint32_t used = ranges->subdivisionsUsed[level] & (0xFFFFu << bin->subdivision) & 0xFFFFu;
        if (used != 0) {
            bin->subdivision = prU64LowestBit(used);
            return true;
        }
    }
    if (level + 1 >= kGpuSharedRangeLevels)
        return false;
    uint64_t levels = ranges->levelsUsed & (~uint64_t(0) << (level + 1));
    if (levels == 0)
        return false;
    bin->level = prU64LowestBit(levels);
    bin->subdivision = prU64LowestBit(ranges->subdivisionsUsed[bin->level]);
    return true;
}

// Returns an entry that holds no range.
GpuSharedRangeIndex entryTake(GpuSharedRanges* ranges) {
    GpuSharedRangeIndex index = ranges->spare;
    if (index != kGpuSharedRangeNone) {
        ranges->spare = ranges->ranges.data[index].nextFree;
        return index;
    }
    index = ranges->ranges.count;
    ranges->ranges.emplace();
    return index;
}

void entryGive(GpuSharedRanges* ranges, GpuSharedRangeIndex index) {
    ranges->ranges.data[index] = GpuSharedRange{.nextFree = ranges->spare};
    ranges->spare = index;
}

// Puts a free range at the head of its bin's list.
void binInsert(GpuSharedRanges* ranges, GpuSharedRangeIndex index) {
    GpuSharedRange& range = ranges->ranges.data[index];
    Bin bin = binOf(range.size);
    GpuSharedRangeIndex& head = ranges->bins[bin.level][bin.subdivision];
    range.free = true;
    range.previousFree = kGpuSharedRangeNone;
    range.nextFree = head;
    if (head != kGpuSharedRangeNone)
        ranges->ranges.data[head].previousFree = index;
    head = index;
    ranges->subdivisionsUsed[bin.level] |= uint16_t(1u << bin.subdivision);
    ranges->levelsUsed |= uint64_t(1) << bin.level;
}

// Takes a free range out of its bin's list.
void binRemove(GpuSharedRanges* ranges, GpuSharedRangeIndex index) {
    GpuSharedRange& range = ranges->ranges.data[index];
    Bin bin = binOf(range.size);
    if (range.nextFree != kGpuSharedRangeNone)
        ranges->ranges.data[range.nextFree].previousFree = range.previousFree;
    if (range.previousFree != kGpuSharedRangeNone) {
        ranges->ranges.data[range.previousFree].nextFree = range.nextFree;
    } else {
        ranges->bins[bin.level][bin.subdivision] = range.nextFree;
        if (range.nextFree == kGpuSharedRangeNone) {
            ranges->subdivisionsUsed[bin.level] &= uint16_t(~(1u << bin.subdivision));
            if (ranges->subdivisionsUsed[bin.level] == 0)
                ranges->levelsUsed &= ~(uint64_t(1) << bin.level);
        }
    }
    range.free = false;
}

// Returns the offset at which `size` bytes at `alignment` lie in a free range, or UINT64_MAX if they do not fit.
uint64_t fitIn(const GpuSharedRange& range, uint64_t size, uint64_t alignment) {
    uint64_t start = alignUp(range.offset, alignment);
    uint64_t padding = start - range.offset;
    return padding <= range.size && range.size - padding >= size ? start : UINT64_MAX;
}

// Makes the free range `into` hold the bytes of its free neighbor `from` too, and frees the neighbor's entry.
void merge(GpuSharedRanges* ranges, GpuSharedRangeIndex into, GpuSharedRangeIndex from) {
    binRemove(ranges, from);
    GpuSharedRange& range = ranges->ranges.data[into];
    GpuSharedRange gone = ranges->ranges.data[from];
    range.size += gone.size;
    if (range.before == from) {
        range.offset = gone.offset;
        range.before = gone.before;
        if (gone.before != kGpuSharedRangeNone)
            ranges->ranges.data[gone.before].after = into;
    } else {
        range.after = gone.after;
        if (gone.after != kGpuSharedRangeNone)
            ranges->ranges.data[gone.after].before = into;
    }
    entryGive(ranges, from);
}

} // namespace

void gpuSharedRangesInit(GpuSharedRanges* ranges) {
    ranges->spare = kGpuSharedRangeNone;
    for (uint32_t level = 0; level < kGpuSharedRangeLevels; ++level) {
        for (GpuSharedRangeIndex& head : ranges->bins[level])
            head = kGpuSharedRangeNone;
    }
}

void gpuSharedRangesReset(GpuSharedRanges* ranges) {
    ranges->ranges.reset();
    *ranges = GpuSharedRanges{};
    gpuSharedRangesInit(ranges);
}

void gpuSharedRangesAddBlock(GpuSharedRanges* ranges, uint32_t block, uint64_t size) {
    PR_ASSERT(size != 0);
    GpuSharedRangeIndex index = entryTake(ranges);
    ranges->ranges.data[index] = GpuSharedRange{
        .size = size,
        .block = block,
        .before = kGpuSharedRangeNone,
        .after = kGpuSharedRangeNone,
    };
    binInsert(ranges, index);
    ranges->freeBytes += size;
}

GpuSharedRangeIndex gpuSharedRangesTake(GpuSharedRanges* ranges, uint64_t size, uint64_t alignment) {
    PR_ASSERT(size != 0 && prU64IsPowerOfTwo(alignment));
    // Every range of the first bin holds the size, but perhaps not at the alignment, so the search goes on to larger
    // bins. It looks at one range of each: a bin whose sizes exceed the size by the alignment always fits.
    Bin bin = binFor(size);
    GpuSharedRangeIndex index = kGpuSharedRangeNone;
    uint64_t start = UINT64_MAX;
    while (nextUsed(ranges, &bin)) {
        GpuSharedRangeIndex head = ranges->bins[bin.level][bin.subdivision];
        start = fitIn(ranges->ranges.data[head], size, alignment);
        if (start != UINT64_MAX) {
            index = head;
            break;
        }
        ++bin.subdivision;
    }
    if (index == kGpuSharedRangeNone)
        return kGpuSharedRangeNone;
    binRemove(ranges, index);

    // The bytes before the aligned offset and after the size stay free, as ranges of their own.
    GpuSharedRange taken = ranges->ranges.data[index];
    if (start != taken.offset) {
        GpuSharedRangeIndex padding = entryTake(ranges);
        ranges->ranges.data[padding] = GpuSharedRange{
            .offset = taken.offset,
            .size = start - taken.offset,
            .block = taken.block,
            .before = taken.before,
            .after = index,
        };
        if (taken.before != kGpuSharedRangeNone)
            ranges->ranges.data[taken.before].after = padding;
        binInsert(ranges, padding);
        taken.size -= start - taken.offset;
        taken.offset = start;
        taken.before = padding;
    }
    if (taken.size != size) {
        GpuSharedRangeIndex rest = entryTake(ranges);
        ranges->ranges.data[rest] = GpuSharedRange{
            .offset = start + size,
            .size = taken.size - size,
            .block = taken.block,
            .before = index,
            .after = taken.after,
        };
        if (taken.after != kGpuSharedRangeNone)
            ranges->ranges.data[taken.after].before = rest;
        binInsert(ranges, rest);
        taken.size = size;
        taken.after = rest;
    }
    taken.free = false;
    taken.previousFree = kGpuSharedRangeNone;
    taken.nextFree = kGpuSharedRangeNone;
    ranges->ranges.data[index] = taken;
    ranges->freeBytes -= size;
    return index;
}

GpuSharedRangeIndex gpuSharedRangesGive(GpuSharedRanges* ranges, GpuSharedRangeIndex range) {
    PR_ASSERT(range < ranges->ranges.count && !ranges->ranges.data[range].free);
    ranges->freeBytes += ranges->ranges.data[range].size;
    GpuSharedRangeIndex before = ranges->ranges.data[range].before;
    if (before != kGpuSharedRangeNone && ranges->ranges.data[before].free)
        merge(ranges, range, before);
    GpuSharedRangeIndex after = ranges->ranges.data[range].after;
    if (after != kGpuSharedRangeNone && ranges->ranges.data[after].free)
        merge(ranges, range, after);
    binInsert(ranges, range);
    return range;
}

bool gpuSharedRangeIsBlock(const GpuSharedRanges* ranges, GpuSharedRangeIndex range) {
    const GpuSharedRange& entry = ranges->ranges.data[range];
    return entry.before == kGpuSharedRangeNone && entry.after == kGpuSharedRangeNone;
}

void gpuSharedRangesRemoveBlock(GpuSharedRanges* ranges, GpuSharedRangeIndex range) {
    PR_ASSERT(ranges->ranges.data[range].free && gpuSharedRangeIsBlock(ranges, range));
    ranges->freeBytes -= ranges->ranges.data[range].size;
    binRemove(ranges, range);
    entryGive(ranges, range);
}

} // namespace pr_impl
