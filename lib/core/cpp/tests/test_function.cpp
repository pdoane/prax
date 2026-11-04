#include "pr/array.h"
#include "pr/callback.h"
#include "pr/defer.h"
#include "pr/function.h"
#include "pr/lock.h"

#include "tools/harness/test.h"

namespace pr_impl {
namespace {

int twice(int x) {
    return x * 2;
}

// Callbacks in the shape of the C API's: a function that takes the context first, and the context after it.
typedef void (*VisitFn)(void* PR_NULLABLE context, uint32_t value);
typedef bool (*AcceptFn)(void* PR_NULLABLE context, const int* value, float weight);

void visitEach(uint32_t count, VisitFn fn, void* PR_NULLABLE context) {
    for (uint32_t i = 0; i < count; ++i)
        fn(context, i);
}

uint32_t countAccepted(const int* values, uint32_t count, AcceptFn fn, void* PR_NULLABLE context) {
    uint32_t accepted = 0;
    for (uint32_t i = 0; i < count; ++i)
        accepted += fn(context, values + i, 0.5f) ? 1 : 0;
    return accepted;
}

// A callable that can be moved and not copied.
struct MoveOnly {
    MoveOnly() = default;
    MoveOnly(MoveOnly&&) = default;
    MoveOnly(const MoveOnly&) = delete;
    int operator()() const { return 1; }
};

// A result that no int converts to.
struct Unrelated {};

} // namespace
} // namespace pr_impl

using namespace pr_impl;

// A function takes a callable that it can copy, whose result converts to its own, and discards the result of a callable
// held by a function that returns nothing.
static_assert(isConstructible<Function<int()>, int (*)()>);
static_assert(isConstructible<Function<void()>, int (*)()>);
static_assert(isConstructible<Function<double(int)>, int (*)(int)>);
static_assert(!isConstructible<Function<Unrelated()>, int (*)()>);
static_assert(!isConstructible<Function<int(int)>, int (*)()>);
static_assert(!isConstructible<Function<int()>, MoveOnly>);

TEST_CASE("a function that returns nothing calls a callable that returns a value") {
    int calls = 0;
    Function<void()> counted = [&calls] { return ++calls; };
    counted();
    counted();
    CHECK(calls == 2);
}

TEST_CASE("a callback passes a lambda with captures as a C API's function and context") {
    uint32_t total = 0;
    auto add = [&](uint32_t value) { total += value; };
    visitEach(5, PR_SCOPED_CALLBACK(add));
    CHECK(total == 10);

    int values[] = {1, 5, 2, 8};
    int threshold = 3;
    auto above = [&](const int* value, float weight) { return float(*value) * weight * 2 > float(threshold); };
    CHECK(countAccepted(values, 4, PR_SCOPED_CALLBACK(above)) == 2);
}

TEST_CASE("a function holds a plain function by name, and a null function pointer leaves it empty") {
    Function<int(int)> byName = twice;
    CHECK(byName(4) == 8);
    int (*none)(int) = nullptr;
    Function<int(int)> empty = none;
    CHECK(!empty);
}

TEST_CASE("an array inserts at its start, middle and end, and as it grows") {
    Array<Array<int>> rows;
    for (int i = 0; i < 20; ++i)
        rows.insert(0, Array<int>(uint32_t(i)));
    rows.insert(10, Array<int>(uint32_t(100)));
    rows.insert(rows.count(), Array<int>(uint32_t(200)));
    REQUIRE(rows.count() == 22);
    CHECK(rows[0].count() == 19);
    CHECK(rows[9].count() == 10);
    CHECK(rows[10].count() == 100);
    CHECK(rows[11].count() == 9);
    CHECK(rows[20].count() == 0);
    CHECK(rows[21].count() == 200);
}

TEST_CASE("a function calls a small callable inline and a large one on the heap") {
    int calls = 0;
    Function<int(int)> add = [&calls](int x) {
        ++calls;
        return x + 1;
    };
    CHECK(add(1) == 2);
    Array<int> captured({1, 2, 3, 4, 5, 6});
    uint64_t bulk[4] = {10, 20, 30, 40};
    Function<int(int)> sum = [captured, bulk](int x) {
        int total = x + int(bulk[3]);
        for (int value : captured)
            total += value;
        return total;
    };
    CHECK(sum(0) == 61);
    Function<int(int)> copy = sum;
    Function<int(int)> moved = move(add);
    CHECK(!add);
    CHECK(copy(1) == 62);
    CHECK(moved(2) == 3);
    CHECK(calls == 2);
    copy = moved;
    CHECK(copy(3) == 4);
    copy.reset();
    CHECK(!copy);
}

TEST_CASE("a function holds what its callable captures until it goes") {
    Array<int> values({7});
    {
        Function<int()> first = [values] { return values[0]; };
        Function<int()> second = first;
        CHECK(second() == 7);
    }
    CHECK(values[0] == 7);
}

TEST_CASE("a deferred call runs at the end of its scope, and a lock holds its mutex for one") {
    int ran = 0;
    {
        Defer counting([&] { ++ran; });
        CHECK(ran == 0);
    }
    CHECK(ran == 1);
    PrMutex mutex;
    prMutexInit(&mutex);
    {
        Lock locked(&mutex);
        ++ran;
    }
    // The mutex is free again, so taking it does not wait.
    prMutexLock(&mutex);
    prMutexUnlock(&mutex);
    prMutexReset(&mutex);
    CHECK(ran == 2);
}
