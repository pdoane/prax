#include "pr/array.h"
#include "pr/checksum.h"
#include "pr/min_max.h"
#include "pr/string.h"
#include "pr/string_ops.h"

#include "compress/backends/builtin/src/inflate.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

using namespace pr;

constexpr uint8_t kFixed[] = {0xcb, 0x48, 0xcd, 0xc9, 0xc9, 0x57, 0xc8, 0x40, 0x90, 0x8a, 0x5c, 0x0};
constexpr uint8_t kDynamic[] = {
    0xed, 0xca, 0xc9, 0x15, 0x82, 0x30, 0x14, 0x0,  0xc0, 0xbb, 0x55, 0xfc, 0xa,  0xac, 0x26, 0xd,
    0xa0, 0x6,  0x54, 0xd0, 0x48, 0x0,  0x17, 0xaa, 0x87, 0x3a, 0x7c, 0x73, 0x9e, 0x49, 0xd7, 0x1c,
    0xe3, 0x72, 0x3b, 0xf7, 0x71, 0xaa, 0xe5, 0xf3, 0x8c, 0xb6, 0x7c, 0xe3, 0xbe, 0x3c, 0x5e, 0x53,
    0x94, 0x77, 0xae, 0x31, 0xef, 0x3c, 0x34, 0xeb, 0x2f, 0x2e, 0xa5, 0x3b, 0x1e, 0x92, 0x2c, 0xcb,
    0xb2, 0x2c, 0xcb, 0xb2, 0x2c, 0xcb, 0xb2, 0x2c, 0xcb, 0xb2, 0x2c, 0xcb, 0xb2, 0x2c, 0xcb, 0xb2,
    0x2c, 0xcb, 0xb2, 0x2c, 0xcb, 0xb2, 0x2c, 0xcb, 0xb2, 0x2c, 0xcb, 0xb2, 0x2c, 0xcb, 0xb2, 0x2c,
    0xcb, 0xb2, 0x2c, 0xcb, 0xb2, 0x2c, 0xcb, 0xb2, 0x2c, 0xcb, 0xb2, 0x2c, 0xcb, 0xb2, 0x2c, 0xcb,
    0xb2, 0x2c, 0xcb, 0xb2, 0x2c, 0xcb, 0xb2, 0x2c, 0xcb, 0xb2, 0x2c, 0xcb, 0xb2, 0x2c, 0xcb, 0xb2,
    0x2c, 0xcb, 0xb2, 0x2c, 0xcb, 0xb2, 0x2c, 0xcb, 0xb2, 0x2c, 0xcb, 0xb2, 0x2c, 0xcb, 0xb2, 0x2c,
    0xcb, 0xb2, 0x2c, 0xcb, 0xb2, 0x2c, 0xcb, 0xb2, 0x2c, 0xcb, 0xb2, 0x2c, 0xcb, 0xb2, 0x2c, 0xcb,
    0xb2, 0x2c, 0xcb, 0xb2, 0x2c, 0xcb, 0xb2, 0x2c, 0xcb, 0xb2, 0x2c, 0xcb, 0xb2, 0x2c, 0xcb, 0xb2,
    0x2c, 0xcb, 0xb2, 0x2c, 0xcb, 0xb2, 0x2c, 0xcb, 0xb2, 0x2c, 0xcb, 0xb2, 0x2c, 0xcb, 0xb2, 0x2c,
    0xcb, 0xb2, 0x2c, 0xcb, 0xb2, 0x2c, 0xcb, 0xb2, 0x2c, 0xcb, 0xb2, 0x2c, 0xcb, 0xb2, 0x2c, 0xcb,
    0xb2, 0x2c, 0xcb, 0xb2, 0x2c, 0xcb, 0xb2, 0x2c, 0xcb, 0xb2, 0x2c, 0xcb, 0xb2, 0x2c, 0xcb, 0xb2,
    0x2c, 0xcb, 0xb2, 0x2c, 0xcb, 0xb2, 0x2c, 0xcb, 0xb2, 0x2c, 0xcb, 0xb2, 0x2c, 0xcb, 0xb2, 0x2c,
    0xcb, 0xb2, 0x2c, 0xcb, 0xb2, 0x2c, 0xcb, 0xb2, 0x2c, 0xcb, 0xb2, 0x2c, 0xcb, 0xb2, 0x2c, 0xcb,
    0xb2, 0x2c, 0xcb, 0xb2, 0x2c, 0xcb, 0xb2, 0x2c, 0xcb, 0xb2, 0x2c, 0xcb, 0xb2, 0x2c, 0xcb, 0xb2,
    0x2c, 0xcb, 0xb2, 0x2c, 0xcb, 0xb2, 0x2c, 0xcb, 0xb2, 0x2c, 0xcb, 0xb2, 0x2c, 0xcb, 0xb2, 0x2c,
    0xcb, 0xb2, 0x2c, 0xcb, 0xb2, 0x2c, 0xcb, 0xb2, 0xfc, 0x2f, 0x79, 0x3,
};

