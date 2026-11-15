// LZFSE decoding, a piece of input at a time.
//
// A stream is a sequence of blocks, each led by a magic number, that ends with an end-of-stream block. A block holds
// raw bytes, LZVN commands, or LZFSE's literals and (L, M, D) triples: L literals, then M bytes copied from D bytes
// back. LZFSE codes the literals and the triples with finite state entropy (tANS) in streams that read backward from
// their ends, so a decoder holds a whole compressed block before it decodes any of it.

#include "compress/backends/builtin/src/lzfse.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/integer.h"
#include "pr/mem.h"
#include "pr/min_max.h"
#include "pr/raw_array.h"

#include "compress/backends/builtin/src/compress_copy.h"

namespace pr_impl {
namespace {

// The magic numbers that lead the blocks.
constexpr uint32_t kMagicEnd = 0x24787662;      // "bvx$": the end of the stream
constexpr uint32_t kMagicRaw = 0x2d787662;      // "bvx-": raw bytes
constexpr uint32_t kMagicV1 = 0x31787662;       // "bvx1": LZFSE, with whole frequency tables
constexpr uint32_t kMagicV2 = 0x32787662;       // "bvx2": LZFSE, with packed fields and coded frequency tables
constexpr uint32_t kMagicLzvn = 0x6e787662;     // "bvxn": LZVN

// The sizes of the block headers. A v2 header has a fixed part, then frequency tables of the size it gives.
constexpr uint32_t kRawHeaderSize = 8;
constexpr uint32_t kLzvnHeaderSize = 12;
constexpr uint32_t kV1HeaderSize = 772;
constexpr uint32_t kV2FixedSize = 32;

// The symbols and states of the four FSE codes of an LZFSE block.
constexpr uint32_t kLSymbols = 20;
constexpr uint32_t kMSymbols = 20;
constexpr uint32_t kDSymbols = 64;
constexpr uint32_t kLiteralSymbols = 256;
constexpr uint32_t kLStates = 64;
constexpr uint32_t kMStates = 64;
constexpr uint32_t kDStates = 256;
constexpr uint32_t kLiteralStates = 1024;

// The frequency tables, in the order that a header stores them: L, M, D and literals.
constexpr uint32_t kFrequencies = kLSymbols + kMSymbols + kDSymbols + kLiteralSymbols;
constexpr uint32_t kMOffset = kLSymbols;
constexpr uint32_t kDOffset = kMOffset + kMSymbols;
constexpr uint32_t kLiteralOffset = kDOffset + kDSymbols;

// The largest v2 header: the fixed part and frequencies of at most 16 bits each.
constexpr uint32_t kV2MaxHeaderSize = kV2FixedSize + 2 * kFrequencies;

// The most triples and literals of an LZFSE block.
constexpr uint32_t kMaxMatches = 10000;
constexpr uint32_t kMaxLiterals = 4 * kMaxMatches;

// The window, a power of two above the farthest back-reference: an LZFSE distance reaches 262139 bytes back, and an
// LZVN distance 65535.
constexpr uint32_t kWindowSize = 1u << 18;

// The number of bytes past a command's literals and its match that its copy may read and write, a word at a time.
constexpr uint32_t kCopySlack = 16;

// The most bytes that one LZFSE triple produces: the largest L and M, each its last symbol's base and extra bits.
constexpr uint32_t kLmdMaxCommand = 315 + 2359;

// The most bytes that one LZVN command produces: 271 literals, or a match of 271 bytes.
constexpr uint32_t kLzvnMaxCommand = 271;

// The extra bits and the base values of the L, M and D symbols. A value is its symbol's base plus its extra bits.
constexpr uint8_t kLExtraBits[kLSymbols] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 3, 5, 8};
constexpr int32_t kLBase[kLSymbols] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 20, 28, 60};
constexpr uint8_t kMExtraBits[kMSymbols] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 5, 8, 11};
constexpr int32_t kMBase[kMSymbols] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 24, 56, 312};
constexpr uint8_t kDExtraBits[kDSymbols] = {
    0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 6, 6, 6, 6, 7, 7, 7, 7,
    8, 8, 8, 8, 9, 9, 9, 9, 10, 10, 10, 10, 11, 11, 11, 11, 12, 12, 12, 12, 13, 13, 13, 13, 14, 14, 14, 14, 15, 15, 15,
    15,
};
constexpr int32_t kDBase[kDSymbols] = {
    0,      1,      2,      3,      4,     6,     8,     10,    12,    16,    20,     24,     28,     36,    44,    52,
    60,     76,     92,     108,    124,   156,   188,   220,   252,   316,   380,    444,    508,    636,   764,   892,
    1020,   1276,   1532,   1788,   2044,  2556,  3068,  3580,  4092,  5116,  6140,   7164,   8188,   10236, 12284, 14332,
    16380,  20476,  24572,  28668,  32764, 40956, 49148, 57340, 65532, 81916, 98300,  114684, 131068, 163836, 196604,
    229372,
};

static_assert(kLmdMaxCommand == uint32_t(kLBase[kLSymbols - 1] + (1 << kLExtraBits[kLSymbols - 1]) - 1 +
    kMBase[kMSymbols - 1] + (1 << kMExtraBits[kMSymbols - 1]) - 1));

// The point that a decoder has reached in the stream.
enum {
    LzfsePhase_Block,       // gathers the next block into `block`
    LzfsePhase_Raw,         // copies `rawLeft` bytes of input
    LzfsePhase_Lzvn,        // runs the LZVN commands of `block`
    LzfsePhase_Lmd,         // runs the triples of the LZFSE block in `block`
    LzfsePhase_Done,
    LzfsePhase_Invalid,
};
typedef uint8_t LzfsePhase;

