// Brotli decoding, a piece of input at a time.

#include "compress/backends/builtin/src/brotli.h"

#include "pr/alloc.h"
#include "pr/array.h"
#include "pr/assert.h"
#include "pr/mem.h"

#include "compress/backends/builtin/src/brotli_tables.h"

namespace pr_impl {
namespace {

// The longest code of a prefix code (rfc 7932 3.5), and the number of bits of a code that the root of a table indexes.
// A longer code continues in a subtable. The root entry for the first kRootBits bits of the code names that subtable.
constexpr uint32_t kCodeMaxLength = 15;
constexpr uint32_t kRootBits = 8;
constexpr uint32_t kRootSize = 1u << kRootBits;

// The alphabets whose size is fixed (rfc 7932 3.3), and the largest alphabet.
constexpr uint32_t kLiteralAlphabet = 256;
constexpr uint32_t kCommandAlphabet = 704;
constexpr uint32_t kBlockCountAlphabet = 26;
constexpr uint32_t kCodeLengthAlphabet = 18;
constexpr uint32_t kMaxAlphabet = kCommandAlphabet;

// The number of contexts of a block type in the literal context map and in the distance context map (rfc 7932 7.3).
constexpr uint32_t kLiteralContexts = 64;
constexpr uint32_t kDistanceContexts = 4;

// A block count that no meta-block uses up, for a category with one block type (rfc 7932 10).
constexpr uint32_t kBlockCountUnbounded = 1u << 24;

// The capacity of the buffer for the input of an incomplete step. One step reads at most a dozen bytes, so that input
// always fits.
constexpr uint32_t kPendingCapacity = 64;

// The smallest window that a decoder allocates. The window doubles in size as output arrives, up to the window size of
// the stream.
constexpr uint32_t kRingInitialSize = 1u << 12;

// The number of bytes that a copy may write past its end, which are also allocated past the end of the window. A
// back-reference reaches at most 16 bytes less than the window size (rfc 7932 9.1), so these bytes of the window are
// never read.
constexpr uint32_t kCopySlack = 16;

// The block counts that the block count codes encode, and the extra bits that each reads (rfc 7932 6).
constexpr uint32_t kBlockCountBase[kBlockCountAlphabet] = {
    1,   5,   9,   13,  17,  25,  33,   41,   49,   65,   81,   97,   113,
    145, 177, 209, 241, 305, 369, 497, 753, 1265, 2289, 4337, 8433, 16625};
constexpr uint8_t kBlockCountExtra[kBlockCountAlphabet] = {2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4,  4,  5,
                                                           5, 5, 5, 6, 6, 7, 8, 9, 10, 11, 12, 13, 24};

// The insert lengths that the insert length codes encode, and the extra bits that each reads (rfc 7932 5).
constexpr uint32_t kInsertBase[24] = {0,  1,  2,  3,  4,   5,   6,   8,   10,  14,   18,   26,
                                      34, 50, 66, 98, 130, 194, 322, 578, 1090, 2114, 6210, 22594};
constexpr uint8_t kInsertExtra[24] = {0, 0, 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 7, 8, 9, 10, 12, 14, 24};

// The copy lengths that the copy length codes encode, and the extra bits that each reads (rfc 7932 5).
constexpr uint32_t kCopyBase[24] = {2,  3,  4,  5,  6,  7,   8,   9,   10,  12,  14,   18,
                                    22, 30, 38, 54, 70, 102, 134, 198, 326, 582, 1094, 2118};
constexpr uint8_t kCopyExtra[24] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 7, 8, 9, 10, 24};

// The first insert length code and the first copy length code of each range of 64 insert-and-copy length codes (rfc
// 7932 5). The first two ranges reuse the last distance.
constexpr uint8_t kCommandInsert[11] = {0, 0, 0, 0, 8, 8, 0, 16, 8, 16, 16};
constexpr uint8_t kCommandCopy[11] = {0, 8, 0, 8, 0, 8, 16, 0, 16, 8, 16};

// The order in which a complex prefix code stores the code lengths of the code length alphabet (rfc 7932 3.5).
constexpr uint8_t kCodeLengthOrder[kCodeLengthAlphabet] = {1, 2, 3,  4,  0,  5,  17, 6,  16,
                                                          7, 8, 9, 10, 11, 12, 13, 14, 15};

// The code lengths of the code length alphabet are themselves coded with a fixed variable-length code (rfc 7932 3.5).
// These tables give the length and symbol of that code, indexed by the next four bits of the stream.
constexpr uint8_t kLengthCodeLength[16] = {2, 2, 2, 3, 2, 2, 2, 4, 2, 2, 2, 3, 2, 2, 2, 4};
constexpr uint8_t kLengthCodeSymbol[16] = {0, 4, 3, 2, 0, 4, 3, 1, 0, 4, 3, 2, 0, 4, 3, 5};

// The block categories (rfc 7932 2).
enum {
    BrotliCategory_Literal,
    BrotliCategory_Command,
    BrotliCategory_Distance,
    BrotliCategory_Count,
};
typedef uint8_t BrotliCategory;

// An entry in the table of a prefix code. A root entry with nonzero `subBits` names a subtable at `value`, which the
// next `subBits` bits index. Any other entry decodes the symbol `value`, whose code is `length` bits long.
struct PrefixEntry {
    uint16_t value;
    uint8_t length;
    uint8_t subBits;
};

// The tables of a meta-block, by slot. The slots for the block type and block count codes of each category come first.
// Then comes the slot of the context map being read, and then the literal, insert-and-copy length and distance codes.
enum {
    BrotliSlot_TypeCodes = 0,
    BrotliSlot_CountCodes = BrotliSlot_TypeCodes + BrotliCategory_Count,
    BrotliSlot_MapCode = BrotliSlot_CountCodes + BrotliCategory_Count,
    BrotliSlot_Codes,
};
typedef uint32_t BrotliSlot;

// The point that a decoder has reached in the stream.
enum {
    BrotliPhase_StreamHeader,
    BrotliPhase_MetaHeader,
    BrotliPhase_Metadata,           // skips `remaining` bytes of metadata
    BrotliPhase_Uncompressed,       // copies `metaLeft` bytes
    BrotliPhase_TypeCount,          // NBLTYPES of `category`
    BrotliPhase_CountCode,          // starts the block count code of `category`
    BrotliPhase_FirstCount,         // the first block count of `category`
    BrotliPhase_Distances,          // NPOSTFIX and NDIRECT
    BrotliPhase_ContextModes,       // the context mode of literal block type `index`
    BrotliPhase_TreeCount,          // NTREES of the literal map, then of the distance map
    BrotliPhase_MapRle,             // RLEMAX of the context map
    BrotliPhase_MapValues,          // the value of the map at `index`
    BrotliPhase_MapImtf,
    BrotliPhase_Codes,              // starts the prefix code at `index` among the meta-block's codes
    BrotliPhase_CodeHeader,         // the form of a prefix code, and the code length code of a complex code
    BrotliPhase_CodeLengths,        // the code lengths of a complex code
    BrotliPhase_Command,
    BrotliPhase_Literals,           // `insertLeft` literals
    BrotliPhase_Distance,
    BrotliPhase_Copy,               // `copyLeft` bytes from `distance` back
    BrotliPhase_Word,               // the transformed dictionary word in `word`
    BrotliPhase_MetaEnd,
    BrotliPhase_Done,
    BrotliPhase_Invalid,
};
typedef uint8_t BrotliPhase;

// The outcome of a step.
enum {
    BrotliStep_Ok,          // the step was taken, or a byte-wise step made progress
    BrotliStep_Input,       // the input ended inside the step
    BrotliStep_Output,      // the window has no room for the step's output
    BrotliStep_Done,
    BrotliStep_Invalid,
    BrotliStep_Stop,        // the input before `stopAt` is consumed
};
typedef uint8_t BrotliStep;

// The next bits of the stream, first bit lowest. `bits` holds `count` bits loaded from the bytes before `at`. Above
// them, it may hold bits from the bytes at and after `at`, loaded early.
struct BitReader {
    const uint8_t* PR_NULLABLE data;
    size_t size;
    size_t at;
    uint64_t bits;
    uint32_t count;
};

// The caller's output, and how much of it a read has written.
struct BrotliOutput {
    uint8_t* PR_NULLABLE data;
    size_t capacity;
    size_t written;
};

// The reading of a prefix code, resumed across steps.
struct CodeReading {
    uint32_t alphabet;
    BrotliSlot slot;                // where the table goes
    BrotliPhase next;               // the phase after the code
    uint32_t symbol;                // the next symbol whose code length is read
    int32_t space;                  // the code space left, in units of a 15-bit code
    uint32_t repeat;
    uint8_t repeatLength;
    uint8_t previousLength;         // the last nonzero code length
};

struct BrotliState {
    BrotliPhase phase;
    bool isLast;                    // true if the meta-block is the last in the stream
    uint32_t category;
    uint32_t index;