constexpr uint8_t kZlib[] = {
    0x78, 0x9c, 0xcb, 0x48, 0xcd, 0xc9, 0xc9, 0x57, 0xc8,
    0x40, 0x90, 0x8a, 0x5c, 0x00, 0x47, 0x74, 0x06, 0xa8,
};
constexpr uint8_t kGzip[] = {
    0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0xff,
    0xcb, 0x48, 0xcd, 0xc9, 0xc9, 0x57, 0xc8, 0x40, 0x90, 0x8a,
    0x5c, 0x00, 0xa2, 0x98, 0x73, 0xf5, 0x13, 0x00, 0x00, 0x00,
};
constexpr uint8_t kWithDictionary[] = {
    0x78, 0xbb, 0x24, 0x29, 0x33, 0x45, 0x1b, 0x0d, 0xab, 0xd1, 0xb0,
    0x1a, 0x0d, 0xab, 0x81, 0x0d, 0x2b, 0x00, 0x29, 0x4c, 0x95, 0xa3,
};

String decode(PrByteView input, size_t inputChunk, size_t outputChunk, InflateFormat format = InflateFormat_Raw) {
    Inflate* parser = inflateCreate(format);
    String result;
    size_t at = 0;
    Array<uint8_t> output(outputChunk);
    for (;;) {
        const size_t size = min(inputChunk, input.size - at);
        size_t consumed = 0, written = 0;
        const InflateResult status = inflateRead(parser, {input.data + at, size}, at + size == input.size, &consumed,
            output.data(), output.count(), &written);
        CHECK(consumed <= size);
        CHECK(written <= output.count());
        result.append(PrStringView{reinterpret_cast<const char*>(output.data()), written});
        at += consumed;
        if (status == InflateResult_Done || status == InflateResult_Invalid) {
            CHECK(status == InflateResult_Done);
            CHECK(at == input.size);
            break;
        }
        REQUIRE((consumed || written));
    }
    inflateDestroy(parser);
    return result;
}

// Writes a Deflate stream of one final block in the fixed codes (rfc 1951 3.2.6), for tests that choose their own
// matches.
struct FixedEncoder {
    Array<uint8_t> bytes;
    uint64_t bits = 0;
    uint32_t count = 0;

    FixedEncoder() { put(1 | (1 << 1), 3); }

    void put(uint32_t value, uint32_t length) {
        bits |= uint64_t(value) << count;
        count += length;
        for (; count >= 8; count -= 8, bits >>= 8)
            bytes.push(uint8_t(bits));
    }

    // Writes a Huffman code. The stream sends the first bit of a code first.
    void code(uint32_t value, uint32_t length) {
        uint32_t reversed = 0;
        for (uint32_t bit = 0; bit < length; ++bit)
            reversed |= ((value >> bit) & 1) << (length - 1 - bit);
        put(reversed, length);
    }

    void symbol(uint32_t value) {
        if (value < 144)
            code(0x30 + value, 8);
        else if (value < 256)
            code(0x190 + value - 144, 9);
        else if (value < 280)
            code(value - 256, 7);
        else
            code(0xc0 + value - 280, 8);
    }

    void match(uint32_t length, uint32_t distance) {
        static const uint16_t kLengthBase[] = {
            3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
            31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258,
        };
        static const uint16_t kDistanceBase[] = {
            1,    2,    3,    4,    5,    7,     9,     13,    17,   25,
            33,   49,   65,   97,   129,  193,   257,   385,   513,  769,
            1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577,
        };
        uint32_t l = 28;
        while (length != 258 && (l == 28 || kLengthBase[l] > length))
            --l;
        symbol(257 + l);
        uint32_t extra = l < 8 || l == 28 ? 0 : (l - 4) / 4;
        put(length - kLengthBase[l], extra);
        uint32_t d = 29;
        while (kDistanceBase[d] > distance)
            --d;
        code(d, 5);
        put(distance - kDistanceBase[d], d < 4 ? 0 : (d - 2) / 2);
    }