// The outcome of a step.
enum {
    LzfseStep_Next,         // the phase changed, and decoding goes on
    LzfseStep_Input,        // the input ended inside the step
    LzfseStep_Output,       // the window has no room for the step's output
    LzfseStep_Done,
    LzfseStep_Invalid,
};
typedef uint8_t LzfseStep;

// An entry of the table of the literal code, by state. The next state is `delta` plus the next `bits` bits.
struct LiteralEntry {
    uint8_t bits;
    uint8_t symbol;
    int16_t delta;
};

// An entry of the table of an L, M or D code, by state. It reads `bits` bits: the low `extraBits` of them add to `base`
// to make the value, and the rest add to `delta` to make the next state.
struct ValueEntry {
    uint8_t bits;
    uint8_t extraBits;
    int16_t delta;
    int32_t base;
};

// The fields of an LZFSE block header that decoding needs, from either version.
struct LmdHeader {
    uint32_t literalCount;
    uint32_t matchCount;
    uint32_t literalBytes;          // of the literal stream
    uint32_t lmdBytes;              // of the triple stream
    int32_t literalBits;            // the unused bits of the literal stream's last byte, negated
    int32_t lmdBits;                // the unused bits of the triple stream's last byte, negated
    uint16_t literalStates[4];
    uint16_t lState;
    uint16_t mState;
    uint16_t dState;
    uint16_t frequencies[kFrequencies];
};

// A stream of bits that reads backward from the end of its bytes. `bits` holds `count` bits, the next highest, loaded
// from the bytes at and after `at`.
struct BackwardBits {
    uint64_t bits;
    int32_t count;
    uint32_t at;
};

// The input of a read.
struct LzfseInput {
    const uint8_t* PR_NULLABLE data;
    size_t size;
    size_t at;
};

// The caller's output, and how much of it a read has written.
struct LzfseOutput {
    uint8_t* PR_NULLABLE data;
    size_t capacity;
    size_t written;
};

// The codes of an LZFSE block and its decoded literals, which a decoder makes when it meets its first LZFSE block. The
// literals have room for kCopySlack bytes past the most that a block holds, which a copy may read.
struct LmdCodes {
    ValueEntry lTable[kLStates];
    ValueEntry mTable[kMStates];
    ValueEntry dTable[kDStates];
    LiteralEntry literalTable[kLiteralStates];
    uint8_t literals[kMaxLiterals + kCopySlack];
};

struct LzfseState {
    LzfsePhase phase;

    // The number of bytes that the stream has produced. Of the bytes that earlier reads wrote, `history` holds the
    // last, up to kWindowSize, for the matches that reach back past a read's own output. It is a ring that a decoder
    // makes when a read ends inside the stream, and `historyAt` is where it writes next.
    uint64_t position;
    uint8_t* PR_NULLABLE history;
    uint32_t historyAt;

    // The block being decoded, from its magic number on: `blockSize` bytes at `blockData`. A read decodes a block that
    // its input holds whole in place, and `block` gathers a block that spans reads and keeps a block that a read leaves
    // unfinished. `rawLeft` is the number of bytes that the block has yet to produce.
    RawArray<uint8_t> block;
    const uint8_t* PR_NULLABLE blockData;
    uint32_t blockSize;
    uint32_t rawLeft;

    // The command being run: the literals that it has yet to copy, which start at `literalAt` of the block for LZVN and
    // of the codes' literals for LZFSE, then the bytes that it has yet to copy from `distance` back.
    uint32_t literalLeft;
    uint32_t literalAt;
    uint32_t matchLeft;
    uint32_t distance;

    // LZVN: the next command, at `commandAt` of the block.
    uint32_t commandAt;

    // LZFSE: the triples left, the triple stream, the states of its codes, and the codes.
    uint32_t matchesLeft;
    uint32_t lmdStart;              // the offset of the triple stream in the block
    BackwardBits lmd;
    uint16_t lState;
    uint16_t mState;
    uint16_t dState;
    int32_t lzfseDistance;          // the last D, or -1 before the block's first, which no triple can reuse
    uint32_t literalCount;
    LmdCodes* PR_NULLABLE codes;
};

// Returns the bits of `value` from bit `offset`, `count` of them, up to 32.
uint32_t field(uint64_t value, uint32_t offset, uint32_t count) {
    return uint32_t((value >> offset) & ((uint64_t(1) << count) - 1));
}

// Returns the shift that puts `frequency` << shift in [states, 2 * states), for a frequency of at most `states`.
uint32_t stateShift(uint32_t frequency, uint32_t states) {
    return prU64HighestBit(states) - prU64HighestBit(frequency);
}

// Returns true if the frequencies sum to at most `states`. A code whose frequencies sum to less leaves the rest of its
// states zero, which read nothing and decode the first symbol.
bool frequenciesFit(const uint16_t* frequencies, uint32_t symbols, uint32_t states) {
    uint32_t sum = 0;
    for (uint32_t i = 0; i < symbols; ++i)
        sum += frequencies[i];
    return sum <= states;
}

