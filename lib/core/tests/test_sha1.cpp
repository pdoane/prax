#include "pr/array.h"
#include "pr/mem.h"
#include "pr/sha1.h"
#include "pr/string.h"
#include "pr/string_ops.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

PrByteView bytes(PrStringView value) {
    return prStringBytes(value);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("SHA-1 matches the digests of rfc 3174 and FIPS 180") {
    auto hex = [](PrStringView text) {
        uint8_t digest[kPrSha1Bytes];
        prSha1Digest(bytes(text), digest);
        String out;
        for (uint8_t byte : digest) {
            out.append("0123456789abcdef"[byte >> 4]);
            out.append("0123456789abcdef"[byte & 15]);
        }
        return out;
    };
    CHECK(hex("") == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    CHECK(hex("abc") == "a9993e364706816aba3e25717850c26c9cd0d89d");
    // 56 bytes, whose padding needs a second block.
    CHECK(hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
        "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    Array<char> million(1000000);
    prMemFill(million.data(), 'a', million.count());
    CHECK(hex(PrStringView(million.data(), million.count())) == "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
}
