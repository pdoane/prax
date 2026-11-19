#include "pr/array.h"
#include "pr/checksum.h"
#include "pr/mem.h"
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

TEST_CASE("CRC-32 of long inputs matches the bytewise definition at every length and alignment") {
    // Lengths past the lanes of a long input, and around the steps of 8, 16 and 64 bytes, from starts that are not
    // aligned.
    Array<uint8_t> data(70000);
    uint32_t state = 7;
    for (uint8_t& value : data) {
        state = state * 1103515245u + 12345u;
        value = uint8_t(state >> 16);
    }
    auto slow = [](const uint8_t* bytes, size_t size) {
        uint32_t crc = 0xffffffffu;
        for (size_t i = 0; i < size; ++i) {
            crc ^= bytes[i];
            for (uint32_t bit = 0; bit < 8; ++bit)
                crc = (crc & 1) != 0 ? 0xedb88320u ^ (crc >> 1) : crc >> 1;
        }
        return ~crc;
    };
    const uint32_t sizes[] = {
        1, 7, 8, 15, 16, 17, 63, 64, 65, 127, 128, 200, 4095, 4096, 4097, 4100, 6000, 65536,
        69990,
    };
    for (uint32_t start = 0; start < 4; ++start) {
        for (uint32_t size : sizes) {
            CAPTURE(start);
            CAPTURE(size);
            CHECK(prCrc32Update(0, {data.data() + start, size}) == slow(data.data() + start, size));
        }
    }
    // A checksum continued from another, over a long input.
    const uint32_t first = prCrc32Update(0, {data.data(), 5000});
    CHECK(prCrc32Update(first, {data.data() + 5000, 60000}) == slow(data.data(), 65000));
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
