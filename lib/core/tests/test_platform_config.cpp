// A build of core configured by test_platform_config.h. Its memory comes from a fixed arena here, and nothing else is
// linked to provide it: the default backends are left out.

#include "core/tests/test_platform_config.h"

#include "pr/memory.h"
#include "pr/string.h"

#include "doctest/doctest.h"

#include <stdlib.h>
#include <string.h>

namespace pr_impl {

namespace {

// Each block is its size, then its bytes, carved from the arena in order and never reused.
alignas(16) uint8_t arena[1 << 16];
size_t arenaUsed = 0;
size_t allocations = 0;
size_t frees = 0;

size_t blockSize(const void* memory) {
    size_t size;
    memcpy(&size, static_cast<const uint8_t*>(memory) - 16, sizeof(size));
    return size;
}

} // namespace

void* PR_NULLABLE configAllocBytes(size_t size) {
    size_t total = 16 + ((size + 15) & ~size_t(15));
    if (arenaUsed + total > sizeof(arena))
        return nullptr;
    uint8_t* block = arena + arenaUsed;
    arenaUsed += total;
    memcpy(block, &size, sizeof(size));
    memset(block + 16, 0xAB, size);
    ++allocations;
    return block + 16;
}

void* PR_NULLABLE configReallocBytes(void* memory, size_t size) {
    void* moved = configAllocBytes(size);
    if (moved != nullptr) {
        size_t old = blockSize(memory);
        memcpy(moved, memory, old < size ? old : size);
        --allocations;
    }
    return moved;
}

void configFreeBytes(void* memory) {
    (void)memory;
    ++frees;
}

void configFailed(PrStringView message, PrStringView file, uint32_t line) {
    (void)message;
    (void)file;
    (void)line;
    abort();
}

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a configured build allocates through the configured functions") {
    size_t before = allocations;
    void* memory = prMemoryAlloc(24);
    CHECK(allocations == before + 1);
    memory = prMemoryRealloc(memory, 100);
    CHECK(allocations == before + 1);
    size_t freed = frees;
    prMemoryFree(memory);
    CHECK(frees == freed + 1);
}

TEST_CASE("the C API allocates through the configured functions") {
    size_t before = allocations;
    PrString string = prStringCopy("a string too long to be stored inline in the string itself");
    CHECK(allocations == before + 1);
    CHECK(prStringView(&string) == "a string too long to be stored inline in the string itself");
    prStringReset(&string);
}

TEST_CASE("a configured build without zeroed allocation zeroes after allocating") {
    uint8_t* bytes = static_cast<uint8_t*>(prMemoryAllocZeroed(64));
    CHECK(bytes[0] == 0);
    CHECK(bytes[63] == 0);
    prMemoryFree(bytes);
}

TEST_CASE("a configured build without usable sizes reports zero") {
    void* memory = prMemoryAlloc(8);
    CHECK(prMemoryUsableSize(memory) == 0);
    prMemoryFree(memory);
}
