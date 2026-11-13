// Tests of the conversions between UTF-8 and the UTF-16 of Windows.

#include "pr/memory.h"
#include "pr/raw_array.h"
#include "pr/windows_string.h"

#include "tools/harness/test.h"

using namespace pr_impl;

TEST_CASE("UTF-8 widens to UTF-16 and narrows back") {
    PrStringView text = "caf\xc3\xa9 \xf0\x9f\x98\x80";
    RawArray<wchar_t> wide;
    CHECK(windowsWiden(text, wide));
    // The emoji is two units, and a NUL ends the array.
    REQUIRE(wide.count == 8);
    CHECK(wide.data[7] == 0);
    RawArray<char> utf8;
    CHECK(windowsNarrow(wide.data, 7, utf8) == text);
    wide.reset();
    utf8.reset();
}

TEST_CASE("an ill-formed string does not convert") {
    CHECK(prWindowsWiden("\xff", nullptr, 0) == kPrWindowsStringInvalid);
    CHECK(prWindowsWidenCopy("\xff", nullptr) == nullptr);
    const wchar_t lone[] = {L'a', wchar_t(0xd800), L'b'};
    CHECK(prWindowsNarrow(lone, 3, nullptr, 0) == kPrWindowsStringInvalid);
    PrRawString narrowed = prWindowsNarrowToRawString(lone, 3);
    CHECK(prRawStringIsNull(&narrowed));
}

TEST_CASE("a widened copy is NUL-terminated and counts its units") {
    uint32_t count = 0;
    wchar_t* wide = prWindowsWidenCopy("ab", &count);
    REQUIRE(wide != nullptr);
    CHECK(count == 2);
    CHECK(wide[0] == L'a');
    CHECK(wide[2] == 0);
    prMemoryFree(wide);
}

TEST_CASE("no units narrow to the empty string") {
    PrRawString narrowed = prWindowsNarrowToRawString(nullptr, 0);
    CHECK(!prRawStringIsNull(&narrowed));
    CHECK(prRawStringView(&narrowed).size == 0);
}

TEST_CASE("a conversion writes nothing to a capacity that does not hold it all") {
    wchar_t out[2] = {L'x', L'x'};
    CHECK(prWindowsWiden("abc", out, 2) == 3);
    CHECK(out[0] == L'x');
}
