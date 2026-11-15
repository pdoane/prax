#include "pr/resource.h"

#include "tools/harness/test.h"

// The tests' resources are the files of lib/core/tests/fixtures, under the name fixtures.

namespace pr_impl {
namespace {

// Returns the bytes of `blob` as text.
PrStringView textOf(const PrBlob* blob) {
    PrByteView bytes = prBlobBytes(blob);
    return PrStringView{reinterpret_cast<const char*>(bytes.data), bytes.size};
}

} // namespace
} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a resource opens as a blob of the file that the build staged for it") {
    PrBlob* blob = prResourceOpen("fixtures/greeting.txt");
    REQUIRE(blob != nullptr);
    CHECK(textOf(blob) == "resource bytes\n");
    prBlobRelease(blob);
}

TEST_CASE("each file of a directory that is a resource is a resource under the directory's name") {
    PrBlob* blob = prResourceOpen("fixtures/nested/inner.txt");
    REQUIRE(blob != nullptr);
    CHECK(textOf(blob) == "nested\n");
    prBlobRelease(blob);
}

TEST_CASE("an empty resource opens as a blob of no bytes") {
    PrBlob* blob = prResourceOpen("fixtures/empty.txt");
    REQUIRE(blob != nullptr);
    CHECK(prBlobBytes(blob).size == 0);
    prBlobRelease(blob);
}

TEST_CASE("a resource that the program does not have does not open") {
    CHECK(prResourceOpen("fixtures/missing.txt") == nullptr);
}

TEST_CASE("a name that could reach outside the program's resources is not well formed, and does not open") {
    CHECK(prResourceNameIsValid("fixtures/greeting.txt"));
    CHECK(prResourceNameIsValid("a"));
    CHECK(prResourceNameIsValid("a.b/c..d"));
    const char* const malformed[] = {
        "",
        "/fixtures/greeting.txt",
        "fixtures//greeting.txt",
        "fixtures/greeting.txt/",
        "./fixtures/greeting.txt",
        "fixtures/./greeting.txt",
        "../fixtures/greeting.txt",
        "fixtures/../fixtures/greeting.txt",
        "fixtures\\greeting.txt",
        "..",
        ".",
    };
    for (const char* name : malformed) {
        CAPTURE(name);
        CHECK(!prResourceNameIsValid(name));
        CHECK(prResourceOpen(name) == nullptr);
    }
}
