#include "pr/checksum.h"

#include "doctest/doctest.h"

#include <string>
#include <string_view>
#include <vector>

namespace pr_impl {

namespace {

PrByteView bytes(std::string_view value) {
    return {reinterpret_cast<const uint8_t*>(value.data()), value.size()};
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("CRC-32 and Adler-32 match published check values") {
    CHECK(crc32Update(0, {}) == 0);
    CHECK(adler32Update(1, {}) == 1);
    CHECK(crc32Update(0, bytes("123456789")) == 0xcbf43926);
    CHECK(adler32Update(1, bytes("123456789")) == 0x091e01de);
    CHECK(adler32Update(1, bytes("Wikipedia")) == 0x11e60398);
}

TEST_CASE("SHA-1 matches the digests of rfc 3174 and FIPS 180") {
    auto hex = [](std::string_view text) {
        uint8_t digest[kSha1Bytes];
        sha1Digest(bytes(text), digest);
        std::string out;
        for (uint8_t byte : digest) {
            out += "0123456789abcdef"[byte >> 4];
            out += "0123456789abcdef"[byte & 15];
        }
        return out;
    };
    CHECK(hex("") == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    CHECK(hex("abc") == "a9993e364706816aba3e25717850c26c9cd0d89d");
    // 56 bytes, whose padding needs a second block.
    CHECK(hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
        "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    CHECK(hex(std::string(1000000, 'a')) == "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
}

TEST_CASE("checksum updates retain state across arbitrary chunk boundaries") {
    std::vector<uint8_t> data(100000, 0xff);
    const PrByteView input{data.data(), data.size()};
    const uint32_t crc = crc32Update(0, input);
    const uint32_t adler = adler32Update(1, input);
    for (size_t split :
            {size_t(0), size_t(1), size_t(5551), size_t(5552), size_t(5553), data.size()}) {
        const PrByteView first{data.data(), split};
        const PrByteView second{data.data() + split, data.size() - split};
        CHECK(crc32Update(crc32Update(0, first), second) == crc);
        CHECK(adler32Update(adler32Update(1, first), second) == adler);
    }
    uint32_t slowLow = 1, slowHigh = 0;
    for (uint8_t value : data) {
        slowLow = (slowLow + value) % 65521;
        slowHigh = (slowHigh + slowLow) % 65521;
    }
    CHECK(adler == ((slowHigh << 16) | slowLow));
    CHECK(crc32Update(crc, {}) == crc);
    CHECK(adler32Update(adler, {}) == adler);
}

TEST_CASE("adler-32 weighs each byte by its distance from the end") {
    std::vector<uint8_t> data(20000);
    uint32_t state = 1;
    for (uint8_t& value : data) {
        state = state * 1103515245u + 12345u;
        value = uint8_t(state >> 16);
    }
    for (size_t start : {size_t(0), size_t(1), size_t(3), size_t(7)}) {
        uint32_t slowLow = 1, slowHigh = 0;
        for (size_t i = start; i < data.size(); ++i) {
            slowLow = (slowLow + data[i]) % 65521;
            slowHigh = (slowHigh + slowLow) % 65521;
        }
        CHECK(adler32Update(1, {data.data() + start, data.size() - start}) == ((slowHigh << 16) | slowLow));
    }
}