// Builds the table of the literal code. Symbol s takes the next frequencies[s] states. The first `low` of those states
// read `shift` bits and the rest read one fewer, so that together they cover all the states.
void buildLiteralTable(const uint16_t* frequencies, LiteralEntry* table) {
    prMemZero(table, sizeof(LiteralEntry) * kLiteralStates);
    uint32_t state = 0;
    for (uint32_t symbol = 0; symbol < kLiteralSymbols; ++symbol) {
        const uint32_t frequency = frequencies[symbol];
        if (frequency == 0)
            continue;
        const uint32_t shift = stateShift(frequency, kLiteralStates);
        const uint32_t low = ((2 * kLiteralStates) >> shift) - frequency;
        for (uint32_t j = 0; j < frequency; ++j, ++state) {
            LiteralEntry& e = table[state];
            e.symbol = uint8_t(symbol);
            if (j < low) {
                e.bits = uint8_t(shift);
                e.delta = int16_t(int32_t((frequency + j) << shift) - int32_t(kLiteralStates));
            } else {
                e.bits = uint8_t(shift - 1);
                e.delta = int16_t((j - low) << (shift - 1));
            }
        }
    }
}

// Builds the table of an L, M or D code, as buildLiteralTable does, with each symbol's extra bits and base.
void buildValueTable(const uint16_t* frequencies, uint32_t symbols, uint32_t states, const uint8_t* extraBits,
        const int32_t* base, ValueEntry* table) {
    prMemZero(table, sizeof(ValueEntry) * states);
    uint32_t state = 0;
    for (uint32_t symbol = 0; symbol < symbols; ++symbol) {
        const uint32_t frequency = frequencies[symbol];
        if (frequency == 0)
            continue;
        const uint32_t shift = stateShift(frequency, states);
        const uint32_t low = ((2 * states) >> shift) - frequency;
        for (uint32_t j = 0; j < frequency; ++j, ++state) {
            ValueEntry& e = table[state];
            e.extraBits = extraBits[symbol];
            e.base = base[symbol];
            if (j < low) {
                e.bits = uint8_t(shift + extraBits[symbol]);
                e.delta = int16_t(int32_t((frequency + j) << shift) - int32_t(states));
            } else {
                e.bits = uint8_t(shift - 1 + extraBits[symbol]);
                e.delta = int16_t((j - low) << (shift - 1));
            }
        }
    }
}

// Starts a stream that ends at `end` of `data`. `extra`, from -7 to 0, is the bits of the last byte that the encoder
// left unused, negated. Returns false if the stream is too short or its unused bits are not zero.
bool bitsStart(BackwardBits& r, const uint8_t* data, uint32_t start, uint32_t end, int32_t extra) {
    if (extra < -7 || extra > 0)
        return false;
    if (extra != 0) {
        if (end - start < 8)
            return false;
        r.at = end - 8;
        r.bits = prMemReadLeU64(data + r.at);
        r.count = 64 + extra;
    } else {
        if (end - start < 7)
            return false;
        r.at = end - 7;
        r.bits = uint64_t(prMemReadLeU32(data + r.at)) | uint64_t(prMemReadLeU16(data + r.at + 4)) << 32 |
            uint64_t(data[r.at + 6]) << 48;
        r.count = 56;
    }
    return (r.bits >> r.count) == 0;
}

// Loads whole bytes until the stream holds at least 56 bits. Returns false if that reads before `start`.
bool bitsFill(BackwardBits& r, const uint8_t* data, uint32_t start) {
    const int32_t added = (63 - r.count) & ~7;
    if (added == 0)
        return true;
    const uint32_t bytes = uint32_t(added) / 8;
    if (r.at - start < bytes)
        return false;
    r.at -= bytes;
    const uint64_t incoming = prMemReadLeU64(data + r.at);
    r.bits = r.bits << added | (incoming & ((uint64_t(1) << added) - 1));
    r.count += added;
    return true;
}

// Takes the next `count` bits, at most the bits held.
inline uint32_t bitsTake(BackwardBits& r, int32_t count) {
    r.count -= count;
    const uint64_t taken = r.bits >> r.count;
    r.bits &= (uint64_t(1) << r.count) - 1;
    return uint32_t(taken);
}

inline uint8_t decodeLiteral(uint16_t& state, const LiteralEntry* table, BackwardBits& r) {
    const LiteralEntry e = table[state];
    state = uint16_t(e.delta + int32_t(bitsTake(r, e.bits)));
    return e.symbol;
}

inline int32_t decodeValue(uint16_t& state, const ValueEntry* table, BackwardBits& r) {
    const ValueEntry e = table[state];
    const uint32_t bits = bitsTake(r, e.bits);
    state = uint16_t(e.delta + int32_t(bits >> e.extraBits));
    return e.base + int32_t(bits & ((1u << e.extraBits) - 1));
}

// Reads the frequency tables of a v2 header, from `data` to `end`. Each frequency has a code of 2 to 14 bits, read from
// the lowest bit. Returns false if the codes do not end with the header.
bool readFrequencies(const uint8_t* data, const uint8_t* end, uint16_t* frequencies) {
    // The code of each frequency below 8, and its length, by the next five bits. Values of 8 and more have codes of 8
    // and 14 bits, which these mark with -1.
    static constexpr uint8_t kLengths[32] = {
        2, 3, 2, 5, 2, 3, 2, 8, 2, 3, 2, 5, 2, 3, 2, 14, 2, 3, 2, 5, 2, 3, 2, 8, 2, 3, 2, 5, 2, 3, 2, 14,
    };
    static constexpr int8_t kValues[32] = {
        0, 2, 1, 4, 0, 3, 1, -1, 0, 2, 1, 5, 0, 3, 1, -1, 0, 2, 1, 6, 0, 3, 1, -1, 0, 2, 1, 7, 0, 3, 1, -1,
    };
    if (data == end) {
        prMemZero(frequencies, sizeof(uint16_t) * kFrequencies);
        return true;
    }
    uint32_t bits = 0;
    uint32_t count = 0;
    for (uint32_t i = 0; i < kFrequencies; ++i) {
        while (data < end && count + 8 <= 32) {
            bits |= uint32_t(*data++) << count;
            count += 8;
        }
        const uint32_t length = kLengths[bits & 31];
        if (length > count)
            return false;
        if (length == 8)
            frequencies[i] = uint16_t(8 + ((bits >> 4) & 0xf));
        else if (length == 14)
            frequencies[i] = uint16_t(24 + ((bits >> 4) & 0x3ff));
        else
            frequencies[i] = uint16_t(kValues[bits & 31]);
        bits >>= length;
        count -= length;
    }
    return count < 8 && data == end;
}

