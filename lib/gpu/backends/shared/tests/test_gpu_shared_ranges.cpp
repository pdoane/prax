// The tests of the ranges that backends place resources in (gpu_shared_ranges.h), which need no device.

#include "pr/array.h"
#include "pr/random.h"

#include "gpu/backends/shared/src/gpu_shared_ranges.h"

#include "tools/harness/test.h"

namespace pr_impl {

using namespace pr;

namespace {

// Ranges with blocks of one size, added when a take finds no room and removed when they hold nothing, as a backend's
// pool of device memory does.
struct Pool {
    GpuSharedRanges ranges{};
    uint64_t blockBytes;
    uint32_t blocks = 0;
    uint32_t nextBlock = 0;
    uint64_t taken = 0;

    explicit Pool(uint64_t bytes) : blockBytes(bytes) {
        gpuSharedRangesInit(&ranges);
    }

    ~Pool() {
        gpuSharedRangesReset(&ranges);
    }

    GpuSharedRangeIndex take(uint64_t size, uint64_t alignment) {
        GpuSharedRangeIndex range = gpuSharedRangesTake(&ranges, size, alignment);
        if (range == kGpuSharedRangeNone) {
            gpuSharedRangesAddBlock(&ranges, nextBlock++, blockBytes);
            ++blocks;
            range = gpuSharedRangesTake(&ranges, size, alignment);
        }
        REQUIRE(range != kGpuSharedRangeNone);
        taken += size;
        return range;
    }

    void give(GpuSharedRangeIndex range) {
        taken -= ranges.ranges.data[range].size;
        GpuSharedRangeIndex freed = gpuSharedRangesGive(&ranges, range);
        if (gpuSharedRangeIsBlock(&ranges, freed)) {
            gpuSharedRangesRemoveBlock(&ranges, freed);
            --blocks;
        }
    }

