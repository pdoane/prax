#include "pr/array.h"
#include "pr/lz4.h"
#include "pr/string.h"

#include "tools/harness/test.h"

extern "C" int prTestLz4CApi(void);

namespace pr_impl {

namespace {

// How a read ended, in one value: its progress if it succeeds, or how it failed.
enum {
    Outcome_Input,
    Outcome_Output,
    Outcome_Done,
    Outcome_Truncated,
    Outcome_Dictionary,
    Outcome_Invalid,
};
typedef uint8_t Outcome;

Outcome outcomeOf(PrStatus status, uint8_t progress) {
    if (status == PrStatus_Truncated)
        return Outcome_Truncated;
    if (status == PrStatus_NotSupported)
        return Outcome_Dictionary;
    if (status != PrStatus_Ok)
        return Outcome_Invalid;
    // The progresses are the first outcomes, in the same order.
    return Outcome(progress);
}

// Decodes a whole block, as prLz4DecodeBlock does, and returns how it ended.
Outcome decodeOutcome(PrByteView block, uint8_t* PR_NULLABLE output, size_t capacity, size_t* written) {
    PrLz4Read read{};
    const PrStatus status = prLz4DecodeBlock(block, output, capacity, &read);
    *written = read.written;
    return outcomeOf(status, read.progress);
}

// Reads with `decoder`, as prLz4DecoderRead does, and returns how the read ended.
Outcome readOutcome(PrLz4Decoder* decoder, PrByteView input, bool final, size_t* consumed,
        uint8_t* PR_NULLABLE output, size_t capacity, size_t* written) {
    PrLz4Read read{};
    const PrStatus status = prLz4DecoderRead(decoder, input, final, output, capacity, &read);
    *consumed = read.consumed;
    *written = read.written;
    return outcomeOf(status, read.progress);
}

// "the quick brown fox jumps over the lazy dog " twenty times, as the block of the frame that lz4 -9 makes of it.
constexpr uint8_t kText[] = {
    0xf0, 0x10, 0x74, 0x68, 0x65, 0x20, 0x71, 0x75, 0x69, 0x63, 0x6b, 0x20, 0x62, 0x72, 0x6f, 0x77, 0x6e, 0x20, 0x66,
    0x6f, 0x78, 0x20, 0x6a, 0x75, 0x6d, 0x70, 0x73, 0x20, 0x6f, 0x76, 0x65, 0x72, 0x20, 0x1f, 0x00, 0x9f, 0x6c, 0x61,
    0x7a, 0x79, 0x20, 0x64, 0x6f, 0x67, 0x20, 0x2c, 0x00, 0xff, 0xff, 0xff, 0x2f, 0x50, 0x20, 0x64, 0x6f, 0x67, 0x20,
};

String text() {
    String all;
    for (int i = 0; i < 20; ++i)
        all.append("the quick brown fox jumps over the lazy dog ");
    return all;
}

// A decoded block and how its decoding ended.
struct Decoded {
    Outcome result;
    String bytes;
};

// Decodes `block` at once into room for `capacity` bytes.
Decoded decodeBlock(PrByteView block, size_t capacity = 4096) {
    Array<uint8_t> output;
    output.resize(uint32_t(capacity));
    size_t written = 0;
    Outcome result = decodeOutcome(block, output.data(), capacity, &written);
    REQUIRE(written <= capacity);
    return {result, String(PrStringView{reinterpret_cast<const char*>(output.data()), written})};
}

// Decodes `block` through a decoder, `piece` bytes of input at a time into room for `room` bytes.
Decoded decodePieces(PrByteView block, size_t piece, size_t room) {
    PrLz4Decoder* decoder = prLz4DecoderCreate();
    Array<uint8_t> output;
    output.resize(uint32_t(room));
    Decoded decoded{Outcome_Input, String()};
    size_t at = 0;
    for (int guard = 0; guard < 100000; ++guard) {
        size_t size = block.size - at < piece ? block.size - at : piece;
        size_t consumed = 0;
        size_t written = 0;
        decoded.result = readOutcome(decoder, PrByteView{block.data + at, size}, at + size == block.size, &consumed,
            output.data(), room, &written);
        at += consumed;
        decoded.bytes.append(PrStringView{reinterpret_cast<const char*>(output.data()), written});
        if (decoded.result != Outcome_Input && decoded.result != Outcome_Output)
            break;
    }
    prLz4DecoderDestroy(decoder);
    return decoded;
}

// Appends a sequence's token and the continuations of its lengths: `literals` literals to follow, and a match of
// `match` bytes, or no match if `match` is zero.
void appendToken(Array<uint8_t>& block, size_t literals, size_t match) {
    size_t length = match == 0 ? 0 : match - 4;
    block.push(uint8_t((literals < 15 ? literals : 15) << 4 | (length < 15 ? length : 15)));
    if (literals >= 15) {
        for (literals -= 15; literals >= 255; literals -= 255)
            block.push(255);
        block.push(uint8_t(literals));
    }
}

// Appends the continuation of a match's length, after its distance.
void appendMatchLength(Array<uint8_t>& block, size_t match) {
    if (match - 4 < 15)
        return;
    size_t left = match - 4 - 15;
    for (; left >= 255; left -= 255)
        block.push(255);
    block.push(uint8_t(left));
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("LZ4 decodes an encoder's block whole and in pieces") {
    const String expected = text();
    Decoded whole = decodeBlock(kText);
    CHECK(whole.result == Outcome_Done);
    CHECK(whole.bytes == expected);
    SUBCASE("into exactly the room that it needs") {
        Decoded exact = decodeBlock(kText, expected.count());
        CHECK(exact.result == Outcome_Done);
        CHECK(exact.bytes == expected);
    }
    SUBCASE("through a decoder, in pieces of input and of output") {
        const size_t pieces[] = {1, 7, sizeof(kText)};
        const size_t rooms[] = {1, 100, 4096};
        for (size_t piece : pieces) {
            for (size_t room : rooms) {
                CAPTURE(piece);
                CAPTURE(room);
                Decoded decoded = decodePieces(kText, piece, room);
                CHECK(decoded.result == Outcome_Done);
                CHECK(decoded.bytes == expected);
            }
        }
    }
}

TEST_CASE("LZ4 decodes lengths that continue past the token") {
    // 300 literals, then a match of 1000 bytes from one back, which repeats the last literal, then two literals.
    Array<uint8_t> block;
    String expected;
    appendToken(block, 300, 1000);
    for (int i = 0; i < 300; ++i) {
        block.push(uint8_t('a' + i % 26));
        expected.append(char('a' + i % 26));
    }
    block.push(1);
    block.push(0);
    appendMatchLength(block, 1000);
    for (int i = 0; i < 1000; ++i)
        expected.append(char('a' + 299 % 26));
    appendToken(block, 2, 0);
    block.push('!');
    block.push('?');
    expected.append("!?");
    Decoded decoded = decodeBlock(block);
    CHECK(decoded.result == Outcome_Done);
    CHECK(decoded.bytes == expected);
    CHECK(decodePieces(block, 3, 64).bytes == expected);
}

TEST_CASE("LZ4 copies a match that overlaps its own output as a pattern") {
    // "abc", then nine bytes from three back, then "d".
    const uint8_t block[] = {0x35, 'a', 'b', 'c', 3, 0, 0x10, 'd'};
    Decoded decoded = decodeBlock(block);
    CHECK(decoded.result == Outcome_Done);
    CHECK(decoded.bytes == "abcabcabcabcd");
}

TEST_CASE("LZ4 decodes a block of literals alone, and an empty one") {
    const uint8_t literals[] = {0x30, 'a', 'b', 'c'};
    CHECK(decodeBlock(literals).bytes == "abc");
    const uint8_t empty[] = {0x00};
    Decoded decoded = decodeBlock(empty);
    CHECK(decoded.result == Outcome_Done);
    CHECK(decoded.bytes == "");
}

TEST_CASE("LZ4 reports a block that does not fit") {
    const String expected = text();
    Decoded decoded = decodeBlock(kText, expected.count() - 1);
    CHECK(decoded.result == Outcome_Output);
    // Room that ends inside the literals, and inside a match.
    CHECK(decodeBlock(kText, 10).result == Outcome_Output);
    CHECK(decodeBlock(kText, 40).result == Outcome_Output);
    CHECK(decodeBlock(kText, 0).result == Outcome_Output);
}

TEST_CASE("LZ4 reports a block that ends inside a sequence") {
    SUBCASE("no input") {
        CHECK(decodeBlock(PrByteView{}).result == Outcome_Truncated);
    }
    SUBCASE("every proper prefix that does not end after a sequence's literals") {
        // kText's sequences end their literals at 33 and 45 bytes, where a prefix is a block of its own.
        for (size_t size = 0; size < sizeof(kText); ++size) {
            CAPTURE(size);
            Outcome result = decodeBlock(PrByteView{kText, size}).result;
            CHECK(result == (size == 33 || size == 45 ? Outcome_Done : Outcome_Truncated));
        }
    }
    SUBCASE("a block that ends with a match") {
        const uint8_t block[] = {0x30, 'a', 'b', 'c', 3, 0};
        CHECK(decodeBlock(block).result == Outcome_Truncated);
    }
    SUBCASE("a length whose continuation never ends") {
        const uint8_t block[] = {0xf0, 255, 255, 255};
        CHECK(decodeBlock(block).result == Outcome_Truncated);
        CHECK(decodePieces(block, 1, 16).result == Outcome_Truncated);
    }
}

TEST_CASE("LZ4 rejects a match from before the start of the output") {
    SUBCASE("a distance of zero") {
        const uint8_t block[] = {0x30, 'a', 'b', 'c', 0, 0, 0x10, 'd'};
        CHECK(decodeBlock(block).result == Outcome_Invalid);
    }
    SUBCASE("a distance past the first byte") {
        const uint8_t block[] = {0x30, 'a', 'b', 'c', 4, 0, 0x10, 'd'};
        CHECK(decodeBlock(block).result == Outcome_Invalid);
        const uint8_t reach[] = {0x30, 'a', 'b', 'c', 3, 0, 0x10, 'd'};
        CHECK(decodeBlock(reach).result == Outcome_Done);
    }
    SUBCASE("a match with no output before it") {
        const uint8_t block[] = {0x00, 1, 0, 0x10, 'd'};
        CHECK(decodeBlock(block).result == Outcome_Invalid);
    }
    SUBCASE("through a decoder that holds the output") {
        const uint8_t block[] = {0x30, 'a', 'b', 'c', 4, 0, 0x10, 'd'};
        CHECK(decodePieces(block, 2, 1).result == Outcome_Invalid);
    }
}

TEST_CASE("LZ4's public header compiles as C") {
    CHECK(prTestLz4CApi() == 0);
}
