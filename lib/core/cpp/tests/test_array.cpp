#include "pr/array.h"
#include "pr/string.h"

#include "tools/harness/test.h"

namespace pr_impl {
namespace {

// An element that counts the instances alive, so that a test sees each one destroyed exactly once.
int alive = 0;

struct Counted {
    int value = 0;
    Counted() { ++alive; }
    Counted(int v) : value(v) { ++alive; }
    Counted(const Counted& other) : value(other.value) { ++alive; }
    Counted(Counted&& other) : value(other.value) {
        other.value = -1;
        ++alive;
    }
    Counted& operator=(const Counted& other) = default;
    Counted& operator=(Counted&& other) {
        value = other.value;
        other.value = -1;
        return *this;
    }
    ~Counted() { --alive; }
    bool operator==(int v) const { return value == v; }
};

} // namespace
} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("an array destroys each element once, through growth, copies and moves") {
    {
        Array<Counted> a;
        for (int i = 0; i < 100; ++i)
            a.push(Counted(i));
        CHECK(alive == 100);
        Array<Counted> copy = a;
        CHECK(alive == 200);
        Array<Counted> moved = move(copy);
        CHECK(alive == 200);
        CHECK(copy.count() == 0);
        CHECK(moved.count() == 100);
        CHECK(moved[99].value == 99);
        a.erase(10, 5);
        CHECK(a.count() == 95);
        CHECK(a[10].value == 15);
        CHECK(alive == 195);
        a.resize(3);
        CHECK(alive == 103);
    }
    CHECK(alive == 0);
}

TEST_CASE("an array inserts, finds and removes, and takes an element of its own") {
    Array<String> names;
    names.push("b");
    names.push("d");
    names.insert(0, "a");
    names.insert(2, "c");
    REQUIRE(names.count() == 4);
    CHECK(names[0] == "a");
    CHECK(names[2] == "c");
    CHECK(names.find("d") == 3);
    CHECK(names.removeFirst("b"));
    CHECK(names.find("b") == kArrayMissing);
    // Pushing an element of the array copies it before the storage grows.
    for (int i = 0; i < 20; ++i)
        names.push(names[0]);
    CHECK(names.last() == "a");
    String joined;
    for (const String& name : names)
        joined.append(name);
    CHECK(joined.count() == names.count());
}

TEST_CASE("a trivial array copies its bytes and hands its storage to a RawArray") {
    Array<int> numbers({1, 2, 3});
    numbers.resize(5);
    CHECK(numbers[4] == 0);
    RawArray<int> raw = numbers.take();
    CHECK(numbers.count() == 0);
    CHECK(numbers.data() == nullptr);
    CHECK(raw.count == 5);
    CHECK(raw.data[2] == 3);
    raw.reset();
}

// A count makes that many elements, and a braced list is the list's elements, whatever their type.
static_assert(!isConstructible<Array<uint32_t>, bool>);
static_assert(!isConstructible<Array<uint32_t>, double>);

TEST_CASE("an array of a count makes that many elements, and a braced list is its elements") {
    Array<uint32_t> counted(5);
    CHECK(counted.count() == 5);
    CHECK(counted[4] == 0);
    Array<uint32_t> one({5u});
    REQUIRE(one.count() == 1);
    CHECK(one[0] == 5);
    Array<uint32_t> three({1u, 2u, 3u});
    CHECK(three.count() == 3);
    Array<uint32_t> none(0);
    CHECK(none.isEmpty());
}

TEST_CASE("an array destroys each element once, through insertion, removal and clearing") {
    {
        Array<Counted> a;
        a.reserve(4);
        for (int i = 0; i < 4; ++i)
            a.emplace(i);
        Counted more[] = {Counted(10), Counted(11), Counted(12)};
        CHECK(alive == 7);
        a.insert(1, Span(more));
        CHECK(alive == 10);
        REQUIRE(a.count() == 7);
        CHECK(a[0] == 0);
        CHECK(a[1] == 10);
        CHECK(a[3] == 12);
        CHECK(a[4] == 1);
        CHECK(a[6] == 3);
        CHECK(a.removeWhere([](const Counted& c) { return c.value >= 10; }) == 3);
        CHECK(alive == 7);
        CHECK(a.count() == 4);
        CHECK(a[3] == 3);
        a.pop();
        CHECK(alive == 6);
        a.append(more, 3);
        CHECK(alive == 9);
        a.clear();
        CHECK(alive == 3);
        CHECK(a.capacity() != 0);
        a.push(Counted(1));
        a.reset();
        CHECK(alive == 3);
        CHECK(a.capacity() == 0);
    }
    CHECK(alive == 0);
}

TEST_CASE("an array compares its elements in order, and an array of bytes is a byte view") {
    Array<int> a({1, 2, 3});
    Array<int> b({1, 2, 3});
    CHECK(a == b);
    b.last() = 4;
    CHECK(!(a == b));
    CHECK(a.contains(2));
    CHECK(!a.contains(4));
    Array<uint8_t> bytes({uint8_t(1), uint8_t(2)});
    PrByteView view = bytes;
    CHECK(view.size == 2);
    CHECK(view.data == bytes.data());
}

TEST_CASE("an array of copies of one value") {
    Array<int> sevens(3, 7);
    CHECK(sevens == Array<int>({7, 7, 7}));
    Array<String> names(2u, String("x"));
    CHECK(names.count() == 2);
    CHECK(names[1] == "x");
    CHECK(Array<int>(0, 1).isEmpty());
}
