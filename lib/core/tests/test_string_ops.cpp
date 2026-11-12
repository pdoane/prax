#include "pr/string_ops.h"
#include "pr/string_trim.h"

#include "tools/harness/test.h"

using namespace pr_impl;

TEST_CASE("a prefix and a suffix") {
    CHECK(prStringStartsWith("--custom", "--"));
    CHECK_FALSE(prStringStartsWith("-", "--"));
    CHECK(prStringStartsWith("x", ""));
    CHECK(prStringEndsWith("a.css", ".css"));
    CHECK_FALSE(prStringEndsWith("css", ".css"));
    CHECK(prStringEndsWith(PrStringView{}, ""));
}

TEST_CASE("comparison orders by bytes, then a prefix first") {
    CHECK(prStringCompare("a", "b") < 0);
    CHECK(prStringCompare("b", "a") > 0);
    CHECK(prStringCompare("ab", "ab") == 0);
    CHECK(prStringCompare("a", "ab") < 0);
    CHECK(prStringCompare("ab", "a") > 0);
    CHECK(prStringCompare("", "") == 0);
}

TEST_CASE("trimming drops whitespace at the ends and nothing inside") {
    CHECK(prStringTrim("  a b\t\n") == "a b");
    CHECK(prStringTrim("   ") == "");
    CHECK(prStringTrim("") == "");
    CHECK(stringTrimBy("-x-", [](char c) { return c == '-'; }) == "x");
    CHECK(prStringTrim("\f\r a\v") == "a\v");
}

TEST_CASE("a prefix, a suffix and a substring with ASCII case folded") {
    CHECK(prAsciiStartsWithFold("Hello", "hE"));
    CHECK(prAsciiEndsWithFold("Hello", "LLO"));
    CHECK_FALSE(prAsciiEndsWithFold("lo", "llo"));
    CHECK(prAsciiContainsFold("Hello", "ELL"));
    CHECK(prAsciiContainsFold("Hello", ""));
    CHECK_FALSE(prAsciiContainsFold("\xC3\xA9", "\xC3\x89"));
}

TEST_CASE("a slice is a view into the same bytes") {
    PrStringView text = "margin-top";
    PrStringView slice = prStringSlice(text, 7, 3);
    CHECK(slice == "top");
    CHECK(slice.data == text.data + 7);
}

TEST_CASE("string slices support empty ranges") {
    CHECK(prStringSlice("", 0, 0).data != nullptr);
    CHECK(prStringSlice("abc", 1, 2) == "bc");
}

TEST_CASE("string find returns an index relative to the view or its size when absent") {
    PrStringView text = "a/b/c";
    CHECK(prStringFindByte(text, '/') == 1);
    CHECK(prStringFindByte(prStringSlice(text, 2, 3), '/') == 1);
    CHECK(prStringFindByte(prStringSlice(text, 4, 1), '/') == 1);
    CHECK(prStringFindByte(prStringSlice(text, 5, 0), '/') == 0);
    CHECK(prStringFindByte(PrStringView{}, '/') == 0);
}

TEST_CASE("a head, a tail and the rest from an index keep what there is") {
    CHECK(prStringHead("abc", 2) == "ab");
    CHECK(prStringHead("abc", 9) == "abc");
    CHECK(prStringTail("abc", 2) == "bc");
    CHECK(prStringTail("abc", 9) == "abc");
    CHECK(prStringFrom("abc", 1) == "bc");
    CHECK(prStringFrom("abc", 9) == "");
}

TEST_CASE("a search from an index, and for the last occurrence") {
    CHECK(prStringFindFrom("abcabc", "bc", 0) == 1);
    CHECK(prStringFindFrom("abcabc", "bc", 2) == 4);
    CHECK(prStringFindFrom("abcabc", "bc", 5) == 6);
    CHECK(prStringFindFrom("abc", "", 2) == 2);
    CHECK(prStringFindLast("abcabc", "bc") == 4);
    CHECK(prStringFindLast("abcabc", "x") == 6);
    CHECK(prStringFindLast("ab", "abc") == 2);
    CHECK(prStringFindLast("abc", "") == 3);
}

TEST_CASE("trimming a byte drops only its run at the end") {
    CHECK(prStringTrimEndByte("ab  ", ' ') == "ab");
    CHECK(prStringTrimEndByte("ab \t", ' ') == "ab \t");
    CHECK(prStringTrimEndByte("   ", ' ') == "");
}
