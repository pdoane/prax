#include "pr/memory.h"
#include "pr/posix_string.h"
#include "pr/string_ops.h"

#include "tools/harness/test.h"

TEST_CASE("posix strings: a copy holds the text and a NUL, and text with a NUL does not convert") {
    char* copy = prPosixStringCopy("path/to file");
    REQUIRE(copy != nullptr);
    CHECK(PrStringView(copy) == "path/to file");
    prMemoryFree(copy);

    char* empty = prPosixStringCopy(PrStringView{});
    REQUIRE(empty != nullptr);
    CHECK(empty[0] == 0);
    prMemoryFree(empty);

    // A view of part of a longer string copies only the part.
    char* part = prPosixStringCopy(PrStringView{"abcdef", 3});
    REQUIRE(part != nullptr);
    CHECK(PrStringView(part) == "abc");
    prMemoryFree(part);

    CHECK(prPosixStringCopy(PrStringView{"a\0b", 3}) == nullptr);
}
