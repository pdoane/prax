// What Zstandard's decoder and encoder share: the format's limits, its tables of codes, and the reading of its FSE
// tables, its backward bit streams and its Huffman weights (rfc 8878).

#pragma once

#include "pr/base.h"
#include "pr/mem.h"
#include "pr/min_max.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The most that a block holds, and that it produces (rfc 8878 3.1.1.2.3).
constexpr int64_t kZstdBlockMax = 1 << 17;

// The longest Huffman code of the literals (rfc 8878 4.2.1).
constexpr uint32_t kZstdHuffmanMaxBits = 11;

// The codes of literal lengths, match lengths and offsets: the number of each and the largest accuracy log of its FSE
// table (rfc 8878 3.1.1.3.2).
constexpr uint32_t kZstdLiteralLengthCodes = 36;
constexpr uint32_t kZstdMatchLengthCodes = 53;
constexpr uint32_t kZstdOffsetCodes = 32;
constexpr uint32_t kZstdLiteralLengthLog = 9;
constexpr uint32_t kZstdMatchLengthLog = 9;
constexpr uint32_t kZstdOffsetLog = 8;

// The literal lengths and match lengths that each code stands for, and the extra bits that follow it (rfc 8878
// 3.1.1.3.2.1.1).
constexpr uint32_t kZstdLiteralLengthBase[36] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 18, 20, 22, 24, 28, 32, 40, 48, 64, 128, 256, 512, 1024,
    2048, 4096, 8192, 16384, 32768, 65536,
};
constexpr uint8_t kZstdLiteralLengthExtra[36] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 3, 3, 4, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
};
constexpr uint32_t kZstdMatchLengthBase[53] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33,
    34, 35, 37, 39, 41, 43, 47, 51, 59, 67, 83, 99, 131, 259, 515, 1027, 2051, 4099, 8195, 16387, 32771, 65539,
};
constexpr uint8_t kZstdMatchLengthExtra[53] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 3,
    3, 4, 4, 5, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
};

// The distributions that the predefined mode uses (rfc 8878 3.1.1.3.2.2): a count of -1 is a probability below one.
constexpr int16_t kZstdLiteralLengthDefault[36] = {
    4, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 2, 1, 1, 1, 1, 1, -1, -1, -1, -1,
};
constexpr int16_t kZstdMatchLengthDefault[53] = {
    1, 4, 3, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1, -1, -1,
};
constexpr int16_t kZstdOffsetDefault[29] = {
    1, 1, 1, 1, 1, 1, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1,
};

// Spreads the symbols of `counts`, of which there are `symbols`, over the 2^log states of an FSE table into `spread`
// (rfc 8878 4.1.1): a symbol of probability below one at the highest states, and the others stepping through the rest.
// Returns false if they do not fill it exactly.
bool zstdFseSpread(const int16_t* counts, int64_t symbols, uint32_t log, uint8_t* spread);

// A state of an FSE table: the symbol it decodes, and the next state, which is `baseline` plus the next `bits` bits.
struct ZstdFseCell {
    uint8_t symbol;
    uint8_t bits;
    uint16_t baseline;
};

// The next bits of a stream that is read backward, from its last byte to its first, and from each byte's highest bit to
// its lowest: the eight bytes from `at`, or all of a shorter stream, of which the highest `consumed` bits have been
// read. Reading past the stream's start leaves `consumed` above 64, and what it reads is of no use.
struct ZstdBackReader {
    const uint8_t* data;
    int64_t size;
    int64_t at;
    uint64_t bits;
    uint64_t consumed;
};

// What a reload of a ZstdBackReader found.
enum { ZstdReload_More, ZstdReload_Last, ZstdReload_Done, ZstdReload_Overflow };
typedef uint8_t ZstdReload;

// The integer of `count` bytes at `bytes`, little-endian.
inline uint64_t zstdLeBytes(const uint8_t* bytes, int64_t count) {
    uint64_t value = 0;
    for (int64_t k = 0; k < count; ++k)
        value |= uint64_t(bytes[k]) << (8 * k);
    return value;
}

// Starts reading the `size` bytes at `data` backward, after the padding of zeros and the one bit above them that end it
// (rfc 8878 4.2.1.3). Returns false if its last byte is zero.
bool zstdBackReaderStart(const uint8_t* data, int64_t size, ZstdBackReader* r);

// The next `count` bits, at most 56 after a reload, without taking them. Past the stream's start they are zero.
inline uint64_t zstdBackPeek(const ZstdBackReader& r, uint32_t count) {
    return r.consumed < 64 ? r.bits << r.consumed >> 1 >> (63 - count) : 0;
}

// Takes the next `count` bits, as zstdBackPeek reads them.
inline uint64_t zstdBackRead(ZstdBackReader& r, uint32_t count) {
    uint64_t value = zstdBackPeek(r, count);
    r.consumed += count;
    return value;
}

// Moves the reader back over the bytes it has read, so that it holds at least 56 bits unless the stream's start is
// near. More means that it does, Last that the stream's start is in the bits it holds, Done that it has read every bit,
// and Overflow that it has read past the start. Between reloads a decoder reads at most 56 bits, so while eight bytes
// lie before the reader, it has not read past them.
inline ZstdReload zstdBackReload(ZstdBackReader& r) {
    if (r.at >= 8) {
        r.at -= int64_t(r.consumed >> 3);
        r.consumed &= 7;
        r.bits = prMemReadLeU64(r.data + r.at);
        return ZstdReload_More;
    }
    if (r.consumed > 64)
        return ZstdReload_Overflow;
    if (r.at == 0)
        return r.consumed == 64 ? ZstdReload_Done : ZstdReload_Last;
    int64_t back = min(int64_t(r.consumed >> 3), r.at);
    r.at -= back;
    r.consumed -= uint64_t(back) * 8;
    r.bits = prMemReadLeU64(r.data + r.at);
    return ZstdReload_Last;
}

// Whether the reader has read exactly every bit of its stream.
inline bool zstdBackDone(ZstdBackReader& r) {
    return zstdBackReload(r) == ZstdReload_Done;
}

// Reads an FSE table description (rfc 8878 4.1.1) from the start of the `size` bytes at `src`: the accuracy log, at
// most `maxLog`, and the counts of the symbols, of which there are at most `symbols`. Returns the bytes it took, or -1
// for a description that is corrupt.
int64_t zstdReadFseCounts(const uint8_t* src, int64_t size, int16_t* counts, int64_t symbols, uint32_t maxLog, uint32_t* outLog);

// Builds the decoding table of `counts` at accuracy `log` into `cells` (rfc 8878 4.1.1): a symbol of probability below
// one at the highest states, and the others stepping through the rest. Returns false if the counts do not fill the
// table exactly.
bool zstdBuildFse(const int16_t* counts, int64_t symbols, uint32_t log, ZstdFseCell* cells);

// Reads the weights of a Huffman code (rfc 8878 4.2.1.1) from the `length` bytes at `src` into `weights`: those of
// every symbol but the last, `*outCount` of them. Returns the bytes they took, or -1.
int64_t zstdReadWeights(const uint8_t* src, int64_t length, uint8_t* weights, int64_t* outCount);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
