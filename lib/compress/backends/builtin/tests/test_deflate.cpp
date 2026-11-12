#include "pr/array.h"

#include "compress/backends/builtin/src/deflate.h"
#include "compress/backends/builtin/src/inflate.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

using namespace pr;

const InflateFormat kFormats[] = {InflateFormat_Raw, InflateFormat_Zlib, InflateFormat_Gzip};
const PrCompressLevel kLevels[] = {PrCompressLevel_Fast, PrCompressLevel_Default, PrCompressLevel_Small};

// Returns `size` bytes of xorshift32 output from `seed`. No match can shorten them.
Array<uint8_t> noise(uint32_t size, uint32_t seed) {
    Array<uint8_t> out(size);
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
Array<uint8_t> text(uint32_t size) {
    static const char* const kWords[] = {
        "the ",   "quick ", "brown ", "fox ",    "jumps ", "over ",
        "lazy ",  "dog ",   "and ",   "then ",   "runs ",  "away, ",
        "while ", "a ",     "cat ",   "sleeps. ",
    };
    Array<uint8_t> out;
    uint32_t x = 7;
    while (out.count() < size) {
        x = x * 1103515245u + 12345u;
        PrStringView word = kWords[(x >> 16) % 16];
        out.append(reinterpret_cast<const uint8_t*>(word.data), uint32_t(word.size));
    }
    out.resize(size);
    return out;
}

Array<uint8_t> encode(InflateFormat format, PrCompressLevel level, const Array<uint8_t>& input) {
    Array<uint8_t> out(deflateBound(format, input.count()));
    const size_t size = deflateEncode(format, level, input, out.data());
    REQUIRE(size <= out.count());
    out.resize(uint32_t(size));
    return out;
}

Array<uint8_t> decode(InflateFormat format, const Array<uint8_t>& stream, uint32_t capacity) {
    Inflate* parser = inflateCreate(format);
    Array<uint8_t> out(capacity + 1);
    size_t consumed = 0;
    size_t written = 0;
    const InflateResult result = inflateRead(parser, stream, true, &consumed, out.data(), out.count(), &written);
    inflateDestroy(parser);
    CHECK(result == InflateResult_Done);
    CHECK(consumed == stream.count());
    out.resize(uint32_t(written));
    return out;
}

void checkRoundTrip(const Array<uint8_t>& input) {
    for (InflateFormat format : kFormats) {
        for (PrCompressLevel level : kLevels) {
            CAPTURE(int(format));
            CAPTURE(int(level));
            const Array<uint8_t> stream = encode(format, level, input);
            CHECK(decode(format, stream, input.count()) == input);
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
        checkRoundTrip(Array<uint8_t>({'a'}));
        checkRoundTrip(Array<uint8_t>({'a', 'b', 'a', 'b', 'a', 'b', 'a'}));
    }
    SUBCASE("text, over many blocks") {
        checkRoundTrip(text(300000));
    }
    SUBCASE("noise, stored") {
        checkRoundTrip(noise(200000, 1));
    }
    SUBCASE("runs of one byte, with matches of the longest length and the shortest distance") {
        Array<uint8_t> runs(100000);
        for (uint8_t& byte : runs)
            byte = 'z';
        checkRoundTrip(runs);
    }
    SUBCASE("noise repeated at the far end of the window") {
        Array<uint8_t> input = noise(32768, 3);
        const Array<uint8_t> copy = input;
        input.append(copy.span());
        input.append(copy.span().slice(0, 1000));
        checkRoundTrip(input);
    }
    SUBCASE("noise and text, mixed") {
        Array<uint8_t> input;
        for (uint32_t i = 0; i < 20; ++i) {
            const Array<uint8_t> a = noise(3000 + i * 97, i + 10);
            const Array<uint8_t> b = text(5000 + i * 31);
            input.append(a.span());
            input.append(b.span());
        }
        checkRoundTrip(input);
    }
}

TEST_CASE("Deflate output stays within its bound and shrinks what repeats") {
    const Array<uint8_t> random = noise(1000000, 9);
    for (InflateFormat format : kFormats)
        CHECK(encode(format, PrCompressLevel_Small, random).count() <= deflateBound(format, random.count()));
    // Noise grows by no more than the headers of its stored blocks.
    CHECK(encode(InflateFormat_Raw, PrCompressLevel_Default, random).count() < random.count() + random.count() / 1000);

    const Array<uint8_t> words = text(200000);
    const size_t fast = encode(InflateFormat_Zlib, PrCompressLevel_Fast, words).count();
    const size_t standard = encode(InflateFormat_Zlib, PrCompressLevel_Default, words).count();
    const size_t small = encode(InflateFormat_Zlib, PrCompressLevel_Small, words).count();
    CHECK(standard < words.count() / 3);
    CHECK(small <= standard);
    CHECK(standard <= fast);
}

TEST_CASE("Deflate frames zlib and gzip streams as their formats define") {
    const Array<uint8_t> input = text(1000);
    const Array<uint8_t> zlib = encode(InflateFormat_Zlib, PrCompressLevel_Default, input);
    // rfc 1950 2.2: Deflate with a 32K window, and a header that is a multiple of 31.
    CHECK(zlib[0] == 0x78);
    CHECK((zlib[0] * 256 + zlib[1]) % 31 == 0);
    const Array<uint8_t> gzip = encode(InflateFormat_Gzip, PrCompressLevel_Default, input);
    // rfc 1952 2.3: the magic number and the Deflate method first, and the size modulo 2^32 last.
    CHECK(gzip[0] == 0x1f);
    CHECK(gzip[1] == 0x8b);
    CHECK(gzip[2] == 8);
    const uint32_t end = gzip.count();
    const uint32_t size = uint32_t(gzip[end - 4]) | uint32_t(gzip[end - 3]) << 8 | uint32_t(gzip[end - 2]) << 16 |
        uint32_t(gzip[end - 1]) << 24;
    CHECK(size == input.count());
}