// Reads the header of an LZFSE block of either version from `data`, which holds `size` bytes. Returns false for fields
// out of range.
bool readLmdHeader(uint32_t magic, const uint8_t* data, uint32_t size, LmdHeader* out) {
    if (magic == kMagicV1) {
        out->literalCount = prMemReadLeU32(data + 12);
        out->matchCount = prMemReadLeU32(data + 16);
        out->literalBytes = prMemReadLeU32(data + 20);
        out->lmdBytes = prMemReadLeU32(data + 24);
        out->literalBits = int32_t(prMemReadLeU32(data + 28));
        for (uint32_t i = 0; i < 4; ++i)
            out->literalStates[i] = prMemReadLeU16(data + 32 + 2 * i);
        out->lmdBits = int32_t(prMemReadLeU32(data + 40));
        out->lState = prMemReadLeU16(data + 44);
        out->mState = prMemReadLeU16(data + 46);
        out->dState = prMemReadLeU16(data + 48);
        for (uint32_t i = 0; i < kFrequencies; ++i)
            out->frequencies[i] = prMemReadLeU16(data + 50 + 2 * i);
    } else {
        const uint64_t v0 = prMemReadLeU64(data + 8);
        const uint64_t v1 = prMemReadLeU64(data + 16);
        const uint64_t v2 = prMemReadLeU64(data + 24);
        out->literalCount = field(v0, 0, 20);
        out->literalBytes = field(v0, 20, 20);
        out->matchCount = field(v0, 40, 20);
        out->literalBits = int32_t(field(v0, 60, 3)) - 7;
        for (uint32_t i = 0; i < 4; ++i)
            out->literalStates[i] = uint16_t(field(v1, 10 * i, 10));
        out->lmdBytes = field(v1, 40, 20);
        out->lmdBits = int32_t(field(v1, 60, 3)) - 7;
        out->lState = uint16_t(field(v2, 32, 10));
        out->mState = uint16_t(field(v2, 42, 10));
        out->dState = uint16_t(field(v2, 52, 10));
        if (!readFrequencies(data + kV2FixedSize, data + size, out->frequencies))
            return false;
    }
    // The encoder pads the literals to a multiple of four, since four interleaved states decode them.
    bool valid = out->literalCount <= kMaxLiterals && out->literalCount % 4 == 0 && out->matchCount <= kMaxMatches &&
        out->lState < kLStates && out->mState < kMStates && out->dState < kDStates;
    for (uint32_t i = 0; i < 4; ++i)
        valid = valid && out->literalStates[i] < kLiteralStates;
    return valid && frequenciesFit(out->frequencies, kLSymbols, kLStates) &&
        frequenciesFit(out->frequencies + kMOffset, kMSymbols, kMStates) &&
        frequenciesFit(out->frequencies + kDOffset, kDSymbols, kDStates) &&
        frequenciesFit(out->frequencies + kLiteralOffset, kLiteralSymbols, kLiteralStates);
}

// Returns the byte `distance` back from the end of the output. The history holds it if this read has not written that
// far.
uint8_t byteBack(const LzfseState& d, const LzfseOutput& out, uint32_t distance) {
    if (distance <= out.written)
        return out.data[out.written - distance];
    const uint32_t back = distance - uint32_t(out.written);
    return d.history[(d.historyAt - back) & (kWindowSize - 1)];
}

// Appends the `size` bytes that a read wrote to the history.
void keepHistory(LzfseState& d, const uint8_t* data, size_t size) {
    if (size == 0)
        return;
    if (d.history == nullptr)
        d.history = static_cast<uint8_t*>(prMemoryAlloc(kWindowSize));
    if (size >= kWindowSize) {
        prMemCopy(d.history, data + size - kWindowSize, kWindowSize);
        d.historyAt = 0;
        return;
    }
    const uint32_t count = uint32_t(size);
    const uint32_t first = min(count, kWindowSize - d.historyAt);
    prMemCopy(d.history + d.historyAt, data, first);
    prMemCopy(d.history, data + first, count - first);
    d.historyAt = (d.historyAt + count) & (kWindowSize - 1);
}

// Copies the literals from `literals` and then the match of the command being run, as far as the output has room.
// Returns false if it runs out of room first.
bool runCommand(LzfseState& d, LzfseOutput& out, const uint8_t* literals) {
    if (d.literalLeft != 0) {
        const uint32_t count = uint32_t(min(size_t(d.literalLeft), out.capacity - out.written));
        if (count != 0)
            prMemCopy(out.data + out.written, literals + d.literalAt, count);
        out.written += count;
        d.position += count;
        d.literalAt += count;
        d.literalLeft -= count;
        if (d.literalLeft != 0)
            return false;
    }
    // A match copies a byte at a time here, where it reaches into the history or the output ends inside it.
    for (; d.matchLeft != 0; --d.matchLeft) {
        if (out.written == out.capacity)
            return false;
        out.data[out.written] = byteBack(d, out, d.distance);
        ++out.written;
        ++d.position;
    }
    return true;
}

