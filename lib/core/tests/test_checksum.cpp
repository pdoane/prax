#include "pr/array.h"
#include "pr/checksum.h"
#include "pr/mem.h"
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

TEST_CASE("CRC-32 and Adler-32 match published check values") {
    CHECK(prCrc32Update(0, {}) == 0);
    CHECK(prAdler32Update(1, {}) == 1);
    CHECK(prCrc32Update(0, bytes("123456789")) == 0xcbf43926);
    CHECK(prAdler32Update(1, bytes("123456789")) == 0x091e01de);
    CHECK(prAdler32Update(1, bytes("Wikipedia")) == 0x11e60398);
}

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

TEST_CASE("checksum updates retain state across arbitrary chunk boundaries") {
    Array<uint8_t> data(100000);
    prMemFill(data.data(), 0xff, data.count());
    const PrByteView input{data.data(), data.count()};
    const uint32_t crc = prCrc32Update(0, input);
    const uint32_t adler = prAdler32Update(1, input);
    const uint32_t splits[] = {0, 1, 5551, 5552, 5553, data.count()};
    for (uint32_t split : splits) {
        const PrByteView first{data.data(), split};
        const PrByteView second{data.data() + split, data.count() - split};
        CHECK(prCrc32Update(prCrc32Update(0, first), second) == crc);
        CHECK(prAdler32Update(prAdler32Update(1, first), second) == adler);
    }
    uint32_t slowLow = 1, slowHigh = 0;
    for (uint8_t value : data) {
        slowLow = (slowLow + value) % 65521;
        slowHigh = (slowHigh + slowLow) % 65521;
    }
    CHECK(adler == ((slowHigh << 16) | slowLow));
    CHECK(prCrc32Update(crc, {}) == crc);
    CHECK(prAdler32Update(adler, {}) == adler);
}

TEST_CASE("adler-32 weighs each byte by its distance from the end") {
    Array<uint8_t> data(20000);
    uint32_t state = 1;
    for (uint8_t& value : data) {
        state = state * 1103515245u + 12345u;
        value = uint8_t(state >> 16);
    }
    const uint32_t starts[] = {0, 1, 3, 7};
    for (uint32_t start : starts) {
        uint32_t slowLow = 1, slowHigh = 0;
        for (uint32_t i = start; i < data.count(); ++i) {
            slowLow = (slowLow + data[i]) % 65521;
            slowHigh = (slowHigh + slowLow) % 65521;
        }
        CHECK(prAdler32Update(1, {data.data() + start, data.count() - start}) == ((slowHigh << 16) | slowLow));
    }
}
