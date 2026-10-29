#include "compress/backends/builtin/src/deflate.h"
#include "compress/backends/builtin/src/inflate.h"

#include "doctest/doctest.h"

#include <string>
#include <vector>

namespace pr_impl {

namespace {

// Returns `size` bytes of xorshift32 output from `seed`. No match can shorten them.
std::vector<uint8_t> noise(size_t size, uint32_t seed) {
    std::vector<uint8_t> out(size);
    uint32_t x = seed;
    for (uint8_t& byte : out) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        byte = uint8_t(x);
    }
    return out;
}

// Returns words of a small vocabulary in an order that repeats only now and then, as text does.
std::vector<uint8_t> text(size_t size) {
    static const char* const kWords[] = {"the ",   "quick ", "brown ", "fox ",    "jumps ", "over ",
                                         "lazy ",  "dog ",   "and ",   "then ",   "runs ",  "away, ",
                                         "while ", "a ",     "cat ",   "sleeps. "};
    std::vector<uint8_t> out;
    uint32_t x = 7;
    while (out.size() < size) {
        x = x * 1103515245u + 12345u;
        const char* word = kWords[(x >> 16) % 16];
        out.insert(out.end(), word, word + std::char_traits<char>::length(word));
    }
    out.resize(size);
    return out;
}

std::vector<uint8_t> encode(InflateFormat format, PrCompressLevel level, const std::vector<uint8_t>& input) {
    std::vector<uint8_t> out(deflateBound(format, input.size()));
    const size_t size = deflateEncode(format, level, {input.data(), input.size()}, out.data());
    REQUIRE(size <= out.size());
    out.resize(size);
    return out;
}

std::vector<uint8_t> decode(InflateFormat format, const std::vector<uint8_t>& stream, size_t capacity) {
    Inflate* parser = inflateCreate(format);
    std::vector<uint8_t> out(capacity + 1);
    size_t consumed = 0;
    size_t written = 0;
    const InflateResult result = inflateRead(parser, {stream.data(), stream.size()}, true, &consumed, out.data(),
                                             out.size(), &written);
    inflateDestroy(parser);
    CHECK(result == InflateResult_Done);
    CHECK(consumed == stream.size());
    out.resize(written);
    return out;
}

void checkRoundTrip(const std::vector<uint8_t>& input) {
    for (InflateFormat format : {InflateFormat_Raw, InflateFormat_Zlib, InflateFormat_Gzip}) {
        for (PrCompressLevel level : {PrCompressLevel_Fast, PrCompressLevel_Default, PrCompressLevel_Small}) {
            CAPTURE(int(format));
            CAPTURE(int(level));
            const std::vector<uint8_t> stream = encode(format, level, input);
            CHECK(decode(format, stream, input.size()) == input);
        }
    }
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("Deflate encodes what Inflate decodes, in each framing and at each level") {
    SUBCASE("nothing") {
        checkRoundTrip({});
    }
    SUBCASE("one byte, and a few") {
        checkRoundTrip({'a'});
        checkRoundTrip({'a', 'b', 'a', 'b', 'a', 'b', 'a'});
    }
    SUBCASE("text, over many blocks") {
        checkRoundTrip(text(300000));
    }
    SUBCASE("noise, stored") {
        checkRoundTrip(noise(200000, 1));
    }
    SUBCASE("runs of one byte, with matches of the longest length and the shortest distance") {
        checkRoundTrip(std::vector<uint8_t>(100000, 'z'));
    }
    SUBCASE("noise repeated at the far end of the window") {
        std::vector<uint8_t> input = noise(32768, 3);
        const std::vector<uint8_t> copy = input;
        input.insert(input.end(), copy.begin(), copy.end());
        input.insert(input.end(), copy.begin(), copy.begin() + 1000);
        checkRoundTrip(input);
    }
    SUBCASE("noise and text, mixed") {
        std::vector<uint8_t> input;
        for (uint32_t i = 0; i < 20; ++i) {
            const std::vector<uint8_t> a = noise(3000 + i * 97, i + 10);
            const std::vector<uint8_t> b = text(5000 + i * 31);
            input.insert(input.end(), a.begin(), a.end());
            input.insert(input.end(), b.begin(), b.end());
        }
        checkRoundTrip(input);
    }
}

TEST_CASE("Deflate output stays within its bound and shrinks what repeats") {
    const std::vector<uint8_t> random = noise(1000000, 9);
    for (InflateFormat format : {InflateFormat_Raw, InflateFormat_Zlib, InflateFormat_Gzip})
        CHECK(encode(format, PrCompressLevel_Small, random).size() <= deflateBound(format, random.size()));
    // Noise grows by no more than the headers of its stored blocks.
    CHECK(encode(InflateFormat_Raw, PrCompressLevel_Default, random).size() < random.size() + random.size() / 1000);

    const std::vector<uint8_t> words = text(200000);
    const size_t fast = encode(InflateFormat_Zlib, PrCompressLevel_Fast, words).size();
    const size_t standard = encode(InflateFormat_Zlib, PrCompressLevel_Default, words).size();
    const size_t small = encode(InflateFormat_Zlib, PrCompressLevel_Small, words).size();
    CHECK(standard < words.size() / 3);
    CHECK(small <= standard);
    CHECK(standard <= fast);
}

TEST_CASE("Deflate frames zlib and gzip streams as their formats define") {
    const std::vector<uint8_t> input = text(1000);
    const std::vector<uint8_t> zlib = encode(InflateFormat_Zlib, PrCompressLevel_Default, input);
    // rfc 1950 2.2: Deflate with a 32K window, and a header that is a multiple of 31.
    CHECK(zlib[0] == 0x78);
    CHECK((zlib[0] * 256 + zlib[1]) % 31 == 0);
    const std::vector<uint8_t> gzip = encode(InflateFormat_Gzip, PrCompressLevel_Default, input);
    // rfc 1952 2.3: the magic number and the Deflate method first, and the size modulo 2^32 last.
    CHECK(gzip[0] == 0x1f);
    CHECK(gzip[1] == 0x8b);
    CHECK(gzip[2] == 8);
    const size_t end = gzip.size();
    const uint32_t size = uint32_t(gzip[end - 4]) | uint32_t(gzip[end - 3]) << 8 | uint32_t(gzip[end - 2]) << 16
                          | uint32_t(gzip[end - 1]) << 24;
    CHECK(size == input.size());
}
