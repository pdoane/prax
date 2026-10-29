#include "pr/reserved_array.h"

#include "doctest/doctest.h"

using namespace pr_impl;

TEST_CASE("a reservation is committed as it fills, and never moves") {
    ReservedArray<uint32_t> array;
    array.prepare(1u << 20);
    CHECK(array.capacity == 1u << 20);
    CHECK(array.mapped);
    CHECK(array.committed == 0);   // reserved and nothing more

    const uint32_t* base = array.data;
    for (uint32_t i = 0; i < 1u << 20; ++i)
        array.push(i);

    CHECK(array.count == 1u << 20);
    CHECK(array.committed == 1u << 20);
    CHECK(array.data == base);     // filling it moved nothing
    CHECK(array.data[0] == 0);     // a page that nothing wrote to reads as zero
    CHECK(array.data[(1u << 20) - 1] == (1u << 20) - 1);

    array.reset();
    CHECK(array.data == nullptr);
    CHECK(array.capacity == 0);
    CHECK(array.committed == 0);
}

TEST_CASE("a commit covers a step at a time, and stops at the capacity") {
    ReservedArray<uint32_t> array;
    array.prepare(1u << 20);

    array.push(0);
    CHECK(array.committed == kVmCommitStep / sizeof(uint32_t));

    ReservedArray<uint32_t> small;
    small.prepare(kVmWorthwhile / sizeof(uint32_t));
    CHECK(small.mapped);
    small.push(0);
    CHECK(small.committed == small.capacity);   // less than a step was left
    small.reset();
    array.reset();
}

TEST_CASE("a small reservation is allocated rather than mapped") {
    // Below kVmWorthwhile, two system calls cost more than the memory.
    ReservedArray<uint32_t> array;
    array.prepare(8);
    CHECK_FALSE(array.mapped);
    CHECK(array.capacity == 8);
    CHECK(array.committed == 8);   // an allocation is committed by being made

    const uint32_t* base = array.data;
    for (uint32_t i = 0; i < 8; ++i)
        array.push(i);
    CHECK(array.data == base);
    array.reset();
}

TEST_CASE("an empty reservation holds nothing") {
    ReservedArray<uint32_t> array;
    array.prepare(0);
    CHECK(array.data == nullptr);
    CHECK(array.capacity == 0);
    array.reset();
}

TEST_CASE("a reservation large enough already is kept as it is") {
    ReservedArray<uint32_t> array;
    array.prepare(1u << 20);
    array.push(7);

    const uint32_t* base = array.data;
    uint32_t committed = array.committed;
    array.prepare(1u << 10);
    CHECK(array.data == base);            // the committed pages stay committed
    CHECK(array.committed == committed);
    CHECK(array.capacity == 1u << 20);
    CHECK(array.count == 0);
    array.reset();
}