    // The window. The bytes from `flushed` up to `position` have not yet been written to the output. `ring` holds
    // `ringSize` bytes, a power of two that grows to `windowMax`.
    uint8_t* PR_NULLABLE ring;
    uint32_t ringSize;
    uint32_t windowMax;
    uint64_t position;
    uint64_t flushed;

    // The header of the meta-block.
    uint32_t metaLeft;              // the bytes that the meta-block has yet to produce
    uint32_t remaining;             // the metadata bytes left to skip
    uint32_t types[BrotliCategory_Count];
    uint32_t current[BrotliCategory_Count];
    uint32_t previous[BrotliCategory_Count];
    uint32_t blockLeft[BrotliCategory_Count];
    uint32_t postfix;
    uint32_t direct;
    uint8_t modes[256];             // by literal block type
    uint32_t literalTrees;
    uint32_t distanceTrees;
    Array<uint8_t> literalMap;      // for each literal block type and context
    Array<uint8_t> distanceMap;     // for each distance block type and context

    // The context map being read.
    Array<uint8_t>* PR_NULLABLE map;
    uint32_t mapTrees;
    uint32_t rleMax;

    // The prefix code being read, its code lengths, and the table of its code length code.
    CodeReading code;
    uint8_t lengths[kMaxAlphabet];
    PrefixEntry lengthCode[kRootSize];

    // The tables of the meta-block, and the offset of each slot's table within them.
    Array<PrefixEntry> tables;
    Array<uint32_t> slots;          // by slot

    // The command being decoded.
    uint32_t insertLeft;
    uint32_t copyLength;
    uint32_t copyLeft;
    uint32_t distance;
    bool distanceZero;              // true if the command reuses the last distance without a distance code
    uint32_t distances[4];          // the last four distances; the most recent is at `distanceAt`
    uint32_t distanceAt;
    uint8_t word[64];
    uint32_t wordSize;

