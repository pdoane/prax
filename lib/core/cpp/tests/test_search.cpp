#include "pr/search.h"

#include "tools/harness/test.h"

using namespace pr_impl;

TEST_CASE("a lower bound is the first item not less than the key") {
    const uint32_t items[] = {1, 3, 3, 7};
    CHECK(lowerBound(Span(items, 4), 0u) == 0);
    CHECK(lowerBound(Span(items, 4), 1u) == 0);
    CHECK(lowerBound(Span(items, 4), 2u) == 1);
    CHECK(lowerBound(Span(items, 4), 3u) == 1);
    CHECK(lowerBound(Span(items, 4), 4u) == 3);
    CHECK(lowerBound(Span(items, 4), 8u) == 4);
    CHECK(lowerBound(Span(items, 0), 5u) == 0);
}

TEST_CASE("an upper bound is the first item greater than the key") {
    const uint32_t items[] = {1, 3, 3, 7};
    CHECK(upperBound(Span(items, 4), 0u) == 0);
    CHECK(upperBound(Span(items, 4), 1u) == 1);
    CHECK(upperBound(Span(items, 4), 2u) == 1);
    CHECK(upperBound(Span(items, 4), 3u) == 3);
    CHECK(upperBound(Span(items, 4), 4u) == 3);
    CHECK(upperBound(Span(items, 4), 7u) == 4);
    CHECK(upperBound(Span(items, 0), 5u) == 0);
}