// Runs the command whole, a word at a time, if the output has room for it and kCopySlack bytes more and its match
// reaches back no further than this read's output. Otherwise copies nothing and returns false. The copies may write up
// to kCopySlack bytes past the command, and the literals' copy may read up to kCopySlack bytes past them where
// `literals` holds `literalsEnd` bytes.
bool runFast(LzfseState& d, LzfseOutput& out, const uint8_t* literals, size_t literalsEnd) {
    const uint32_t count = d.literalLeft + d.matchLeft;
    if (out.capacity - out.written < size_t(count) + kCopySlack ||
            (d.matchLeft != 0 && d.distance > out.written + d.literalLeft))
        return false;
    uint8_t* to = out.data + out.written;
    const uint8_t* from = literals + d.literalAt;
    if (size_t(d.literalAt) + d.literalLeft + kCopySlack <= literalsEnd) {
        for (uint32_t at = 0; at < d.literalLeft; at += 16)
            prMemCopy(to + at, from + at, 16);
    } else if (d.literalLeft != 0) {
        prMemCopy(to, from, d.literalLeft);
    }
    if (d.matchLeft != 0)
        compressCopyMatch(to + d.literalLeft, d.distance, d.matchLeft);
    out.written += count;
    d.position += count;
    d.literalAt += d.literalLeft;
    d.literalLeft = 0;
    d.matchLeft = 0;
    return true;
}

// Copies input into `block` until it holds `size` bytes. Returns false if the input ends first.
bool gather(LzfseState& d, LzfseInput& in, uint32_t size) {
    if (d.block.count < size) {
        const size_t taken = min(size_t(size - d.block.count), in.size - in.at);
        d.block.append(in.data + in.at, uint32_t(taken));
        in.at += taken;
    }
    return d.block.count >= size;
}

// Returns the first `size` bytes of the next block: the input's, if the block starts in this read's input and the input
// holds them, and otherwise `block`'s, once it has gathered them. Returns null if the input ends first.
const uint8_t* PR_NULLABLE peekBlock(LzfseState& d, LzfseInput& in, uint32_t size) {
    if (d.block.count == 0 && in.size - in.at >= size)
        return in.data + in.at;
    return gather(d, in, size) ? d.block.data : nullptr;
}

// Starts the block of `size` bytes, which peekBlock returned whole at `data`, and reads it from the input if it is the
// input's.
void takeBlock(LzfseState& d, LzfseInput& in, const uint8_t* data, uint32_t size) {
    if (d.block.count == 0)
        in.at += size;
    d.blockData = data;
    d.blockSize = size;
}

void endBlock(LzfseState& d) {
    d.block.clear();
    d.blockData = nullptr;
    d.blockSize = 0;
}

// Decodes the literals of the LZFSE block, whose header is `headerSize` bytes, and starts its triples.
bool startLmd(LzfseState& d, const LmdHeader& header, uint32_t headerSize) {
    if (d.codes == nullptr)
        d.codes = alloc<LmdCodes>();
    LmdCodes& c = *d.codes;
    const uint16_t* f = header.frequencies;
    buildValueTable(f, kLSymbols, kLStates, kLExtraBits, kLBase, c.lTable);
    buildValueTable(f + kMOffset, kMSymbols, kMStates, kMExtraBits, kMBase, c.mTable);
    buildValueTable(f + kDOffset, kDSymbols, kDStates, kDExtraBits, kDBase, c.dTable);
    buildLiteralTable(f + kLiteralOffset, c.literalTable);

    // The literal stream may read the header before it, which the stream's last bytes load along with its first bits.
    const uint8_t* data = d.blockData;
    BackwardBits r{};
    if (!bitsStart(r, data, 0, headerSize + header.literalBytes, header.literalBits))
        return false;
    uint16_t states[4] = {
        header.literalStates[0], header.literalStates[1], header.literalStates[2], header.literalStates[3],
    };
    for (uint32_t i = 0; i < header.literalCount; i += 4) {
        if (!bitsFill(r, data, 0))
            return false;
        for (uint32_t k = 0; k < 4; ++k)
            c.literals[i + k] = decodeLiteral(states[k], c.literalTable, r);
    }
    d.literalCount = header.literalCount;
    d.literalAt = 0;

    d.lmdStart = headerSize + header.literalBytes;
    if (!bitsStart(d.lmd, data, d.lmdStart, d.lmdStart + header.lmdBytes, header.lmdBits))
        return false;
    d.matchesLeft = header.matchCount;
    d.lState = header.lState;
    d.mState = header.mState;
    d.dState = header.dState;
    d.lzfseDistance = -1;
    return true;
}