    Array<uint8_t> finish() {
        symbol(256);
        if (count != 0)
            put(0, 8 - count);
        return bytes;
    }
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("Deflate copies matches of every distance class across calls, from the output and the history") {
    // Random literals and matches. Many matches reach back beyond the output of one call into the history.
    FixedEncoder encoder;
    String expected;
    uint32_t state = 12345;
    auto next = [&](uint32_t bound) {
        state = state * 1103515245u + 12345u;
        return (state >> 8) % bound;
    };
    while (expected.count() < 200000) {
        if (expected.count() < 8 || next(4) == 0) {
            const char literal = char(next(7) + 'a');
            encoder.symbol(uint8_t(literal));
            expected.append(literal);
            continue;
        }
        const uint32_t limit = expected.count() < 32768 ? expected.count() : 32768;
        const uint32_t distance = next(3) == 0 ? next(limit < 8 ? limit : 8) + 1 : next(limit) + 1;
        const uint32_t length = next(256) + 3;
        encoder.match(length, distance);
        for (uint32_t i = 0; i < length; ++i)
            expected.append(expected.view().data[expected.count() - distance]);
    }
    const Array<uint8_t> input = encoder.finish();
    const size_t inputChunks[] = {1, 13, 4096, input.count()};
    const size_t outputChunks[] = {1, 266, 300, 70000};
    for (size_t inputChunk : inputChunks) {
        for (size_t outputChunk : outputChunks)
            CHECK(decode(input, inputChunk, outputChunk) == expected);
    }
}

TEST_CASE("Deflate resumes fixed and dynamic codes across input and output boundaries") {
    String expected;
    for (int i = 0; i < 1800; ++i)
        expected.append("The quick brown fox jumps over the lazy dog.\n");
    const size_t inputChunks[] = {1, 2, 7, 4096};
    const size_t outputChunks[] = {1, 3, 257, 8192};
    for (size_t inputChunk : inputChunks) {
        for (size_t outputChunk : outputChunks) {
            CHECK(decode({kFixed, sizeof(kFixed)}, inputChunk, outputChunk) == "hello hello hello!\n");
            CHECK(decode({kDynamic, sizeof(kDynamic)}, inputChunk, outputChunk) == expected);
        }
    }
}

TEST_CASE("Deflate stored blocks preserve history and byte alignment") {
    const uint8_t stored[] = {0, 3, 0, 0xfc, 0xff, 'a', 'b', 'c', 1, 2, 0, 0xfd, 0xff, 'd', 'e'};
    CHECK(decode({stored, sizeof(stored)}, 1, 1) == "abcde");
}

TEST_CASE("Deflate terminal state, trailing bytes, zero output and reset are explicit") {
    Inflate* parser = inflateCreate(InflateFormat_Raw);
    uint8_t output[32];
    size_t consumed, written;
    CHECK(inflateRead(parser, {kFixed, sizeof(kFixed)}, true, &consumed, nullptr, 0, &written) == InflateResult_Output);
    CHECK(written == 0);
    const size_t first = consumed;
    CHECK(inflateRead(parser, {kFixed + first, sizeof(kFixed) - first}, true, &consumed, output,
        sizeof(output), &written) == InflateResult_Done);
    CHECK(PrStringView{reinterpret_cast<char*>(output), written} == "hello hello hello!\n");
    CHECK(consumed + first == sizeof(kFixed));
    CHECK(inflateRead(parser, {kFixed, sizeof(kFixed)}, true, &consumed, output, sizeof(output), &written) ==
        InflateResult_Done);
    CHECK(consumed == 0);
    CHECK(written == 0);
    inflateReset(parser, InflateFormat_Raw);
    const uint8_t emptyWithTrailing[] = {3, 0, 0xaa, 0xbb};
    CHECK(inflateRead(parser, {emptyWithTrailing, sizeof(emptyWithTrailing)}, true, &consumed, nullptr, 0, &written) ==
        InflateResult_Done);
    CHECK(consumed == 2);
    CHECK(written == 0);
    inflateDestroy(parser);
    inflateDestroy(nullptr);
}

TEST_CASE("Deflate reports truncated and malformed input without losing terminal state") {
    Inflate* parser = inflateCreate(InflateFormat_Raw);
    uint8_t output[64];
    size_t consumed, written;
    for (size_t size = 0; size < sizeof(kFixed); ++size) {
        inflateReset(parser, InflateFormat_Raw);
        CHECK(inflateRead(parser, {kFixed, size}, true, &consumed, output, sizeof(output), &written) ==
            InflateResult_Truncated);
        CHECK(consumed <= size);
        CHECK(inflateRead(parser, {kFixed, sizeof(kFixed)}, true, &consumed, output, sizeof(output), &written) ==
            InflateResult_Invalid);
        CHECK(consumed == 0);
    }
    const uint8_t badType[] = {7};
    const uint8_t badStored[] = {1, 1, 0, 0, 0, 'a'};
    const uint8_t badDynamic[] = {5, 0, 0, 0};
    const uint8_t badFixed[] = {3, 2, 0};
    const PrByteView bad[] = {badType, badStored, badDynamic, badFixed};
    for (PrByteView input : bad) {
        inflateReset(parser, InflateFormat_Raw);
        CHECK(inflateRead(parser, input, true, &consumed, output, sizeof(output), &written) == InflateResult_Invalid);
    }
    inflateDestroy(parser);
}

TEST_CASE("Deflate back references span stored blocks at the full window distance") {
    Array<uint8_t> input({0, 0, 0x80, 0xff, 0x7f});
    String expected;
    for (size_t i = 0; i < 32768; ++i) {
        input.push(uint8_t(i % 251));
        expected.append(char(i % 251));
    }
    // Final fixed block: length 3, distance 32768, then end-of-block.
    uint64_t packed = uint64_t(3) | (uint64_t(64) << 3) | (uint64_t(23) << 10) | (uint64_t(8191) << 15);
    for (unsigned i = 0; i < 5; ++i) {
        input.push(uint8_t(packed));
        packed >>= 8;
    }
    expected.append(prStringSlice(expected, 0, 3));
    CHECK(decode(input, 1, 3) == expected);

    // The same distance exceeds the 256-byte window advertised by this zlib header.
    const uint8_t header[] = {0x08, 0x1d};
    input.insert(0, Span(header));
    Inflate* parser = inflateCreate(InflateFormat_Zlib);
    Array<uint8_t> output(expected.count());
    size_t consumed, written;
    CHECK(inflateRead(parser, input, true, &consumed, output.data(), output.count(), &written) ==
        InflateResult_Invalid);
    CHECK(written == 32768);
    CHECK(consumed < input.count());
    inflateDestroy(parser);
}

TEST_CASE("Deflate permits a literal-only dynamic block without distance codes") {
    const uint8_t input[] = {
        0x5, 0xc0, 0x1, 0x4, 0x0, 0x0, 0x0, 0x0,  0x10, 0x0, 0x0,
        0x0, 0x0,  0x0, 0x0, 0x0, 0x0, 0x1, 0x0,  0x0,  0x0, 0x0,
        0x0, 0x0,  0x0, 0x0, 0x0, 0x0, 0x0, 0x0,  0x0,  0x0, 0x0,
        0x0, 0x0,  0x0, 0x0, 0x0, 0x0, 0x0, 0x80, 0x4,
    };
    CHECK(decode({input, sizeof(input)}, 1, 1) == "A");
}

TEST_CASE("Zlib and gzip resume headers, data and checksums across span boundaries") {
    const size_t inputChunks[] = {1, 2, 7, 4096};
    const size_t outputChunks[] = {1, 3, 32};
    for (size_t inputChunk : inputChunks) {
        for (size_t outputChunk : outputChunks) {
            CHECK(decode({kZlib, sizeof(kZlib)}, inputChunk, outputChunk, InflateFormat_Zlib) == "hello hello hello!\n");
            CHECK(decode({kGzip, sizeof(kGzip)}, inputChunk, outputChunk, InflateFormat_Gzip) == "hello hello hello!\n");
        }
    }
}

TEST_CASE("Gzip validates optional headers and consecutive members without retaining field text") {
    Array<uint8_t> input;
    input.append(kGzip, 10);
    input[3] = 0x1f;
    const uint8_t fields[] = {3, 0, 'a', 0, 'b', 'f', 'i', 'l', 'e', 0};
    input.append(Span(fields));
    for (uint32_t i = 0; i < 70000; ++i)
        input.push('c');
    input.push(0);
    const uint32_t crc = prCrc32Update(0, input);
    input.push(uint8_t(crc));
    input.push(uint8_t(crc >> 8));
    input.append(Span(kGzip).from(10));
    input.append(Span(kGzip));
    CHECK(decode(input, 1, 3, InflateFormat_Gzip) == "hello hello hello!\nhello hello hello!\n");

    Inflate* parser = inflateCreate(InflateFormat_Gzip);
    uint8_t output[64];
    size_t consumed, written;
    input[10] ^= 1;
    CHECK(inflateRead(parser, input, true, &consumed, output, sizeof(output), &written) == InflateResult_Invalid);
    CHECK(written == 0);
    inflateDestroy(parser);
}

TEST_CASE("Framed Deflate rejects truncated fields and corrupt checksums") {
    Inflate* parser = inflateCreate(InflateFormat_Raw);
    uint8_t output[64];
    size_t consumed, written;
    const InflateFormat formats[] = {InflateFormat_Zlib, InflateFormat_Gzip};
    for (InflateFormat format : formats) {
        const PrByteView bytes = format == InflateFormat_Zlib ? PrByteView(kZlib) : PrByteView(kGzip);
        for (size_t size = 0; size < bytes.size; ++size) {
            inflateReset(parser, format);
            CHECK(inflateRead(parser, {bytes.data, size}, true, &consumed, output, sizeof(output), &written) ==
                InflateResult_Truncated);
        }
        const uint32_t indexes[] = {0, uint32_t(bytes.size - 1), uint32_t(bytes.size - 4)};
        for (uint32_t index : indexes) {
            Array<uint8_t> bad;
            bad.append(bytes.data, uint32_t(bytes.size));
            bad[index] ^= 1;
            inflateReset(parser, format);
            CHECK(inflateRead(parser, bad, true, &consumed, output, sizeof(output), &written) == InflateResult_Invalid);
            CHECK(inflateRead(parser, bytes, true, &consumed, output, sizeof(output), &written) ==
                InflateResult_Invalid);
            CHECK(consumed == 0);
            CHECK(written == 0);
        }
    }
    inflateDestroy(parser);
}

TEST_CASE("Gzip awaits a final member boundary while zlib leaves trailing bytes unread") {
    Inflate* parser = inflateCreate(InflateFormat_Gzip);
    uint8_t output[32];
    size_t consumed, written;
    CHECK(inflateRead(parser, {kGzip, sizeof(kGzip)}, false, &consumed, output, sizeof(output), &written) ==
        InflateResult_Input);
    CHECK(consumed == sizeof(kGzip));
    CHECK(written == 19);
    CHECK(inflateRead(parser, {}, true, &consumed, nullptr, 0, &written) == InflateResult_Done);
    Array<uint8_t> input(kZlib);
    input.push(0xaa);
    inflateReset(parser, InflateFormat_Zlib);
    CHECK(inflateRead(parser, input, true, &consumed, output, sizeof(output), &written) == InflateResult_Done);
    CHECK(consumed == sizeof(kZlib));
    inflateReset(parser, InflateFormat_Gzip);
    input = Array<uint8_t>(kGzip);
    input.push(0xaa);
    CHECK(inflateRead(parser, input, true, &consumed, output, sizeof(output), &written) == InflateResult_Truncated);
    inflateDestroy(parser);
}

TEST_CASE("Zlib requests and validates preset dictionaries and retains their history suffix") {
    Array<uint8_t> dictionary;
    for (size_t i = 0; i < 40000; ++i)
        dictionary.push(uint8_t(i % 251));
    String expected;
    const PrStringView suffix{reinterpret_cast<const char*>(dictionary.data() + dictionary.count() - 300), 300};
    for (size_t i = 0; i < 3; ++i)
        expected.append(suffix);
    Inflate* parser = inflateCreate(InflateFormat_Zlib);
    size_t at = 0;
    String result;
    uint8_t output[7];
    for (;;) {
        size_t consumed, written;
        const size_t count = at < sizeof(kWithDictionary) ? 1 : 0;
        const InflateResult status =
            inflateRead(parser, {kWithDictionary + at, count}, at + count == sizeof(kWithDictionary),
                &consumed, output, sizeof(output), &written);
        at += consumed;
        result.append(PrStringView{reinterpret_cast<const char*>(output), written});
        if (status == InflateResult_Dictionary) {
            CHECK(at == 6);
            CHECK(inflateDictionaryChecksum(parser) == prAdler32Update(1, dictionary));
            CHECK_FALSE(inflateSetDictionary(parser, {}));
            CHECK(inflateRead(parser, {}, true, &consumed, nullptr, 0, &written) == InflateResult_Dictionary);
            REQUIRE(inflateSetDictionary(parser, dictionary));
        } else if (status == InflateResult_Done || status == InflateResult_Invalid) {
            CHECK(status == InflateResult_Done);
            CHECK(at == sizeof(kWithDictionary));
            break;
        } else {
            REQUIRE((consumed || written));
        }
    }
    CHECK(result == expected);
    inflateDestroy(parser);
}
