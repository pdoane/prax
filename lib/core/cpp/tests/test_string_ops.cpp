#include "pr/string_ops.h"

#include "doctest/doctest.h"

using namespace pr_impl;

TEST_CASE("a prefix and a suffix") {
    CHECK(stringStartsWith("--custom", "--"));
    CHECK_FALSE(stringStartsWith("-", "--"));
    CHECK(stringStartsWith("x", ""));
    CHECK(stringEndsWith("a.css", ".css"));
    CHECK_FALSE(stringEndsWith("css", ".css"));
    CHECK(stringEndsWith(PrStringView{}, ""));
}

TEST_CASE("comparison orders by bytes, then a prefix first") {
    CHECK(stringCompare("a", "b") < 0);
    CHECK(stringCompare("b", "a") > 0);
    CHECK(stringCompare("ab", "ab") == 0);
    CHECK(stringCompare("a", "ab") < 0);
    CHECK(stringCompare("ab", "a") > 0);
    CHECK(stringCompare("", "") == 0);
}

TEST_CASE("trimming drops whitespace at the ends and nothing inside") {
    CHECK(stringTrim("  a b\t\n") == "a b");
    CHECK(stringTrim("   ") == "");
    CHECK(stringTrim("") == "");
    CHECK(stringTrim("-x-", [](char c) { return c == '-'; }) == "x");
    CHECK(stringTrim("\f\r a\v") == "a\v");
}

TEST_CASE("a prefix, a suffix and a substring with ASCII case folded") {
    CHECK(asciiStartsWithFold("Hello", "hE"));
    CHECK(asciiEndsWithFold("Hello", "LLO"));
    CHECK_FALSE(asciiEndsWithFold("lo", "llo"));
    CHECK(asciiContainsFold("Hello", "ELL"));
    CHECK(asciiContainsFold("Hello", ""));
    CHECK_FALSE(asciiContainsFold("\xC3\xA9", "\xC3\x89"));
}

TEST_CASE("a slice is a view into the same bytes") {
    PrStringView text = "margin-top";
    PrStringView slice = stringSlice(text, 7, 3);
    CHECK(slice == "top");
    CHECK(slice.data == text.data + 7);
}

TEST_CASE("string slices support empty ranges") {
    CHECK(stringSlice("", 0, 0).data != nullptr);
    CHECK(stringSlice("abc", 1, 2) == "bc");
}

TEST_CASE("string find returns an index relative to the view or its size when absent") {
    PrStringView text = "a/b/c";
    CHECK(stringFind(text, '/') == 1);
    CHECK(stringFind(stringSlice(text, 2, 3), '/') == 1);
    CHECK(stringFind(stringSlice(text, 4, 1), '/') == 1);
    CHECK(stringFind(stringSlice(text, 5, 0), '/') == 0);
    CHECK(stringFind(PrStringView{}, '/') == 0);
}