// Takes the next block and starts its phase.
LzfseStep stepBlock(LzfseState& d, LzfseInput& in) {
    const uint8_t* data = peekBlock(d, in, 4);
    if (data == nullptr)
        return LzfseStep_Input;
    const uint32_t magic = prMemReadLeU32(data);
    if (magic == kMagicEnd) {
        takeBlock(d, in, data, 4);
        endBlock(d);
        d.phase = LzfsePhase_Done;
        return LzfseStep_Next;
    }
    if (magic == kMagicRaw) {
        data = peekBlock(d, in, kRawHeaderSize);
        if (data == nullptr)
            return LzfseStep_Input;
        takeBlock(d, in, data, kRawHeaderSize);
        d.rawLeft = prMemReadLeU32(data + 4);
        endBlock(d);
        d.phase = LzfsePhase_Raw;
        return LzfseStep_Next;
    }
    uint32_t headerSize = 0;
    uint64_t size = 0;
    if (magic == kMagicLzvn) {
        data = peekBlock(d, in, kLzvnHeaderSize);
        if (data == nullptr)
            return LzfseStep_Input;
        size = uint64_t(kLzvnHeaderSize) + prMemReadLeU32(data + 8);
    } else if (magic == kMagicV1) {
        data = peekBlock(d, in, kV1HeaderSize);
        if (data == nullptr)
            return LzfseStep_Input;
        headerSize = kV1HeaderSize;
        size = headerSize + uint64_t(prMemReadLeU32(data + 20)) + prMemReadLeU32(data + 24);
    } else if (magic == kMagicV2) {
        data = peekBlock(d, in, kV2FixedSize);
        if (data == nullptr)
            return LzfseStep_Input;
        const uint64_t v0 = prMemReadLeU64(data + 8);
        const uint64_t v1 = prMemReadLeU64(data + 16);
        headerSize = prMemReadLeU32(data + 24);
        if (headerSize < kV2FixedSize || headerSize > kV2MaxHeaderSize)
            return LzfseStep_Invalid;
        size = headerSize + uint64_t(field(v0, 20, 20)) + field(v1, 40, 20);
    } else {
        return LzfseStep_Invalid;
    }
    if (size > 0xffffffffu)
        return LzfseStep_Invalid;
    data = peekBlock(d, in, uint32_t(size));
    if (data == nullptr)
        return LzfseStep_Input;
    takeBlock(d, in, data, uint32_t(size));
    d.rawLeft = prMemReadLeU32(data + 4);
    if (magic == kMagicLzvn) {
        d.commandAt = kLzvnHeaderSize;
        d.distance = 0;
        d.phase = LzfsePhase_Lzvn;
        return LzfseStep_Next;
    }
    LmdHeader header;
    if (!readLmdHeader(magic, data, headerSize, &header) || !startLmd(d, header, headerSize))
        return LzfseStep_Invalid;
    d.phase = LzfsePhase_Lmd;
    return LzfseStep_Next;
}

// Copies the raw bytes of an uncompressed block.
LzfseStep stepRaw(LzfseState& d, LzfseInput& in, LzfseOutput& out) {
    while (d.rawLeft != 0) {
        if (in.at == in.size)
            return LzfseStep_Input;
        if (out.written == out.capacity)
            return LzfseStep_Output;
        const size_t count = min(min(size_t(d.rawLeft), in.size - in.at), out.capacity - out.written);
        prMemCopy(out.data + out.written, in.data + in.at, count);
        in.at += count;
        out.written += count;
        d.position += count;
        d.rawLeft -= uint32_t(count);
    }
    d.phase = LzfsePhase_Block;
    return LzfseStep_Next;
}

// Returns true if a command of `literals` literals and then a match of `match` bytes from `distance` back fits in what
// the block has yet to produce, and the match reaches no further back than the stream's output.
bool commandFits(const LzfseState& d, uint32_t literals, uint32_t match, uint64_t distance) {
    if (uint64_t(literals) + match > d.rawLeft)
        return false;
    return match == 0 || (distance != 0 && distance <= d.position + literals);
}

// What an LZVN opcode is.
enum {
    LzvnOp_Command,         // a command: literals, then a match
    LzvnOp_Nop,             // a byte that does nothing
    LzvnOp_End,             // the end of the block
    LzvnOp_Invalid,
};
typedef uint8_t LzvnOp;

// An LZVN command: its opcode and operands, which take `size` bytes, then `literals` literals, then a match of `match`
// bytes from `distance` back.
struct LzvnCommand {
    uint32_t size;
    uint32_t literals;
    uint32_t match;
    uint32_t distance;
};

// Reads the opcode at `at` of the block's `end` bytes, and the command that it leads into `*out`. A command that gives
// no distance has the last one, `last`.
LzvnOp readLzvn(const uint8_t* data, uint32_t at, uint32_t end, uint32_t last, LzvnCommand* out) {
    const uint32_t opcode = data[at];
    // The opcodes with no command: the end of the stream, two that do nothing, and the undefined ones.
    if (opcode == 6)
        return LzvnOp_End;
    if (opcode == 14 || opcode == 22)
        return LzvnOp_Nop;
    if ((opcode < 64 && (opcode & 7) == 6) || (opcode >= 112 && opcode < 128) || (opcode >= 208 && opcode < 224))
        return LzvnOp_Invalid;
    const uint32_t operand = at + 1 < end ? data[at + 1] : 0;
    const uint32_t operands = at + 2 < end ? operand | uint32_t(data[at + 2]) << 8 : 0;
    *out = LzvnCommand{.size = 1, .literals = 0, .match = 0, .distance = last};
    if (opcode >= 240) {
        // A match with the last distance: 1111MMMM, or 11110000 MMMMMMMM for 16 bytes and more.
        out->match = opcode & 15;
        if (out->match == 0) {
            out->size = 2;
            out->match = operand + 16;
        }
    } else if (opcode >= 224) {
        // Literals: 1110LLLL, or 11100000 LLLLLLLL for 16 and more.
        out->literals = opcode & 15;
        if (out->literals == 0) {
            out->size = 2;
            out->literals = operand + 16;
        }
    } else if (opcode >= 160 && opcode < 192) {
        // A medium distance: 101LLMMM DDDDDDMM DDDDDDDD.
        out->size = 3;
        out->literals = (opcode >> 3) & 3;
        out->match = ((opcode & 7) << 2 | (operands & 3)) + 3;
        out->distance = operands >> 2;
    } else {
        // LLMMMDDD DDDDDDDD for a small distance, LLMMM111 DDDDDDDD DDDDDDDD for a large one, and LLMMM110 to reuse the
        // last distance.
        out->literals = opcode >> 6;
        out->match = ((opcode >> 3) & 7) + 3;
        if ((opcode & 7) == 7) {
            out->size = 3;
            out->distance = operands;
        } else if ((opcode & 7) != 6) {
            out->size = 2;
            out->distance = (opcode & 7) << 8 | operand;
        }
    }
    return LzvnOp_Command;
}

