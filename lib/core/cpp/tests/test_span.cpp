#include "pr/array.h"
#include "pr/span.h"

#include "tools/harness/test.h"

using namespace pr_impl;

// An array converts to a span implicitly, a span of `T` to a span of `const T`, and nothing takes the const away. A
// container gives its elements only when asked.
static_assert(isConvertible<int (&)[3], Span<int>>);
static_assert(isConvertible<const int (&)[3], Span<const int>>);
static_assert(isConvertible<int (&)[3], Span<const int>>);
static_assert(isConvertible<Span<int>, Span<const int>>);
static_assert(!isConvertible<Span<const int>, Span<int>>);
static_assert(!isConvertible<const int (&)[3], Span<int>>);
static_assert(!isConvertible<const Array<int>&, Span<const int>>);

TEST_CASE("a span reads and changes the elements that it views") {
    int values[] = {1, 2, 3, 4, 5};
    Span span(values);
    CHECK(span.count() == 5);
    CHECK(span.first() == 1);
    CHECK(span.last() == 5);
    span[1] = 20;
    CHECK(values[1] == 20);
    int total = 0;
    for (int value : span)
        total += value;
    CHECK(total == 33);
    Span<const int> reading = span.asConst();
    CHECK(reading.data() == values);
    CHECK(reading[1] == 20);
}

TEST_CASE("a span slices into the spans of its parts") {
    int values[] = {1, 2, 3, 4, 5};
    Span span(values);
    Span middle = span.slice(1, 3);
    CHECK(middle.count() == 3);
    CHECK(middle.first() == 2);
    CHECK(middle.last() == 4);
    CHECK(span.from(3).count() == 2);
    CHECK(span.from(5).isEmpty());
    CHECK(span.slice(5, 0).isEmpty());
}

TEST_CASE("spans compare their elements in order") {
    int a[] = {1, 2, 3};
    int b[] = {1, 2, 3};
    int c[] = {1, 2, 4};
    CHECK(Span(a) == Span(b));
    CHECK(!(Span(a) == Span(c)));
    CHECK(!(Span(a) == Span(a).slice(0, 2)));
    CHECK(Span<int>() == Span<int>());
    CHECK(Span<int>().isEmpty());
}