    // The bits of a partly read byte, and the input of an incomplete step.
    uint64_t bits;
    uint32_t bitCount;
    uint8_t pending[kPendingCapacity];
    uint32_t pendingSize;
};

// Loads bytes until the reader holds at least `count` bits, which is at most 32. Returns false if the input ends first.
inline bool fill(BitReader& r, uint32_t count) {
    if (r.count >= count)
        return true;
    if (r.size - r.at >= 8) {
        r.bits |= readLeU64(r.data + r.at) << r.count;
        const uint32_t added = (63 - r.count) / 8;
        r.at += added;
        r.count += added * 8;
        return true;
    }
    while (r.count < count) {
        if (r.at == r.size)
            return false;
        r.bits |= uint64_t(r.data[r.at++]) << r.count;
        r.count += 8;
    }
    return true;
}

inline void drop(BitReader& r, uint32_t count) {
    r.bits >>= count;
    r.count -= count;
}

inline bool readBits(BitReader& r, uint32_t count, uint32_t& value) {
    if (!fill(r, count))
        return false;
    value = uint32_t(r.bits & ((uint64_t(1) << count) - 1));
    drop(r, count);
    return true;
}

// Returns whole bytes held to the input, leaving the bits of a partly read byte.
void unload(BitReader& r) {
    const uint32_t whole = r.count / 8;
    r.at -= whole;
    r.count -= whole * 8;
    r.bits &= (uint64_t(1) << r.count) - 1;
}

// Returns the number of input bytes that the reader has consumed.
inline size_t consumedOf(const BitReader& r) {
    return r.at - r.count / 8;
}

// Reads a symbol with `table`. Returns false and reads nothing if the input ends before the code of the symbol does.
inline bool readSymbol(BitReader& r, const PrefixEntry* table, uint32_t& symbol) {
    fill(r, kCodeMaxLength);
    PrefixEntry entry = table[r.bits & (kRootSize - 1)];
    if (entry.subBits != 0)
        entry = table[entry.value + ((r.bits >> kRootBits) & ((1u << entry.subBits) - 1))];
    if (entry.length > r.count)
        return false;
    drop(r, entry.length);
    symbol = entry.value;
    return true;
}

// Each byte with its bits in reverse order, built at compile time.
struct ReversedBytes {
    uint8_t bytes[256];
};

constexpr ReversedBytes reversedBytes() {
    ReversedBytes reversed{};
    for (uint32_t value = 0; value < 256; ++value)
        for (uint32_t bit = 0; bit < 8; ++bit)
            reversed.bytes[value] = uint8_t(reversed.bytes[value] | ((value >> bit) & 1) << (7 - bit));
    return reversed;
}

constexpr ReversedBytes kReversed = reversedBytes();

// Returns `value`, which has `count` bits up to 8, with its bits in reverse order.
inline uint32_t reverseBits(uint32_t value, uint32_t count) {
    return uint32_t(kReversed.bytes[value]) >> (8 - count);
}

// The symbols of a canonical prefix code in code order, and the size of its table. The table holds the root and a
// subtable under each root entry that longer codes share. Each subtable is as large as the longest of those codes
// needs.
struct CodeShape {
    uint16_t counts[kCodeMaxLength + 1];    // by code length
    uint16_t sorted[kMaxAlphabet];
    uint8_t longest[kRootSize];             // by root entry
    uint16_t subtables[kRootSize];          // by root entry
    uint32_t size;
};

void shapeCode(CodeShape& shape, const uint8_t* lengths, uint32_t count) {
    memZero(shape.counts, sizeof(shape.counts));
    for (uint32_t symbol = 0; symbol < count; ++symbol)
        ++shape.counts[lengths[symbol]];
    uint16_t starts[kCodeMaxLength + 2]{};
    for (uint32_t length = 1; length <= kCodeMaxLength; ++length)
        starts[length + 1] = uint16_t(starts[length] + shape.counts[length]);
    for (uint32_t symbol = 0; symbol < count; ++symbol)
        if (lengths[symbol] != 0)
            shape.sorted[starts[lengths[symbol]]++] = uint16_t(symbol);

    // Finds the longest code under each root entry. The canonical codes are visited in order, and their lengths do not
    // decrease.
    memZero(shape.longest, sizeof(shape.longest));
    uint32_t code = 0;
    for (uint32_t length = 1; length <= kCodeMaxLength; ++length) {
        if (length <= kRootBits) {
            code += shape.counts[length];
        } else {
            for (uint32_t k = 0; k < shape.counts[length]; ++k, ++code)
                shape.longest[kReversed.bytes[code >> (length - kRootBits)]] = uint8_t(length);
        }
        code <<= 1;
    }
    shape.size = kRootSize;
    for (uint32_t root = 0; root < kRootSize; ++root) {
        if (shape.longest[root] != 0) {
            shape.subtables[root] = uint16_t(shape.size);
            shape.size += 1u << (shape.longest[root] - kRootBits);
        }
    }
}

// Writes the table of a complete code with at least two symbols to `out`, which has `shape.size` entries.
void fillTable(PrefixEntry* out, const CodeShape& shape) {
    for (uint32_t root = 0; root < kRootSize; ++root)
        if (shape.longest[root] != 0)
            out[root] = PrefixEntry{shape.subtables[root], 0, uint8_t(shape.longest[root] - kRootBits)};
    uint32_t code = 0;
    uint32_t index = 0;
    for (uint32_t length = 1; length <= kCodeMaxLength; ++length) {
        for (uint32_t k = 0; k < shape.counts[length]; ++k, ++code, ++index) {
            const PrefixEntry entry{shape.sorted[index], uint8_t(length), 0};
            if (length <= kRootBits) {
                for (uint32_t at = reverseBits(code, length); at < kRootSize; at += 1u << length)
                    out[at] = entry;
                continue;
            }
            const uint32_t root = kReversed.bytes[code >> (length - kRootBits)];
            const uint32_t low = length - kRootBits;
            const uint32_t size = 1u << (shape.longest[root] - kRootBits);
            PrefixEntry* subtable = out + shape.subtables[root];
            for (uint32_t at = reverseBits(code & ((1u << low) - 1), low); at < size; at += 1u << low)
                subtable[at] = entry;
        }
        code <<= 1;
    }
}

// Fills `out`, which has kRootSize entries, with the table of a code with one symbol. The code reads no bits.
void buildSingle(PrefixEntry* out, uint32_t symbol) {
    for (uint32_t at = 0; at < kRootSize; ++at)
        out[at] = PrefixEntry{uint16_t(symbol), 0, 0};
}

// Appends the table of `lengths`, a code of at least two symbols, to the meta-block's tables and records it in `slot`.
void addTable(BrotliState& d, BrotliSlot slot, const uint8_t* lengths, uint32_t count) {
    CodeShape shape;
    shapeCode(shape, lengths, count);
    const uint32_t at = d.tables.count;
    d.tables.resize(at + shape.size);
    fillTable(d.tables.data + at, shape);
    d.slots.data[slot] = at;
}

void addSingle(BrotliState& d, BrotliSlot slot, uint32_t symbol) {
    const uint32_t at = d.tables.count;
    d.tables.resize(at + kRootSize);
    buildSingle(d.tables.data + at, symbol);
    d.slots.data[slot] = at;
}

inline const PrefixEntry* tableOf(const BrotliState& d, BrotliSlot slot) {
    return d.tables.data + d.slots.data[slot];
}

// Returns the number of bits that a simple prefix code spends on a symbol of the alphabet (rfc 7932 3.4).
uint32_t alphabetBits(uint32_t alphabet) {
    uint32_t bits = 0;
    while ((1u << bits) < alphabet)
        ++bits;
    return bits;
}

// Reads the count used for NBLTYPES and NTREES, which takes 1 to 11 bits and ranges from 1 to 256 (rfc 7932 9.2).
bool readCount(BitReader& r, uint32_t& value) {
    uint32_t bit = 0;
    if (!readBits(r, 1, bit))
        return false;
    if (bit == 0) {
        value = 1;
        return true;
    }
    uint32_t width = 0;
    if (!readBits(r, 3, width))
        return false;
    if (width == 0) {
        value = 2;
        return true;
    }
    uint32_t extra = 0;
    if (!readBits(r, width, extra))
        return false;
    value = (1u << width) + extra + 1;
    return true;
}

// Reads a block count with the code in `table` (rfc 7932 6).
bool readBlockCount(BitReader& r, const PrefixEntry* table, uint32_t& value) {
    uint32_t symbol = 0;
    uint32_t extra = 0;
    if (!readSymbol(r, table, symbol) || !readBits(r, kBlockCountExtra[symbol], extra))
        return false;
    value = kBlockCountBase[symbol] + extra;
    return true;
}

// Drops the bits up to the next byte boundary, which must be zero.
bool align(BitReader& r) {
    const uint32_t partial = r.count & 7;
    const bool zero = (r.bits & ((1u << partial) - 1)) == 0;
    drop(r, partial);
    return zero;
}

// Begins reading a prefix code over `alphabet` into `slot`, followed by `next`.
void startCode(BrotliState& d, uint32_t alphabet, BrotliSlot slot, BrotliPhase next) {
    d.code = CodeReading{};
    d.code.alphabet = alphabet;
    d.code.slot = slot;
    d.code.next = next;
    d.phase = BrotliPhase_CodeHeader;
}

// Reads a simple prefix code, or the code length code of a complex code (rfc 7932 3.4, 3.5).
BrotliStep readCodeHeader(BrotliState& d, BitReader& r) {
    CodeReading& code = d.code;
    uint32_t form = 0;
    if (!readBits(r, 2, form))
        return BrotliStep_Input;
    if (form == 1) {
        uint32_t count = 0;
        if (!readBits(r, 2, count))
            return BrotliStep_Input;
        ++count;
        const uint32_t bits = alphabetBits(code.alphabet);
        uint32_t symbols[4];
        for (uint32_t i = 0; i < count; ++i) {
            if (!readBits(r, bits, symbols[i]))
                return BrotliStep_Input;
            if (symbols[i] >= code.alphabet)
                return BrotliStep_Invalid;
            for (uint32_t j = 0; j < i; ++j)
                if (symbols[j] == symbols[i])
                    return BrotliStep_Invalid;
        }
        uint32_t select = 0;
        if (count == 4 && !readBits(r, 1, select))
            return BrotliStep_Input;
        if (count == 1) {
            addSingle(d, code.slot, symbols[0]);
        } else {
            static constexpr uint8_t kSimpleLengths[5][4] = {
                {0, 0, 0, 0}, {0, 0, 0, 0}, {1, 1, 0, 0}, {1, 2, 2, 0}, {2, 2, 2, 2}};
            static constexpr uint8_t kSelectLengths[4] = {1, 2, 3, 3};
            memZero(d.lengths, code.alphabet);
            for (uint32_t i = 0; i < count; ++i)
                d.lengths[symbols[i]] = select ? kSelectLengths[i] : kSimpleLengths[count][i];
            addTable(d, code.slot, d.lengths, code.alphabet);
        }
        d.phase = code.next;
        return BrotliStep_Ok;
    }

    uint8_t lengths[kCodeLengthAlphabet]{};
    int32_t space = 32;
    uint32_t nonzero = 0;
    for (uint32_t i = form; i < kCodeLengthAlphabet && space > 0; ++i) {
        fill(r, 4);
        const uint32_t peek = uint32_t(r.bits & 15);
        if (kLengthCodeLength[peek] > r.count)
            return BrotliStep_Input;
        drop(r, kLengthCodeLength[peek]);
        const uint8_t length = kLengthCodeSymbol[peek];
        lengths[kCodeLengthOrder[i]] = length;
        if (length != 0) {
            space -= 32 >> length;
            ++nonzero;
        }
    }
    if (nonzero != 1 && space != 0)
        return BrotliStep_Invalid;
    if (nonzero == 1) {
        for (uint32_t symbol = 0; symbol < kCodeLengthAlphabet; ++symbol)
            if (lengths[symbol] != 0)
                buildSingle(d.lengthCode, symbol);
    } else {
        // Code length codes are at most 5 bits, so the table is only a root.
        CodeShape shape;
        shapeCode(shape, lengths, kCodeLengthAlphabet);
        fillTable(d.lengthCode, shape);
    }
    code.space = 32768;
    code.previousLength = 8;
    d.phase = BrotliPhase_CodeLengths;
    return BrotliStep_Ok;
}

// Reads as many of the code lengths of a complex code as the input holds, and builds the code after reading the last
// one (rfc 7932 3.5). Returns Ok if it read any.
BrotliStep readCodeLengths(BrotliState& d, BitReader& r) {
    CodeReading& code = d.code;
    bool progress = false;
    while (code.symbol < code.alphabet && code.space > 0) {
        const BitReader before = r;
        uint32_t symbol = 0;
        if (!readSymbol(r, d.lengthCode, symbol))
            return progress ? BrotliStep_Ok : BrotliStep_Input;
        if (symbol < 16) {
            d.lengths[code.symbol++] = uint8_t(symbol);
            code.repeat = 0;
            if (symbol != 0) {
                code.previousLength = uint8_t(symbol);
                code.space -= int32_t(32768u >> symbol);
            }
            progress = true;
            continue;
        }
        const uint32_t extraBits = symbol == 16 ? 2 : 3;
        uint32_t extra = 0;
        if (!readBits(r, extraBits, extra)) {
            r = before;
            return progress ? BrotliStep_Ok : BrotliStep_Input;
        }
        const uint8_t length = symbol == 16 ? code.previousLength : 0;
        if (code.repeatLength != length) {
            code.repeat = 0;
            code.repeatLength = length;
        }
        const uint32_t previous = code.repeat;
        if (code.repeat > 0)
            code.repeat = (code.repeat - 2) << extraBits;
        code.repeat += extra + 3;
        const uint32_t added = code.repeat - previous;
        if (added > code.alphabet - code.symbol)
            return BrotliStep_Invalid;
        memFill(d.lengths + code.symbol, length, added);
        code.symbol += added;
        if (length != 0)
            code.space -= int32_t(added * (32768u >> length));
        progress = true;
    }
    if (code.space != 0)
        return BrotliStep_Invalid;
    memZero(d.lengths + code.symbol, code.alphabet - code.symbol);
    addTable(d, code.slot, d.lengths, code.alphabet);
    d.phase = code.next;
    return BrotliStep_Ok;
}

// Reads a block switch command of `category` (rfc 7932 6).
BrotliStep switchBlock(BrotliState& d, BitReader& r, uint32_t category) {
    uint32_t symbol = 0;
    uint32_t count = 0;
    if (!readSymbol(r, tableOf(d, BrotliSlot_TypeCodes + category), symbol)
        || !readBlockCount(r, tableOf(d, BrotliSlot_CountCodes + category), count))
        return BrotliStep_Input;
    const uint32_t types = d.types[category];
    uint32_t type = symbol - 2;
    if (symbol == 0)
        type = d.previous[category];
    else if (symbol == 1)
        type = d.current[category] + 1 == types ? 0 : d.current[category] + 1;
    d.previous[category] = d.current[category];
    d.current[category] = type;
    d.blockLeft[category] = count;
    return BrotliStep_Ok;
}

// Copies the window from `flushed` to the output, as far as the output has room.
void flush(BrotliState& d, BrotliOutput& out) {
    while (d.flushed < d.position && out.written < out.capacity) {
        const uint32_t at = uint32_t(d.flushed & (d.ringSize - 1));
        size_t count = d.position - d.flushed;
        count = count < d.ringSize - at ? count : d.ringSize - at;
        count = count < out.capacity - out.written ? count : out.capacity - out.written;
        memCopy(out.data + out.written, d.ring + at, count);
        out.written += count;
        d.flushed += count;
    }
}

// Grows the window toward the window size, flushes it to the output, and returns the number of bytes that the window
// has room for, up to `wanted`. Until the window reaches its full size, it grows instead of wrapping, so that its bytes
// stay in order. It grows enough to hold the rest of the meta-block at once.
uint32_t room(BrotliState& d, BrotliOutput& out, uint32_t wanted) {
    if (d.ringSize < d.windowMax && d.position + wanted > d.ringSize) {
        const uint64_t needed = d.position + (wanted > d.metaLeft ? wanted : d.metaLeft);
        uint32_t size = d.ringSize != 0 ? d.ringSize : kRingInitialSize;
        while (size < d.windowMax && size < needed)
            size *= 2;
        size = size < d.windowMax ? size : d.windowMax;
        d.ring = static_cast<uint8_t*>(prMemoryRealloc(d.ring, size + kCopySlack));
        d.ringSize = size;
    }
    if (d.position + wanted - d.flushed > d.ringSize)
        flush(d, out);
    const uint64_t free = d.ringSize - (d.position - d.flushed);
    return free < wanted ? uint32_t(free) : wanted;
}

// Returns the byte `back` bytes before the end of the window, or zero if that is before the start of the stream.
inline uint8_t byteBack(const BrotliState& d, uint32_t back) {
    return d.position >= back ? d.ring[(d.position - back) & (d.ringSize - 1)] : 0;
}

// Applies FermentFirst at offset `at` of the word (rfc 7932 8), and returns the number of bytes in the character that
// it changed.
uint32_t ferment(uint8_t* word, uint32_t size, uint32_t at) {
    if (word[at] < 192) {
        if (word[at] >= 97 && word[at] <= 122)
            word[at] ^= 32;
        return 1;
    }
    if (word[at] < 224) {
        if (at + 1 < size)
            word[at + 1] ^= 32;
        return 2;
    }
    if (at + 2 < size)
        word[at + 2] ^= 5;
    return 3;
}

// Transforms the dictionary word of length `length` that `id` names, and writes it to `word` (rfc 7932 8). Returns
// false if no word has that ID.
bool dictionaryWord(BrotliState& d, uint32_t length, uint32_t id) {
    if (length < kBrotliWordMinLength || length > kBrotliWordMaxLength)
        return false;
    const uint32_t bits = kBrotliDictionaryBits[length];
    const uint32_t transformId = id >> bits;
    if (transformId >= kBrotliTransformCount)
        return false;
    const BrotliTransform& transform = kBrotliTransforms[transformId];
    const uint8_t* base =
        kBrotliDictionary + kBrotliDictionaryOffsets[length] + (id & ((1u << bits) - 1)) * length;
    uint32_t size = 0;
    memCopy(d.word, transform.prefix, transform.prefixSize);
    size += transform.prefixSize;
    uint32_t wordSize = length;
    const BrotliElementary elementary = transform.elementary;
    if (elementary >= BrotliElementary_OmitLast1) {
        const uint32_t omitted = elementary - BrotliElementary_OmitLast1 + 1u;
        wordSize = omitted < length ? length - omitted : 0;
    } else if (elementary >= BrotliElementary_OmitFirst1) {
        const uint32_t omitted = elementary - BrotliElementary_OmitFirst1 + 1u;
        base += omitted < length ? omitted : length;
        wordSize = omitted < length ? length - omitted : 0;
    }
    uint8_t* word = d.word + size;
    memCopy(word, base, wordSize);
    if (elementary == BrotliElementary_FermentFirst && wordSize > 0) {
        ferment(word, wordSize, 0);
    } else if (elementary == BrotliElementary_FermentAll) {
        for (uint32_t at = 0; at < wordSize;)
            at += ferment(word, wordSize, at);
    }
    size += wordSize;
    memCopy(d.word + size, transform.suffix, transform.suffixSize);
    d.wordSize = size + transform.suffixSize;
    return true;
}

// Uses the distance of the command, which is either a back-reference within the window or a dictionary word beyond it
// (rfc 7932 4, 8). A distance code other than zero adds the distance of a back-reference to the last distances.
BrotliStep useDistance(BrotliState& d, uint32_t distance, bool remember) {
    const uint64_t windowSize = d.windowMax - 16;
    const uint64_t reach = d.position < windowSize ? d.position : windowSize;
    if (distance > reach) {
        if (!dictionaryWord(d, d.copyLength, uint32_t(distance - reach - 1)) || d.wordSize > d.metaLeft)
            return BrotliStep_Invalid;
        d.phase = BrotliPhase_Word;
        return BrotliStep_Ok;
    }
    if (d.copyLength > d.metaLeft)
        return BrotliStep_Invalid;
    if (remember)
        d.distances[++d.distanceAt & 3] = distance;
    d.distance = distance;
    d.copyLeft = d.copyLength;
    d.phase = BrotliPhase_Copy;
    return BrotliStep_Ok;
}

// Sets the phase that follows the output of a command: the next command, or the end of the meta-block.
void endCommand(BrotliState& d) {
    d.phase = d.metaLeft == 0 ? BrotliPhase_MetaEnd : BrotliPhase_Command;
}

BrotliStep readMetaHeader(BrotliState& d, BitReader& r) {
    uint32_t isLast = 0;
    uint32_t value = 0;
    if (!readBits(r, 1, isLast))
        return BrotliStep_Input;
    if (isLast) {
        if (!readBits(r, 1, value))
            return BrotliStep_Input;
        if (value) {
            d.isLast = true;
            d.phase = BrotliPhase_MetaEnd;
            return BrotliStep_Ok;
        }
    }
    uint32_t nibbles = 0;
    if (!readBits(r, 2, nibbles))
        return BrotliStep_Input;
    if (nibbles == 3) {
        // A meta-block of metadata, which produces nothing.
        uint32_t reserved = 0;
        uint32_t skipBytes = 0;
        if (!readBits(r, 1, reserved) || !readBits(r, 2, skipBytes))
            return BrotliStep_Input;
        if (reserved != 0)
            return BrotliStep_Invalid;
        uint32_t skip = 0;
        for (uint32_t i = 0; i < skipBytes; ++i) {
            if (!readBits(r, 8, value))
                return BrotliStep_Input;
            if (i + 1 == skipBytes && skipBytes > 1 && value == 0)
                return BrotliStep_Invalid;
            skip |= value << (8 * i);
        }
        if (!align(r))
            return BrotliStep_Invalid;
        d.isLast = isLast != 0;
        d.remaining = skipBytes != 0 ? skip + 1 : 0;
        d.phase = BrotliPhase_Metadata;
        return BrotliStep_Ok;
    }
    nibbles += 4;
    uint32_t length = 0;
    if (!readBits(r, nibbles * 4, length))
        return BrotliStep_Input;
    if (nibbles > 4 && (length >> (nibbles * 4 - 4)) == 0)
        return BrotliStep_Invalid;
    uint32_t uncompressed = 0;
    if (!isLast && !readBits(r, 1, uncompressed))
        return BrotliStep_Input;
    if (uncompressed && !align(r))
        return BrotliStep_Invalid;
    d.isLast = isLast != 0;
    d.metaLeft = length + 1;
    if (uncompressed) {
        d.phase = BrotliPhase_Uncompressed;
        return BrotliStep_Ok;
    }
    d.tables.clear();
    d.slots.resize(BrotliSlot_Codes);
    d.category = 0;
    d.phase = BrotliPhase_TypeCount;
    return BrotliStep_Ok;
}

// Starts reading the next of the literal, insert-and-copy length and distance prefix codes of the meta-block. Once they
// are all read, begins the commands.
void nextCode(BrotliState& d) {
    const uint32_t commandTrees = d.types[BrotliCategory_Command];
    const uint32_t i = d.index++;
    if (i < d.literalTrees) {
        startCode(d, kLiteralAlphabet, BrotliSlot_Codes + i, BrotliPhase_Codes);
    } else if (i < d.literalTrees + commandTrees) {
        startCode(d, kCommandAlphabet, BrotliSlot_Codes + i, BrotliPhase_Codes);
    } else if (i < d.literalTrees + commandTrees + d.distanceTrees) {
        startCode(d, 16 + d.direct + (48u << d.postfix), BrotliSlot_Codes + i, BrotliPhase_Codes);
    } else {
        d.phase = BrotliPhase_Command;
    }
}

// Reads the literal context map, then the distance context map, and then the prefix codes.
void endMap(BrotliState& d) {
    if (d.map == &d.literalMap) {
        d.category = 1;
        d.phase = BrotliPhase_TreeCount;
        return;
    }
    d.slots.resize(BrotliSlot_Codes + d.literalTrees + d.types[BrotliCategory_Command] + d.distanceTrees);
    d.index = 0;
    d.phase = BrotliPhase_Codes;
}

BrotliStep readLiterals(BrotliState& d, BitReader& r, BrotliOutput& out) {
    if (d.insertLeft == 0) {
        if (d.metaLeft == 0)
            d.phase = BrotliPhase_MetaEnd;
        else
            d.phase = BrotliPhase_Distance;
        return BrotliStep_Ok;
    }
    const bool switched = d.types[BrotliCategory_Literal] >= 2;
    if (switched && d.blockLeft[BrotliCategory_Literal] == 0)
        return switchBlock(d, r, BrotliCategory_Literal);
    uint32_t count = room(d, out, d.insertLeft);
    if (count == 0)
        return BrotliStep_Output;
    if (switched && count > d.blockLeft[BrotliCategory_Literal])
        count = d.blockLeft[BrotliCategory_Literal];
    const uint32_t type = d.current[BrotliCategory_Literal];
    const uint8_t* lookup = kBrotliContextLookup + 512 * d.modes[type];
    const uint8_t* map = d.literalMap.data + kLiteralContexts * type;
    const PrefixEntry* tables = d.tables.data;
    const uint32_t* slots = d.slots.data + BrotliSlot_Codes;
    uint8_t* ring = d.ring;
    const uint64_t mask = d.ringSize - 1;
    uint64_t position = d.position;
    uint8_t p1 = byteBack(d, 1);
    uint8_t p2 = byteBack(d, 2);
    uint32_t done = 0;
    for (; done < count; ++done) {
        uint32_t literal = 0;
        if (!readSymbol(r, tables + slots[map[lookup[p1] | lookup[256 + p2]]], literal))
            break;
        ring[position++ & mask] = uint8_t(literal);
        p2 = p1;
        p1 = uint8_t(literal);
    }
    d.position = position;
    d.insertLeft -= done;
    d.metaLeft -= done;
    if (switched)
        d.blockLeft[BrotliCategory_Literal] -= done;
    return done != 0 ? BrotliStep_Ok : BrotliStep_Input;
}

BrotliStep readCommand(BrotliState& d, BitReader& r) {
    const bool switched = d.types[BrotliCategory_Command] >= 2;
    if (switched && d.blockLeft[BrotliCategory_Command] == 0)
        return switchBlock(d, r, BrotliCategory_Command);
    uint32_t symbol = 0;
    uint32_t insertExtra = 0;
    uint32_t copyExtra = 0;
    const BrotliSlot slot = BrotliSlot_Codes + d.literalTrees + d.current[BrotliCategory_Command];
    if (!readSymbol(r, tableOf(d, slot), symbol))
        return BrotliStep_Input;
    const uint32_t range = symbol >> 6;
    const uint32_t insertCode = kCommandInsert[range] + ((symbol >> 3) & 7);
    const uint32_t copyCode = kCommandCopy[range] + (symbol & 7);
    if (!readBits(r, kInsertExtra[insertCode], insertExtra) || !readBits(r, kCopyExtra[copyCode], copyExtra))
        return BrotliStep_Input;
    if (switched)
        --d.blockLeft[BrotliCategory_Command];
    d.insertLeft = kInsertBase[insertCode] + insertExtra;
    d.copyLength = kCopyBase[copyCode] + copyExtra;
    d.distanceZero = range < 2;
    if (d.insertLeft > d.metaLeft)
        return BrotliStep_Invalid;
    d.phase = BrotliPhase_Literals;
    return BrotliStep_Ok;
}

BrotliStep readDistance(BrotliState& d, BitReader& r) {
    if (d.distanceZero)
        return useDistance(d, d.distances[d.distanceAt & 3], false);
    const bool switched = d.types[BrotliCategory_Distance] >= 2;
    if (switched && d.blockLeft[BrotliCategory_Distance] == 0)
        return switchBlock(d, r, BrotliCategory_Distance);
    const uint32_t context = d.copyLength > 4 ? 3 : d.copyLength - 2;
    const uint32_t tree = d.distanceMap.data[kDistanceContexts * d.current[BrotliCategory_Distance] + context];
    const BrotliSlot slot = BrotliSlot_Codes + d.literalTrees + d.types[BrotliCategory_Command] + tree;
    uint32_t code = 0;
    if (!readSymbol(r, tableOf(d, slot), code))
        return BrotliStep_Input;
    int64_t distance = 0;
    if (code < 16) {
        // The last distances, and the last two distances adjusted by up to three (rfc 7932 4).
        static constexpr uint8_t kBack[16] = {0, 1, 2, 3, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1};
        static constexpr int8_t kDelta[16] = {0, 0, 0, 0, -1, 1, -2, 2, -3, 3, -1, 1, -2, 2, -3, 3};
        distance = int64_t(d.distances[(d.distanceAt - kBack[code]) & 3]) + kDelta[code];
        if (distance <= 0)
            return BrotliStep_Invalid;
    } else if (code < 16 + d.direct) {
        distance = code - 15;
    } else {
        const uint32_t offsetCode = code - d.direct - 16;
        const uint32_t bits = 1 + (offsetCode >> (d.postfix + 1));
        uint32_t extra = 0;
        if (!readBits(r, bits, extra))
            return BrotliStep_Input;
        const uint32_t high = offsetCode >> d.postfix;
        const uint32_t low = offsetCode & ((1u << d.postfix) - 1);
        const int64_t offset = (int64_t(2 + (high & 1)) << bits) - 4;
        distance = ((offset + extra) << d.postfix) + low + d.direct + 1;
    }
    if (switched)
        --d.blockLeft[BrotliCategory_Distance];
    if (distance > int64_t(UINT32_MAX))
        return BrotliStep_Invalid;
    return useDistance(d, uint32_t(distance), code != 0);
}

// Copies `count` bytes from `distance` back to `out`, 16 bytes at a time, and may write up to kCopySlack bytes past
// them. A distance under 16 repeats a pattern with period `distance`. After the first 16 bytes, the copy reads from the
// nearest multiple of the period that is at least 16 bytes back.
void copyForward(uint8_t* out, uint32_t distance, uint32_t count) {
    const uint8_t* from = out - distance;
    uint32_t at = 0;
    if (distance >= 16) {
        for (; at < count; at += 16)
            memCopy(out + at, from + at, 16);
        return;
    }
    for (; at < 16; ++at)
        out[at] = from[at];
    const uint32_t step = distance * (16 / distance);
    for (; at < count; at += step) {
        uint8_t chunk[16];
        memCopy(chunk, out + at - step, 16);
        memCopy(out + at, chunk, 16);
    }
}

BrotliStep readCopy(BrotliState& d, BrotliOutput& out) {
    const uint32_t available = room(d, out, d.copyLeft + kCopySlack);
    if (available == 0)
        return BrotliStep_Output;
    uint8_t* ring = d.ring;
    const uint64_t mask = d.ringSize - 1;
    const uint64_t distance = d.distance;
    uint64_t position = d.position;
    uint32_t count = available < d.copyLeft ? available : d.copyLeft;
    const uint32_t to = uint32_t(position & mask);
    const uint32_t from = uint32_t((position - distance) & mask);
    if (available > kCopySlack && from < to) {
        // Neither the source nor the destination wraps, and the bytes past the copy have been flushed.
        count = available - kCopySlack < d.copyLeft ? available - kCopySlack : d.copyLeft;
        count = count < d.ringSize - to ? count : d.ringSize - to;
        copyForward(ring + to, uint32_t(distance), count);
        position += count;
    } else {
        for (uint32_t i = 0; i < count; ++i, ++position)
            ring[position & mask] = ring[(position - distance) & mask];
    }
    d.position = position;
    d.copyLeft -= count;
    d.metaLeft -= count;
    if (d.copyLeft == 0)
        endCommand(d);
    return BrotliStep_Ok;
}

// Copies or skips whole bytes: first those that the reader holds, then those of the input. Returns the number of bytes
// taken.
uint32_t takeBytes(BitReader& r, uint8_t* PR_NULLABLE out, uint32_t wanted) {
    PR_ASSERT((r.count & 7) == 0);
    uint32_t taken = 0;
    while (taken < wanted && r.count != 0) {
        if (out != nullptr)
            out[taken] = uint8_t(r.bits);
        drop(r, 8);
        ++taken;
    }
    if (r.count != 0)
        return taken;
    r.bits = 0;
    size_t available = r.size - r.at;
    const uint32_t count = available < wanted - taken ? uint32_t(available) : wanted - taken;
    if (out != nullptr && count != 0)
        memCopy(out + taken, r.data + r.at, count);
    r.at += count;
    return taken + count;
}

BrotliStep readUncompressed(BrotliState& d, BitReader& r, BrotliOutput& out) {
    if (d.metaLeft == 0) {
        d.phase = BrotliPhase_MetaEnd;
        return BrotliStep_Ok;
    }
    uint32_t count = room(d, out, d.metaLeft);
    if (count == 0)
        return BrotliStep_Output;
    // Takes as much as is contiguous in the window.
    const uint32_t at = uint32_t(d.position & (d.ringSize - 1));
    count = count < d.ringSize - at ? count : d.ringSize - at;
    const uint32_t taken = takeBytes(r, d.ring + at, count);
    d.position += taken;
    d.metaLeft -= taken;
    return taken != 0 ? BrotliStep_Ok : BrotliStep_Input;
}

BrotliStep step(BrotliState& d, BitReader& r, BrotliOutput& out) {
    uint32_t value = 0;
    switch (d.phase) {
        case BrotliPhase_StreamHeader: {
            // WBITS (rfc 7932 9.1).
            uint32_t bits = 16;
            if (!readBits(r, 1, value))
                return BrotliStep_Input;
            if (value) {
                if (!readBits(r, 3, value))
                    return BrotliStep_Input;
                if (value != 0) {
                    bits = 17 + value;
                } else {
                    if (!readBits(r, 3, value))
                        return BrotliStep_Input;
                    if (value == 1)
                        return BrotliStep_Invalid;
                    bits = value == 0 ? 17 : 8 + value;
                }
            }
            d.windowMax = 1u << bits;
            d.phase = BrotliPhase_MetaHeader;
            return BrotliStep_Ok;
        }
        case BrotliPhase_MetaHeader:
            return readMetaHeader(d, r);
        case BrotliPhase_Metadata: {
            if (d.remaining == 0) {
                d.phase = BrotliPhase_MetaEnd;
                return BrotliStep_Ok;
            }
            const uint32_t taken = takeBytes(r, nullptr, d.remaining);
            d.remaining -= taken;
            return taken != 0 ? BrotliStep_Ok : BrotliStep_Input;
        }
        case BrotliPhase_Uncompressed:
            return readUncompressed(d, r, out);
        case BrotliPhase_TypeCount: {
            const uint32_t category = d.category;
            if (!readCount(r, value))
                return BrotliStep_Input;
            d.types[category] = value;
            d.current[category] = 0;
            d.previous[category] = 1;
            if (value >= 2) {
                startCode(d, value + 2, BrotliSlot_TypeCodes + category, BrotliPhase_CountCode);
                return BrotliStep_Ok;
            }
            d.blockLeft[category] = kBlockCountUnbounded;
            if (++d.category == BrotliCategory_Count)
                d.phase = BrotliPhase_Distances;
            return BrotliStep_Ok;
        }
        case BrotliPhase_CountCode:
            startCode(d, kBlockCountAlphabet, BrotliSlot_CountCodes + d.category, BrotliPhase_FirstCount);
            return BrotliStep_Ok;
        case BrotliPhase_FirstCount:
            if (!readBlockCount(r, tableOf(d, BrotliSlot_CountCodes + d.category), value))
                return BrotliStep_Input;
            d.blockLeft[d.category] = value;
            d.phase = ++d.category == BrotliCategory_Count ? BrotliPhase_Distances : BrotliPhase_TypeCount;
            return BrotliStep_Ok;
        case BrotliPhase_Distances: {
            uint32_t postfix = 0;
            uint32_t direct = 0;
            if (!readBits(r, 2, postfix) || !readBits(r, 4, direct))
                return BrotliStep_Input;
            d.postfix = postfix;
            d.direct = direct << postfix;
            d.index = 0;
            d.phase = BrotliPhase_ContextModes;
            return BrotliStep_Ok;
        }
        case BrotliPhase_ContextModes:
            if (d.index == d.types[BrotliCategory_Literal]) {
                d.category = 0;
                d.phase = BrotliPhase_TreeCount;
                return BrotliStep_Ok;
            }
            if (!readBits(r, 2, value))
                return BrotliStep_Input;
            d.modes[d.index++] = uint8_t(value);
            return BrotliStep_Ok;
        case BrotliPhase_TreeCount: {
            if (!readCount(r, value))
                return BrotliStep_Input;
            const bool literal = d.category == 0;
            d.map = literal ? &d.literalMap : &d.distanceMap;
            const uint32_t size = literal ? kLiteralContexts * d.types[BrotliCategory_Literal]
                                          : kDistanceContexts * d.types[BrotliCategory_Distance];
            d.map->resize(size);
            (literal ? d.literalTrees : d.distanceTrees) = value;
            d.mapTrees = value;
            if (value >= 2) {
                d.phase = BrotliPhase_MapRle;
                return BrotliStep_Ok;
            }
            memZero(d.map->data, size);
            endMap(d);
            return BrotliStep_Ok;
        }
        case BrotliPhase_MapRle: {
            // RLEMAX (rfc 7932 7.3).
            uint32_t rleMax = 0;
            if (!readBits(r, 1, value))
                return BrotliStep_Input;
            if (value) {
                if (!readBits(r, 4, rleMax))
                    return BrotliStep_Input;
                ++rleMax;
            }
            d.rleMax = rleMax;
            d.index = 0;
            startCode(d, d.mapTrees + rleMax, BrotliSlot_MapCode, BrotliPhase_MapValues);
            return BrotliStep_Ok;
        }
        case BrotliPhase_MapValues: {
            Array<uint8_t>& map = *d.map;
            if (d.index == map.count) {
                d.phase = BrotliPhase_MapImtf;
                return BrotliStep_Ok;
            }
            uint32_t symbol = 0;
            if (!readSymbol(r, tableOf(d, BrotliSlot_MapCode), symbol))
                return BrotliStep_Input;
            if (symbol == 0) {
                map.data[d.index++] = 0;
            } else if (symbol <= d.rleMax) {
                uint32_t extra = 0;
                if (!readBits(r, symbol, extra))
                    return BrotliStep_Input;
                const uint32_t run = (1u << symbol) + extra;
                if (run > map.count - d.index)
                    return BrotliStep_Invalid;
                memZero(map.data + d.index, run);
                d.index += run;
            } else {
                map.data[d.index++] = uint8_t(symbol - d.rleMax);
            }
            return BrotliStep_Ok;
        }
        case BrotliPhase_MapImtf: {
            if (!readBits(r, 1, value))
                return BrotliStep_Input;
            if (value) {
                // The inverse move-to-front transform (rfc 7932 7.3).
                uint8_t order[256];
                for (uint32_t i = 0; i < 256; ++i)
                    order[i] = uint8_t(i);
                Array<uint8_t>& map = *d.map;
                for (uint32_t i = 0; i < map.count; ++i) {
                    const uint8_t index = map.data[i];
                    const uint8_t moved = order[index];
                    map.data[i] = moved;
                    memMove(order + 1, order, index);
                    order[0] = moved;
                }
            }
            endMap(d);
            return BrotliStep_Ok;
        }
        case BrotliPhase_Codes:
            nextCode(d);
            return BrotliStep_Ok;
        case BrotliPhase_CodeHeader:
            return readCodeHeader(d, r);
        case BrotliPhase_CodeLengths:
            return readCodeLengths(d, r);
        case BrotliPhase_Command:
            return readCommand(d, r);
        case BrotliPhase_Literals:
            return readLiterals(d, r, out);
        case BrotliPhase_Distance:
            return readDistance(d, r);
        case BrotliPhase_Copy:
            return readCopy(d, out);
        case BrotliPhase_Word: {
            if (room(d, out, d.wordSize) < d.wordSize)
                return BrotliStep_Output;
            for (uint32_t i = 0; i < d.wordSize; ++i)
                d.ring[(d.position + i) & (d.ringSize - 1)] = d.word[i];
            d.position += d.wordSize;
            d.metaLeft -= d.wordSize;
            endCommand(d);
            return BrotliStep_Ok;
        }
        case BrotliPhase_MetaEnd:
            if (!d.isLast) {
                d.phase = BrotliPhase_MetaHeader;
                return BrotliStep_Ok;
            }
            // The bits after the last meta-block, to the byte boundary, are zero (rfc 7932 9.2, 9.3).
            if (!align(r))
                return BrotliStep_Invalid;
            d.phase = BrotliPhase_Done;
            return BrotliStep_Ok;
        case BrotliPhase_Done:
            return BrotliStep_Done;
        case BrotliPhase_Invalid:
            return BrotliStep_Invalid;
    }
    return BrotliStep_Invalid;
}

// Takes steps until a step cannot be taken, or until the reader has consumed the input before `stopAt`. A step that
// cannot be taken reads nothing.
BrotliStep decode(BrotliState& d, BitReader& r, BrotliOutput& out, size_t stopAt) {
    for (;;) {
        if (consumedOf(r) >= stopAt)
            return BrotliStep_Stop;
        const BitReader before = r;
        const BrotliStep result = step(d, r, out);
        if (result == BrotliStep_Ok)
            continue;
        if (result != BrotliStep_Done)
            r = before;
        return result;
    }
}

} // namespace

