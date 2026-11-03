#include "pr/base64.h"

#include "doctest/doctest.h"

#include <string>

namespace pr_impl {

namespace {

// Returns the input decoded, or "!" if it is not base64.
std::string decoded(const std::string& input) {
    std::string out(prBase64DecodedCapacity(input.size()), '\0');
    size_t size = 0;
    if (!prBase64Decode(input, out.data(), out.size(), &size))
        return "!";
    REQUIRE(size <= out.size());
    out.resize(size);
    return out;
}

std::string encoded(const std::string& input) {
    std::string out(prBase64EncodedSize(input.size()), '\0');
    prBase64Encode(input, out.data(), out.size());
    return out;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("base64 decodes what it encodes") {
    std::string bytes;
    for (int i = 0; i < 300; ++i) {
        CHECK(decoded(encoded(bytes)) == bytes);
        bytes += char(i * 37 + 11);
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
    std::string text = "SGVsbG8sIHdvcmxkIQ==";
    size_t size = 0;
    REQUIRE(prBase64Decode(text, text.data(), text.size(), &size));
    CHECK(text.substr(0, size) == "Hello, world!");

    char out[4] = {};
    std::string input = "SGVsbG8=";
    REQUIRE(prBase64Decode(input, out, 4, &size));
    CHECK(size == 5);
    CHECK(std::string(out, 4) == "Hell");
    REQUIRE(prBase64Decode(input, nullptr, 0, &size));
    CHECK(size == 5);
}
