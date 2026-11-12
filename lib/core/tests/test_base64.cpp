#include "pr/array.h"
#include "pr/base64.h"
#include "pr/string.h"
#include "pr/string_ops.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

// Returns the input decoded, or "!" if it is not base64.
String decoded(PrStringView input) {
    Array<char> out(prBase64DecodedCapacity(input.size));
    size_t size = 0;
    if (!prBase64Decode(input, out.data(), out.count(), &size))
        return "!";
    REQUIRE(size <= out.count());
    return String(PrStringView(out.data(), size));
}

String encoded(PrStringView input) {
    Array<char> out(prBase64EncodedSize(input.size));
    prBase64Encode(input, out.data(), out.count());
    return String(PrStringView(out.data(), out.count()));
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("base64 decodes what it encodes") {
    String bytes;
    for (int i = 0; i < 300; ++i) {
        CHECK(decoded(encoded(bytes)) == bytes);
        bytes.append(char(i * 37 + 11));
    }
    CHECK(encoded("Man") == "TWFu");
    CHECK(decoded("TWFu") == "Man");
    CHECK(decoded("TWE=") == "Ma");
    CHECK(decoded("TQ==") == "M");
}

TEST_CASE("base64 ignores whitespace and takes padding as optional") {
    CHECK(decoded("TW\nFu TW E=") == "ManMa");
    CHECK(decoded(" T W\tF u\r\n") == "Man");
    CHECK(decoded("TWE") == "Ma");
    CHECK(decoded("TQ") == "M");
    CHECK(decoded("") == "");
}

TEST_CASE("base64 refuses what is not the alphabet or padded wrongly") {
    CHECK(decoded("TW*u") == "!");
    CHECK(decoded("TWFuT") == "!");        // one past a multiple of four
    CHECK(decoded("TQ===") == "!");
    CHECK(decoded("TQ=") == "!");          // padding short of a multiple of four
    CHECK(decoded("T=Q=") == "!");         // the alphabet after padding
    CHECK(decoded("TWFu\x80") == "!");
}

TEST_CASE("base64 decodes in place and stops at the capacity") {
    const char kText[] = "SGVsbG8sIHdvcmxkIQ==";
    Array<char> text;
    text.append(kText, sizeof(kText) - 1);
    size_t size = 0;
    REQUIRE(prBase64Decode(PrStringView(text.data(), text.count()), text.data(), text.count(), &size));
    CHECK(PrStringView(text.data(), size) == "Hello, world!");

    char out[4] = {};
    PrStringView input = "SGVsbG8=";
    REQUIRE(prBase64Decode(input, out, 4, &size));
    CHECK(size == 5);
    CHECK(PrStringView(out, 4) == "Hell");
    REQUIRE(prBase64Decode(input, nullptr, 0, &size));
    CHECK(size == 5);
}