// Runs LZVN commands while the output has room for the longest command and kCopySlack bytes more, with the decoder's
// state in locals, as runLmdFast does. It stops before the end of the block, before a command whose match reaches back
// past this read's output, and before invalid data, each of which stepLzvn reads again.
void runLzvnFast(LzfseState& d, LzfseOutput& out) {
    const uint8_t* data = d.blockData;
    const uint32_t end = d.blockSize;
    uint32_t at = d.commandAt;
    uint32_t last = d.distance;
    uint32_t rawLeft = d.rawLeft;
    uint8_t* const output = out.data;
    size_t written = out.written;
    while (at < end && out.capacity - written >= kLzvnMaxCommand + kCopySlack) {
        LzvnCommand command;
        const LzvnOp op = readLzvn(data, at, end, last, &command);
        if (op == LzvnOp_Nop) {
            ++at;
            continue;
        }
        const uint32_t literals = command.literals;
        const uint32_t match = command.match;
        const uint32_t from = at + command.size;
        if (op != LzvnOp_Command || uint64_t(from) + literals >= end || literals + match > rawLeft ||
                (match != 0 && (command.distance == 0 || command.distance > written + literals)))
            break;
        uint8_t* to = output + written;
        if (from + literals + kCopySlack <= end) {
            for (uint32_t k = 0; k < literals; k += 16)
                prMemCopy(to + k, data + from + k, 16);
        } else if (literals != 0) {
            prMemCopy(to, data + from, literals);
        }
        if (match != 0)
            compressCopyMatch(to + literals, command.distance, match);
        rawLeft -= literals + match;
        written += literals + match;
        last = command.distance;
        at = from + literals;
    }
    d.position += written - out.written;
    out.written = written;
    d.commandAt = at;
    d.distance = last;
    d.rawLeft = rawLeft;
}

// Runs the commands of an LZVN block. Each command is an opcode and its operands, then up to 271 literals, then a
// match. A command that gives no distance reuses the last one. The block ends with an end-of-stream command, which pads
// it to eight bytes.
LzfseStep stepLzvn(LzfseState& d, LzfseOutput& out) {
    const uint8_t* data = d.blockData;
    const uint32_t end = d.blockSize;
    for (;;) {
        if (!runCommand(d, out, data))
            return LzfseStep_Output;
        runLzvnFast(d, out);
        const uint32_t at = d.commandAt;
        if (at >= end)
            return LzfseStep_Invalid;
        LzvnCommand command;
        const LzvnOp op = readLzvn(data, at, end, d.distance, &command);
        if (op == LzvnOp_End) {
            if (at + 8 != end || d.rawLeft != 0)
                return LzfseStep_Invalid;
            endBlock(d);
            d.phase = LzfsePhase_Block;
            return LzfseStep_Next;
        }
        if (op == LzvnOp_Nop) {
            d.commandAt = at + 1;
            continue;
        }
        // The command and its literals are followed by at least the opcode of the next command.
        if (op == LzvnOp_Invalid || uint64_t(at) + command.size + command.literals >= end ||
                !commandFits(d, command.literals, command.match, command.distance))
            return LzfseStep_Invalid;
        d.literalAt = at + command.size;
        d.literalLeft = command.literals;
        d.matchLeft = command.match;
        d.distance = command.distance;
        d.rawLeft -= command.literals + command.match;
        d.commandAt = at + command.size + command.literals;
        // A command that the fast copy declines runs at the top of the loop.
        runFast(d, out, data, end);
    }
}

// Runs triples while the output has room for the longest triple and kCopySlack bytes more. The decoder's state stays in
// locals, which the copies' byte stores would otherwise make the compiler reload. It stops at the end of the block's
// triples, and at a triple whose match reaches back past this read's output, which it leaves as the command being run.
// Returns false for invalid data.
bool runLmdFast(LzfseState& d, LzfseOutput& out) {
    const LmdCodes& c = *d.codes;
    const uint8_t* data = d.blockData;
    const uint32_t lmdStart = d.lmdStart;
    const uint32_t literalCount = d.literalCount;
    BackwardBits r = d.lmd;
    uint16_t lState = d.lState;
    uint16_t mState = d.mState;
    uint16_t dState = d.dState;
    int32_t last = d.lzfseDistance;
    uint32_t matches = d.matchesLeft;
    uint32_t literalAt = d.literalAt;
    uint32_t rawLeft = d.rawLeft;
    uint8_t* const output = out.data;
    size_t written = out.written;
    uint64_t position = d.position;
    bool valid = true;
    while (matches != 0 && out.capacity - written >= kLmdMaxCommand + kCopySlack) {
        if (!bitsFill(r, data, lmdStart)) {
            valid = false;
            break;
        }
        const uint32_t literals = uint32_t(decodeValue(lState, c.lTable, r));
        const uint32_t match = uint32_t(decodeValue(mState, c.mTable, r));
        const int32_t distance = decodeValue(dState, c.dTable, r);
        if (distance != 0)
            last = distance;
        --matches;
        if (literals > literalCount - literalAt || last < 0 || literals + match > rawLeft ||
                (match != 0 && uint64_t(last) > position + literals)) {
            valid = false;
            break;
        }
        rawLeft -= literals + match;
        if (match != 0 && uint32_t(last) > written + literals) {
            d.literalLeft = literals;
            d.matchLeft = match;
            d.distance = uint32_t(last);
            break;
        }
        uint8_t* to = output + written;
        const uint8_t* from = c.literals + literalAt;
        for (uint32_t at = 0; at < literals; at += 16)
            prMemCopy(to + at, from + at, 16);
        literalAt += literals;
        if (match != 0)
            compressCopyMatch(to + literals, uint32_t(last), match);
        written += literals + match;
        position += literals + match;
    }
    d.lmd = r;
    d.lState = lState;
    d.mState = mState;
    d.dState = dState;
    d.lzfseDistance = last;
    d.matchesLeft = matches;
    d.literalAt = literalAt;
    d.rawLeft = rawLeft;
    out.written = written;
    d.position = position;
    return valid;
}

