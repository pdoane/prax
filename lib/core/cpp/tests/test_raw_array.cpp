#include "pr/raw_array.h"

#include "tools/harness/test.h"

using namespace pr_impl;

TEST_CASE("an empty array owns nothing") {
    RawArray<uint32_t> array;
    CHECK(array.data == nullptr);
    CHECK(array.count == 0);
    CHECK(array.capacity == 0);
}

TEST_CASE("push keeps every element across growth") {
    RawArray<uint32_t> array;
    for (uint32_t i = 0; i < 1000; ++i)
        array.push(i);

    REQUIRE(array.count == 1000);
    CHECK(array.capacity >= 1000);
    for (uint32_t i = 0; i < 1000; ++i)
        CHECK(array.data[i] == i);
    array.reset();
}

TEST_CASE("emplace returns the element it adds, in the grown storage") {
    RawArray<uint32_t> array;
    array.emplace() = 7;
    CHECK(array.count == 1);
    CHECK(array.data[0] == 7);
    // Each element is written in the same expression as the growth, across many growths.
    for (uint32_t i = 1; i < 100; ++i)
        array.emplace() = i;
    CHECK(array.count == 100);
    CHECK(array.data[99] == 99);
    array.reset();
}

TEST_CASE("reserve grows without disturbing the elements") {
    RawArray<uint32_t> array;
    array.push(1);
    array.push(2);

    array.reserve(500);
    CHECK(array.capacity >= 500);
    CHECK(array.count == 2);
    CHECK(array.data[0] == 1);
    CHECK(array.data[1] == 2);

    uint32_t capacity = array.capacity;
    array.reserve(10);
    CHECK(array.capacity == capacity);
    array.reset();
}

TEST_CASE("an exact reservation sets the capacity, shrinking it or freeing it") {
    RawArray<uint32_t> array;
    array.reserveExact(3);
    CHECK(array.capacity == 3);
    for (uint32_t i = 0; i < 3; ++i)
        array.push(i);
    CHECK(array.capacity == 3);
    array.reserve(100);
    array.pop();
    array.reserveExact(2);
    CHECK(array.capacity == 2);
    CHECK(array.data[0] == 0);
    CHECK(array.data[1] == 1);
    array.clear();
    array.reserveExact(0);
    CHECK(array.data == nullptr);
    CHECK(array.capacity == 0);
}

TEST_CASE("growth at least doubles") {
    RawArray<uint32_t> array;
    array.reserve(1);
    CHECK(array.capacity == 16);
    array.reserve(17);
    CHECK(array.capacity == 32);
    array.reserve(100);
    CHECK(array.capacity == 100);
    array.reset();
}

TEST_CASE("the first allocation is a size, not a count") {
    // Sizing the first allocation by element count would give a 72-byte element the same sixteen elements as a 4-byte
    // one.
    struct Wide {
        uint8_t bytes[72];
    };
    RawArray<Wide> wide;
    wide.reserve(1);
    CHECK(wide.capacity == 4);

    RawArray<uint8_t> narrow;
    narrow.reserve(1);
    CHECK(narrow.capacity == 64);

    wide.reset();
    narrow.reset();
}

TEST_CASE("append copies a run") {
    RawArray<uint32_t> array;
    uint32_t items[] = {10, 11, 12};
    array.append(items, 3);
    array.append(items, 0);

    REQUIRE(array.count == 3);
    CHECK(array.data[0] == 10);
    CHECK(array.data[2] == 12);
    array.reset();
}

TEST_CASE("erase keeps the order of what remains") {
    RawArray<uint32_t> array;
    for (uint32_t i = 0; i < 10; ++i)
        array.push(i);

    array.erase(3, 2);
    REQUIRE(array.count == 8);
    CHECK(array.data[2] == 2);
    CHECK(array.data[3] == 5);

    array.erase(0, 0);
    CHECK(array.count == 8);

    array.erase(0, array.count);
    CHECK(array.count == 0);
    array.reset();
}

TEST_CASE("arrays of different types keep their alignment") {
    RawArray<uint8_t> bytes;
    RawArray<double> doubles;
    for (uint32_t i = 0; i < 100; ++i) {
        bytes.push(uint8_t(i));
        doubles.push(i * 0.5);
    }
    CHECK((uintptr_t(doubles.data) % alignof(double)) == 0);
    for (uint32_t i = 0; i < 100; ++i) {
        CHECK(bytes.data[i] == uint8_t(i));
        CHECK(doubles.data[i] == i * 0.5);
    }
    bytes.reset();
    doubles.reset();
}

TEST_CASE("an array frees what it grew") {
    RawArray<uint32_t> array;
    for (uint32_t i = 0; i < 100; ++i)
        array.push(i);

    CHECK(array.count == 100);
    CHECK(array.capacity >= 100);
    CHECK(array.data[99] == 99);

    array.reset();
    CHECK(array.count == 0);
    CHECK(array.capacity == 0);
    CHECK(array.data == nullptr);

    // Reset leaves an empty, reusable array.
    array.push(7);
    CHECK(array.data[0] == 7);
    array.reset();
}

TEST_CASE("an array inserts and erases in place") {
    RawArray<uint32_t> array;
    array.push(0);
    array.push(2);
    array.insert(1, 1);
    array.insert(0, 9);
    CHECK(array.count == 4);
    CHECK(array.data[0] == 9);
    CHECK(array.data[1] == 0);
    CHECK(array.data[2] == 1);
    CHECK(array.data[3] == 2);

    array.erase(0);
    array.erase(1, 2);
    CHECK(array.count == 1);
    CHECK(array.data[0] == 0);
    array.reset();
}