    // The bytes of the blocks for each byte taken.
    double overhead() const {
        return double(blocks * blockBytes) / double(taken);
    }
};

// A size like a scene's resources: mostly small, some large, up to a sixteenth of a megabyte.
uint64_t mixedSize(PrRandom* random) {
    uint32_t bits = 4 + prRandomU32Below(random, 13);
    return (uint64_t(1) << bits) + prRandomU32Below(random, 1u << bits);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("ranges taken from a block lie at their alignment, within the block, and apart from each other") {
    GpuSharedRanges ranges{};
    gpuSharedRangesInit(&ranges);
    constexpr uint64_t kBlock = 1u << 20;
    gpuSharedRangesAddBlock(&ranges, 7, kBlock);
    PrRandom random{.state = 1};
    struct Taken {
        uint64_t offset;
        uint64_t size;
    };
    Array<Taken> taken;
    uint64_t total = 0;
    for (;;) {
        uint64_t size = 1 + prRandomU32Below(&random, 5000);
        uint64_t alignment = uint64_t(1) << prRandomU32Below(&random, 13);
        GpuSharedRangeIndex index = gpuSharedRangesTake(&ranges, size, alignment);
        if (index == kGpuSharedRangeNone)
            break;
        const GpuSharedRange& range = ranges.ranges.data[index];
        CHECK(range.block == 7);
        CHECK(range.size == size);
        CHECK(range.offset % alignment == 0);
        CHECK(range.offset + size <= kBlock);
        taken.push(Taken{range.offset, size});
        total += size;
    }
    CHECK(ranges.freeBytes == kBlock - total);
    // The block is nearly full before a take of at most 5000 bytes at an alignment of at most 4096 fails.
    CHECK(total > kBlock * 9 / 10);
    bool apart = true;
    for (uint32_t i = 0; i < taken.count(); ++i) {
        for (uint32_t k = i + 1; k < taken.count(); ++k)
            apart = apart && (taken[i].offset + taken[i].size <= taken[k].offset ||
                taken[k].offset + taken[k].size <= taken[i].offset);
    }
    CHECK(apart);
    gpuSharedRangesReset(&ranges);
}

TEST_CASE("ranges given back in any order merge, so a block that holds nothing is one range that takes its size") {
    GpuSharedRanges ranges{};
    gpuSharedRangesInit(&ranges);
    constexpr uint64_t kBlock = 1u << 16;
    gpuSharedRangesAddBlock(&ranges, 0, kBlock);
    PrRandom random{.state = 2};
    Array<GpuSharedRangeIndex> taken;
    for (;;) {
        GpuSharedRangeIndex index = gpuSharedRangesTake(&ranges, 1 + prRandomU32Below(&random, 700), 64);
        if (index == kGpuSharedRangeNone)
            break;
        taken.push(index);
    }
    CHECK(gpuSharedRangesTake(&ranges, kBlock, 1) == kGpuSharedRangeNone);
    GpuSharedRangeIndex freed = kGpuSharedRangeNone;
    while (taken.count() != 0) {
        uint32_t at = prRandomU32Below(&random, taken.count());
        freed = gpuSharedRangesGive(&ranges, taken[at]);
        taken[at] = taken[taken.count() - 1];
        taken.pop();
        CHECK(gpuSharedRangeIsBlock(&ranges, freed) == (taken.count() == 0));
    }
    CHECK(ranges.freeBytes == kBlock);
    GpuSharedRangeIndex whole = gpuSharedRangesTake(&ranges, kBlock, 4096);
    REQUIRE(whole != kGpuSharedRangeNone);
    CHECK(ranges.ranges.data[whole].offset == 0);
    freed = gpuSharedRangesGive(&ranges, whole);
    gpuSharedRangesRemoveBlock(&ranges, freed);
    CHECK(ranges.freeBytes == 0);
    CHECK(gpuSharedRangesTake(&ranges, 1, 1) == kGpuSharedRangeNone);
    gpuSharedRangesReset(&ranges);
}

TEST_CASE("a take finds a range of any size in any of many blocks, and storage does not grow as ranges come and go") {
    Pool pool(1u << 20);
    Array<GpuSharedRangeIndex> taken;
    // Sizes from one byte to a whole block.
    for (uint32_t bits = 0; bits <= 20; ++bits)
        taken.push(pool.take(uint64_t(1) << bits, 1));
    uint32_t entries = pool.ranges.ranges.count;
    for (uint32_t round = 0; round < 100; ++round) {
        for (GpuSharedRangeIndex range : taken)
            pool.give(range);
        CHECK(pool.blocks == 0);
        taken.clear();
        for (uint32_t bits = 0; bits <= 20; ++bits)
            taken.push(pool.take(uint64_t(1) << bits, 1));
    }
    CHECK(pool.ranges.ranges.count == entries);
    for (GpuSharedRangeIndex range : taken)
        pool.give(range);
}

TEST_CASE("fragmentation stays bounded as resources of mixed sizes come and go at a steady total") {
    Pool pool(4u << 20);
    PrRandom random{.state = 3};
    Array<GpuSharedRangeIndex> taken;
    constexpr uint64_t kSteadyBytes = 64u << 20;
    constexpr uint32_t kOperations = 200000;
    // The overhead once the total is first reached, and the worst in each quarter of the churn after it.
    double worst[4] = {};
    while (pool.taken < kSteadyBytes)
        taken.push(pool.take(mixedSize(&random), uint64_t(1) << prRandomU32Below(&random, 9)));
    double filled = pool.overhead();
    for (uint32_t i = 0; i < kOperations; ++i) {
        // Above the total, a resource goes. Below it, one comes.
        if (pool.taken >= kSteadyBytes) {
            uint32_t at = prRandomU32Below(&random, taken.count());
            pool.give(taken[at]);
            taken[at] = taken[taken.count() - 1];
            taken.pop();
        } else {
            taken.push(pool.take(mixedSize(&random), uint64_t(1) << prRandomU32Below(&random, 9)));
        }
        double overhead = pool.overhead();
        double& quarter = worst[uint64_t(i) * 4 / kOperations];
        if (overhead > quarter)
            quarter = overhead;
    }
    MESSAGE("bytes of blocks per byte taken: ", filled, " once filled, then at worst ", worst[0], ", ", worst[1], ", ",
        worst[2], " and ", worst[3], " in each quarter of the churn");
    CHECK(filled < 1.1);
    CHECK(worst[3] < 1.5);
    // The churn reaches a level and stays there: its last quarter is no worse than its first.
    CHECK(worst[3] < worst[0] * 1.02);
    for (GpuSharedRangeIndex range : taken)
        pool.give(range);
    CHECK(pool.blocks == 0);
}