// Runs the triples of an LZFSE block.
LzfseStep stepLmd(LzfseState& d, LzfseOutput& out) {
    const uint8_t* data = d.blockData;
    LmdCodes& c = *d.codes;
    for (;;) {
        if (!runCommand(d, out, c.literals))
            return LzfseStep_Output;
        if (d.matchesLeft == 0) {
            if (d.rawLeft != 0)
                return LzfseStep_Invalid;
            endBlock(d);
            d.phase = LzfsePhase_Block;
            return LzfseStep_Next;
        }
        // The fast loop runs what it can, and the rest runs a triple at a time.
        const uint32_t before = d.matchesLeft;
        if (!runLmdFast(d, out))
            return LzfseStep_Invalid;
        if (d.matchesLeft != before)
            continue;
        // The extra bits of a triple fit the 56 bits that a fill loads: 14 for L, 17 for M and 23 for D.
        if (!bitsFill(d.lmd, data, d.lmdStart))
            return LzfseStep_Invalid;
        const int32_t literals = decodeValue(d.lState, c.lTable, d.lmd);
        const int32_t match = decodeValue(d.mState, c.mTable, d.lmd);
        const int32_t distance = decodeValue(d.dState, c.dTable, d.lmd);
        // A D of zero reuses the last one.
        if (distance != 0)
            d.lzfseDistance = distance;
        --d.matchesLeft;
        if (uint32_t(literals) > d.literalCount - d.literalAt || d.lzfseDistance < 0 ||
                !commandFits(d, uint32_t(literals), uint32_t(match), uint64_t(d.lzfseDistance)))
            return LzfseStep_Invalid;
        d.literalLeft = uint32_t(literals);
        d.matchLeft = uint32_t(match);
        d.distance = uint32_t(d.lzfseDistance);
        d.rawLeft -= uint32_t(literals + match);
        // A triple that the fast copy declines runs at the top of the loop.
        runFast(d, out, c.literals, sizeof(c.literals));
    }
}

LzfseStep step(LzfseState& d, LzfseInput& in, LzfseOutput& out) {
    switch (d.phase) {
        case LzfsePhase_Block: return stepBlock(d, in);
        case LzfsePhase_Raw: return stepRaw(d, in, out);
        case LzfsePhase_Lzvn: return stepLzvn(d, out);
        case LzfsePhase_Lmd: return stepLmd(d, out);
        case LzfsePhase_Done: return LzfseStep_Done;
        default: return LzfseStep_Invalid;
    }
}

} // namespace

struct Lzfse : LzfseState {};

Lzfse* lzfseCreate(void) {
    Lzfse* decoder = allocZeroed<Lzfse>();
    lzfseReset(decoder);
    return decoder;
}

void lzfseReset(Lzfse* decoder) {
    LzfseState& d = *decoder;
    d.phase = LzfsePhase_Block;
    d.position = 0;
    d.historyAt = 0;
    endBlock(d);
    d.rawLeft = 0;
    d.literalLeft = d.matchLeft = 0;
}

void lzfseDestroy(Lzfse* PR_NULLABLE decoder) {
    if (decoder == nullptr)
        return;
    prMemoryFree(decoder->history);
    prMemoryFree(decoder->codes);
    decoder->block.reset();
    prMemoryFree(decoder);
}

LzfseResult lzfseRead(Lzfse* decoder, PrByteView bytes, bool final, size_t* consumed,
        uint8_t* PR_NULLABLE output, size_t capacity, size_t* written) {
    PR_ASSERT(bytes.data != nullptr || bytes.size == 0);
    PR_ASSERT(output != nullptr || capacity == 0);
    LzfseState& d = *decoder;
    LzfseOutput out{output, capacity, 0};
    LzfseInput in{bytes.data, bytes.size, 0};
    LzfseStep result = LzfseStep_Next;
    while (result == LzfseStep_Next)
        result = step(d, in, out);

    LzfseResult status = LzfseResult_Invalid;
    switch (result) {
        case LzfseStep_Input:
            status = final ? LzfseResult_Truncated : LzfseResult_Input;
            break;
        case LzfseStep_Output:
            status = LzfseResult_Output;
            break;
        case LzfseStep_Done:
            status = LzfseResult_Done;
            break;
        default:
            status = LzfseResult_Invalid;
            break;
    }
    if (status == LzfseResult_Input || status == LzfseResult_Output) {
        // The stream goes on in a later read, which reads neither this read's input nor its output: the block that this
        // read left unfinished moves into `block`, and what it wrote joins the history.
        if (d.blockData != nullptr && d.blockData != d.block.data) {
            d.block.clear();
            d.block.append(d.blockData, d.blockSize);
            d.blockData = d.block.data;
        }
        keepHistory(d, output, out.written);
    } else if (status != LzfseResult_Done) {
        d.phase = LzfsePhase_Invalid;
    }
    *consumed = in.at;
    *written = out.written;
    return status;
}

} // namespace pr_impl