struct Brotli : BrotliState {};

Brotli* brotliCreate(void) {
    Brotli* decoder = allocZeroed<Brotli>();
    brotliReset(decoder);
    return decoder;
}

void brotliReset(Brotli* decoder) {
    BrotliState& d = *decoder;
    d.phase = BrotliPhase_StreamHeader;
    d.isLast = false;
    d.position = d.flushed = 0;
    d.windowMax = 0;
    // The last distances at the start of the stream (rfc 7932 4).
    d.distances[0] = 16;
    d.distances[1] = 15;
    d.distances[2] = 11;
    d.distances[3] = 4;
    d.distanceAt = 3;
    d.bits = 0;
    d.bitCount = 0;
    d.pendingSize = 0;
}

void brotliDestroy(Brotli* PR_NULLABLE decoder) {
    if (decoder == nullptr)
        return;
    prMemoryFree(decoder->ring);
    decoder->literalMap.reset();
    decoder->distanceMap.reset();
    decoder->tables.reset();
    decoder->slots.reset();
    prMemoryFree(decoder);
}

BrotliResult brotliRead(Brotli* decoder, PrByteView bytes, bool final, size_t* consumed,
                        uint8_t* PR_NULLABLE output, size_t capacity, size_t* written) {
    PR_ASSERT(bytes.data != nullptr || bytes.size == 0);
    PR_ASSERT(output != nullptr || capacity == 0);
    BrotliState& d = *decoder;
    BrotliOutput out{output, capacity, 0};
    flush(d, out);
    size_t taken = 0;
    BrotliStep result = BrotliStep_Stop;

    // Decodes the input of an incomplete step, followed by as much of this input as the pending buffer holds, until the
    // steps reach this input.
    if (d.pendingSize != 0 && d.phase != BrotliPhase_Done && d.phase != BrotliPhase_Invalid) {
        const uint32_t held = d.pendingSize;
        const size_t space = kPendingCapacity - held;
        const size_t added = bytes.size < space ? bytes.size : space;
        if (added != 0)
            memCopy(d.pending + held, bytes.data, added);
        BitReader r{d.pending, held + added, 0, d.bits, d.bitCount};
        result = decode(d, r, out, held);
        unload(r);
        if (result == BrotliStep_Input) {
            // The step needs more input than this read gives.
            memMove(d.pending, d.pending + r.at, held + added - r.at);
            d.pendingSize = uint32_t(held + added - r.at);
            taken = added;
        } else if (r.at >= held) {
            d.pendingSize = 0;
            taken = r.at - held;
        } else {
            memMove(d.pending, d.pending + r.at, held - r.at);
            d.pendingSize = uint32_t(held - r.at);
        }
        d.bits = r.bits;
        d.bitCount = r.count;
    }
    if (result == BrotliStep_Stop) {
        BitReader r{bytes.data + taken, bytes.size - taken, 0, d.bits, d.bitCount};
        result = decode(d, r, out, SIZE_MAX);
        unload(r);
        if (result == BrotliStep_Input) {
            const size_t tail = r.size - r.at;
            PR_ASSERT(tail <= kPendingCapacity);
            if (tail != 0)
                memCopy(d.pending, r.data + r.at, tail);
            d.pendingSize = uint32_t(tail);
            taken = bytes.size;
        } else {
            taken += r.at;
        }
        d.bits = r.bits;
        d.bitCount = r.count;
    }
    flush(d, out);

    BrotliResult status = BrotliResult_Invalid;
    switch (result) {
        case BrotliStep_Input:
            status = final ? BrotliResult_Truncated : BrotliResult_Input;
            break;
        case BrotliStep_Output:
            status = BrotliResult_Output;
            break;
        case BrotliStep_Done:
            status = d.flushed == d.position ? BrotliResult_Done : BrotliResult_Output;
            break;
        default:
            status = BrotliResult_Invalid;
            break;
    }
    if (status == BrotliResult_Invalid || status == BrotliResult_Truncated)
        d.phase = BrotliPhase_Invalid;
    *consumed = taken;
    *written = out.written;
    return status;
}

} // namespace pr_impl
