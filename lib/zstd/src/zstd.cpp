// Zstandard decoding, a piece of input at a time.
//
// A block is decoded whole: its literals, Huffman-coded in one or four streams, then its sequences, FSE-coded and read
// backward from the block's end, each a run of literals and a match. A read gathers a compressed block in the decoder
// when its input holds only part of one, and decodes it in place otherwise. Blocks decode into the history, which
// matches read back over, and which a read flushes to the caller's output as room allows. The history keeps the frame's
// window behind the newest block. It grows toward twice that, and then slides its window down to the start.
//
// A first read that has the whole stream decodes into the caller's output instead, where matches read back over the
// output itself, until a block does not fit. The decoder then copies the window into its history and goes on as any
// other read does.

#include "pr/zstd.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/integer.h"
#include "pr/mem.h"
#include "pr/min_max.h"
#include "pr/sort.h"
#include "pr/xxh64.h"

#include "zstd/src/zstd_format.h"

namespace pr_impl {

// What a read needs next, or how the stream ended.
enum {
    ZstdResult_Input,
    ZstdResult_Output,
    ZstdResult_Done,
    ZstdResult_Truncated,
    ZstdResult_Dictionary,      // a frame needs a dictionary
    ZstdResult_Invalid,         // the stream is corrupt, or a frame's checksum does not match its content
};
typedef uint8_t ZstdResult;

// The bytes past a block's output that its copies may write: a run of literals 16 bytes at a time, and then a match's
// slack. The literals have room for a run's reads past their end.
constexpr int64_t kSlack = 32;

// A state of a table of sequence codes: its code's value, `base` plus the next `extra` bits, and the next state, as an
// ZstdFseCell's.
struct ZstdSeqCell {
    uint32_t base;
    uint8_t extra;
    uint8_t bits;
    uint16_t baseline;
};

// Where a decoder is in the stream.
enum {
    ZstdPhase_Magic,
    ZstdPhase_SkipSize,
    ZstdPhase_Skip,             // skips `skip` bytes of a skippable frame
    ZstdPhase_Header,
    ZstdPhase_BlockHeader,
    ZstdPhase_Raw,              // copies `blockLeft` bytes
    ZstdPhase_Rle,
    ZstdPhase_Compressed,
    ZstdPhase_Checksum,
    ZstdPhase_Done,
    ZstdPhase_Failed,           // the stream ended as `failure`
};
typedef uint8_t ZstdPhase;

struct Zstd {
    ZstdPhase phase;
    ZstdResult failure;
    bool direct;                // the history is the caller's output, from base 0
    int64_t windowLimit;
    int64_t frames;             // the frames decoded, skippable ones included

    // The frame.
    int64_t window;
    int64_t contentSize;        // -1 when the header does not give it
    bool hasChecksum;
    int64_t frameStart;         // the position of the frame's first byte
    PrXxh64 checksum;

    // The block.
    bool lastBlock;
    int64_t blockMax;
    int64_t blockSize;
    int64_t blockLeft;          // of a raw block

    // The tables that a block may reuse from the one before: the Huffman code of the literals, whose entries are a
    // symbol and its code's length above it, `huffmanBits` the longest; the sequence codes' tables, by SeqCode, each
    // the decoder's own or a predefined one, and their accuracy logs; and the last three offsets.
    uint32_t huffmanBits;
    bool hasHuffman;
    bool hasSequenceTables;
    uint32_t literalLengthLog;
    uint32_t offsetLog;
    uint32_t matchLengthLog;
    int64_t repeats[3];
    uint16_t huffman[2048];
    const ZstdSeqCell* PR_NULLABLE sequenceTables[3];
    ZstdSeqCell literalLengths[512];
    ZstdSeqCell offsets[256];
    ZstdSeqCell matchLengths[512];

    // The bytes left of a skippable frame, and the bytes of a header or a block that has arrived in part.
    int64_t skip;
    int64_t heldSize;
    uint8_t held[kZstdBlockMax];

    // The block's literals.
    uint8_t literals[kZstdBlockMax + kSlack];

    // The history: its first byte is at `base` in the stream, its last before `position`, and the bytes from `flushed`
    // on are not yet in the output.
    int64_t base;
    int64_t position;
    int64_t flushed;
    uint8_t* PR_NULLABLE history;
    int64_t historySize;
};

namespace {

constexpr uint32_t kMagic = 0xfd2fb528;

// The magic numbers of skippable frames are these sixteen (rfc 8878 3.1.2).
constexpr uint32_t kSkippableMagic = 0x184d2a50;
constexpr uint32_t kSkippableMask = 0xfffffff0;

// The kinds of sequence code.
enum { SeqCode_LiteralLength, SeqCode_Offset, SeqCode_MatchLength };
typedef uint8_t SeqCode;

ZstdSeqCell sequenceCell(SeqCode code, uint8_t symbol, uint8_t bits, uint16_t baseline) {
    switch (code) {
        case SeqCode_LiteralLength:
            return ZstdSeqCell{kZstdLiteralLengthBase[symbol % 36], kZstdLiteralLengthExtra[symbol % 36], bits, baseline};
        case SeqCode_MatchLength:
            return ZstdSeqCell{kZstdMatchLengthBase[symbol % 53], kZstdMatchLengthExtra[symbol % 53], bits, baseline};
        default:
            return ZstdSeqCell{1u << (symbol & 31), symbol, bits, baseline};
    }
}

// Builds the table of a sequence code from `counts` at accuracy `log`.
bool buildSequenceTable(SeqCode code, const int16_t* counts, int64_t symbols, uint32_t log, ZstdSeqCell* table) {
    ZstdFseCell cells[512];
    if (!zstdBuildFse(counts, symbols, log, cells))
        return false;
    for (int64_t u = 0; u < int64_t(1) << log; ++u)
        table[u] = sequenceCell(code, cells[u].symbol, cells[u].bits, cells[u].baseline);
    return true;
}

// Reads the description of the Huffman code of the literals (rfc 8878 4.2.1) and builds its table. Returns the bytes it
// took, or -1.
int64_t readHuffman(Zstd& d, const uint8_t* src, int64_t length) {
    uint8_t weights[256] = {};
    int64_t count = 0;
    int64_t size = zstdReadWeights(src, length, weights, &count);
    if (size < 0)
        return -1;
    // The last weight is the one that completes the code, and its symbol is at most 255.
    if (count > 255)
        return -1;
    uint32_t total = 0;
    uint32_t ranks[13] = {};
    for (int64_t s = 0; s < count; ++s) {
        uint8_t w = weights[s];
        if (w > kZstdHuffmanMaxBits)
            return -1;
        ranks[w] += 1;
        if (w != 0)
            total += 1u << (w - 1);
    }
    if (total == 0)
        return -1;
    uint32_t bits = prU64HighestBit(total) + 1;
    if (bits > kZstdHuffmanMaxBits)
        return -1;
    uint32_t rest = (1u << bits) - total;
    if ((rest & (rest - 1)) != 0)
        return -1;
    uint8_t last = uint8_t(prU64HighestBit(rest) + 1);
    weights[count++] = last;
    ranks[last] += 1;
    if (ranks[1] < 2 || (ranks[1] & 1) != 0)
        return -1;
    // Each weight's codes follow those of the weights below it, in symbol order within a weight.
    uint32_t starts[13] = {};
    uint32_t next = 0;
    for (uint32_t w = 1; w < bits + 1; ++w) {
        starts[w] = next;
        next += ranks[w] << (w - 1);
    }
    for (int64_t s = 0; s < count; ++s) {
        uint8_t w = weights[s];
        if (w == 0)
            continue;
        uint32_t run = 1u << (w - 1);
        uint16_t entry = uint16_t(uint32_t(s) | (bits + 1 - w) << 8);
        for (uint32_t k = 0; k < run; ++k)
            d.huffman[starts[w] + k] = entry;
        starts[w] += run;
    }
    d.huffmanBits = bits;
    d.hasHuffman = true;
    return size;
}

// The symbol of the code that the reader is at, which it takes.
inline uint8_t huffmanSymbol(ZstdBackReader& r, const uint16_t* table, uint32_t bits) {
    uint16_t entry = table[zstdBackPeek(r, bits) & 2047];
    r.consumed += entry >> 8;
    return uint8_t(entry);
}

// Finishes a stream from `at` in the `size` bytes at `out`.
bool huffmanTail(ZstdBackReader& r, const uint16_t* table, uint32_t bits, uint8_t* out, int64_t size, int64_t at) {
    for (int64_t k = at; k < size; ++k) {
        if (zstdBackReload(r) == ZstdReload_Overflow)
            return false;
        out[k] = huffmanSymbol(r, table, bits);
    }
    return zstdBackDone(r);
}

// Decodes one Huffman stream into `out`, whose `size` bytes it must fill: five codes, of at most 11 bits, a reload
// while the stream has bytes to load.
bool huffmanStream(const Zstd& d, const uint8_t* src, int64_t length, uint8_t* out, int64_t size) {
    ZstdBackReader r;
    if (!zstdBackReaderStart(src, length, &r))
        return false;
    uint32_t bits = d.huffmanBits;
    int64_t at = 0;
    while (size - at >= 5 && r.at >= 8) {
        zstdBackReload(r);
        for (int64_t k = 0; k < 5; ++k)
            out[at + k] = huffmanSymbol(r, d.huffman, bits);
        at += 5;
    }
    return huffmanTail(r, d.huffman, bits, out, size, at);
}

// Decodes four Huffman streams into the four parts of `out`, after the stream sizes before them, interleaved while all
// four have input and output left.
bool huffmanStreams(const Zstd& d, const uint8_t* src, int64_t length, uint8_t* out, int64_t size) {
    if (length < 6)
        return false;
    int64_t startTwo = 6 + prMemReadLeU16(src);
    int64_t startThree = startTwo + prMemReadLeU16(src + 2);
    int64_t startFour = startThree + prMemReadLeU16(src + 4);
    if (startFour > length)
        return false;
    int64_t segment = (size + 3) / 4;
    if (3 * segment > size)
        return false;
    ZstdBackReader a;
    ZstdBackReader b;
    ZstdBackReader c;
    ZstdBackReader e;
    if (!zstdBackReaderStart(src + 6, startTwo - 6, &a) || !zstdBackReaderStart(src + startTwo, startThree - startTwo, &b) ||
            !zstdBackReaderStart(src + startThree, startFour - startThree, &c) ||
            !zstdBackReaderStart(src + startFour, length - startFour, &e))
        return false;
    uint32_t bits = d.huffmanBits;
    uint8_t* outA = out;
    uint8_t* outB = out + segment;
    uint8_t* outC = out + 2 * segment;
    uint8_t* outE = out + 3 * segment;
    int64_t sizeE = size - 3 * segment;
    int64_t at = 0;
    // The last part is the shortest.
    while (sizeE - at >= 5 && a.at >= 8 && b.at >= 8 && c.at >= 8 && e.at >= 8) {
        zstdBackReload(a);
        zstdBackReload(b);
        zstdBackReload(c);
        zstdBackReload(e);
        for (int64_t k = 0; k < 5; ++k) {
            outA[at + k] = huffmanSymbol(a, d.huffman, bits);
            outB[at + k] = huffmanSymbol(b, d.huffman, bits);
            outC[at + k] = huffmanSymbol(c, d.huffman, bits);
            outE[at + k] = huffmanSymbol(e, d.huffman, bits);
        }
        at += 5;
    }
    return huffmanTail(a, d.huffman, bits, outA, segment, at) && huffmanTail(b, d.huffman, bits, outB, segment, at) &&
        huffmanTail(c, d.huffman, bits, outC, segment, at) && huffmanTail(e, d.huffman, bits, outE, sizeE, at);
}

// Decodes the literals section of a block (rfc 8878 3.1.1.3.1) into `d.literals`. Returns the bytes it took, or -1.
int64_t readLiterals(Zstd& d, const uint8_t* src, int64_t length, int64_t* literalCount) {
    if (length == 0)
        return -1;
    int64_t b0 = src[0];
    int64_t kind = b0 & 3;
    int64_t format = b0 >> 2 & 3;
    if (kind < 2) {
        // Raw or RLE: a size of 5, 12 or 20 bits.
        int64_t header = 1;
        int64_t size = b0 >> 3;
        if (format == 1) {
            header = 2;
            if (length < 2)
                return -1;
            size = b0 >> 4 | int64_t(src[1]) << 4;
        } else if (format == 3) {
            header = 3;
            if (length < 3)
                return -1;
            size = b0 >> 4 | int64_t(src[1]) << 4 | int64_t(src[2]) << 12;
        }
        if (size > d.blockMax)
            return -1;
        *literalCount = size;
        if (kind == 0) {
            if (header + size > length)
                return -1;
            prMemCopy(d.literals, src + header, size_t(size));
            return header + size;
        }
        if (header + 1 > length)
            return -1;
        prMemFill(d.literals, src[header], size_t(size));
        return header + 1;
    }
    // Compressed, or treeless with the last block's code: sizes of 10, 14 or 18 bits, and one stream or four.
    int64_t header = format < 2 ? 3 : format + 2;
    if (length < header)
        return -1;
    uint64_t field = zstdLeBytes(src, header) >> 4;
    uint64_t width = format < 2 ? 10 : 4 * uint64_t(format) + 6;
    int64_t size = int64_t(field & ((uint64_t(1) << width) - 1));
    int64_t compressed = int64_t(field >> width & ((uint64_t(1) << width) - 1));
    if (size > d.blockMax || header + compressed > length)
        return -1;
    const uint8_t* data = src + header;
    int64_t dataSize = compressed;
    if (kind == 2) {
        int64_t taken = readHuffman(d, data, dataSize);
        if (taken < 0)
            return -1;
        data += taken;
        dataSize -= taken;
    } else if (!d.hasHuffman) {
        return -1;
    }
    if (format == 0 && !huffmanStream(d, data, dataSize, d.literals, size))
        return -1;
    if (format != 0 && !huffmanStreams(d, data, dataSize, d.literals, size))
        return -1;
    *literalCount = size;
    return header + compressed;
}

// What differs between the kinds of sequence code: the number of its codes and the largest accuracy log of its table,
// and its predefined distribution, by SeqCode.
struct SeqCodeKind {
    uint32_t codes;
    uint32_t maxLog;
    const int16_t* defaults;
    uint32_t defaultCodes;
    uint32_t defaultLog;
};
constexpr SeqCodeKind kSeqCodeKinds[3] = {
    {kZstdLiteralLengthCodes, kZstdLiteralLengthLog, kZstdLiteralLengthDefault, 36, 6},
    {kZstdOffsetCodes, kZstdOffsetLog, kZstdOffsetDefault, 29, 5},
    {kZstdMatchLengthCodes, kZstdMatchLengthLog, kZstdMatchLengthDefault, 53, 6},
};

// The tables of the predefined distributions, as large as each code's own, built once.
struct PredefinedTables {
    ZstdSeqCell literalLengths[512];
    ZstdSeqCell offsets[256];
    ZstdSeqCell matchLengths[512];
};

const PredefinedTables& predefinedTables() {
    static const PredefinedTables tables = [] {
        PredefinedTables built{};
        ZstdSeqCell* out[3] = {built.literalLengths, built.offsets, built.matchLengths};
        for (SeqCode code = 0; code < 3; ++code) {
            const SeqCodeKind& kind = kSeqCodeKinds[code];
            buildSequenceTable(code, kind.defaults, kind.defaultCodes, kind.defaultLog, out[code]);
        }
        return built;
    }();
    return tables;
}

// Reads the mode and the table of one sequence code (rfc 8878 3.1.1.3.2.1), from `at` in the `length` bytes at `src`.
// Returns where the next starts, or -1.
int64_t readSequenceTable(Zstd& d, SeqCode code, uint32_t mode, const uint8_t* src, int64_t length, int64_t at) {
    const SeqCodeKind& kind = kSeqCodeKinds[code];
    int64_t symbols = kind.codes;
    ZstdSeqCell* table = code == SeqCode_LiteralLength ? d.literalLengths : d.matchLengths;
    if (code == SeqCode_Offset)
        table = d.offsets;
    uint32_t log = 0;
    int64_t next = at;
    if (mode == 0) {
        // Predefined.
        const PredefinedTables& predefined = predefinedTables();
        const ZstdSeqCell* tables[3] = {predefined.literalLengths, predefined.offsets, predefined.matchLengths};
        d.sequenceTables[code] = tables[code];
        log = kind.defaultLog;
    } else if (mode == 1) {
        // RLE: one symbol, which reads no bits.
        if (at >= length || src[at] >= symbols)
            return -1;
        table[0] = sequenceCell(code, src[at], 0, 0);
        d.sequenceTables[code] = table;
        next = at + 1;
    } else if (mode == 2) {
        int16_t counts[53];
        uint32_t tableLog = 0;
        int64_t taken = zstdReadFseCounts(src + at, length - at, counts, symbols, kind.maxLog, &tableLog);
        if (taken < 0 || !buildSequenceTable(code, counts, symbols, tableLog, table))
            return -1;
        d.sequenceTables[code] = table;
        log = tableLog;
        next = at + taken;
    } else {
        // Repeat: the last block's.
        return d.hasSequenceTables ? at : -1;
    }
    if (code == SeqCode_LiteralLength)
        d.literalLengthLog = log;
    else if (code == SeqCode_Offset)
        d.offsetLog = log;
    else
        d.matchLengthLog = log;
    return next;
}

// Copies a match of `length` bytes from `distance` back, to `out`. The copy runs forward, so a match longer than its
// distance repeats a pattern. It copies eight bytes at a time and may write up to 8 bytes past the match. A match
// nearer than eight bytes is a pattern with period `distance`. After its first eight bytes, it copies from the nearest
// multiple of the period that is at least eight bytes back.
inline void copyMatch(uint8_t* out, uint32_t distance, uint32_t length) {
    uint32_t at = 0;
    const uint8_t* from = out - distance;
    if (distance >= 8) {
        for (; at < length; at += 8)
            prMemCopy(out + at, from + at, 8);
        return;
    }
    for (; at < 8; ++at)
        out[at] = from[at];
    // The eight bytes read may overlap the eight written, so they pass through a register.
    const uint32_t step = distance * (8 / distance);
    for (; at < length; at += step) {
        uint64_t word = 0;
        prMemCopy(&word, out + (at - step), 8);
        prMemCopy(out + at, &word, 8);
    }
}

// Copies 16 bytes, through a local, which the compiler makes one load and one store.
inline void copy16(uint8_t* to, const uint8_t* from) {
    uint8_t chunk[16];
    prMemCopy(chunk, from, 16);
    prMemCopy(to, chunk, 16);
}

// Copies `count` literals: 16 bytes, and then 32 at a time, reading and writing up to 31 bytes past them.
inline void copyLiterals(uint8_t* to, const uint8_t* from, int64_t count) {
    copy16(to, from);
    for (int64_t k = 16; k < count; k += 32) {
        copy16(to + k, from + k);
        copy16(to + k + 16, from + k + 16);
    }
}

// Copies a match of `length` bytes from `offset` back, to `to`, as copyMatch does, and for an offset of 16 or more as
// the literals are, writing up to 31 bytes past the match. A load of 16 bytes then reads only bytes already written.
inline void copySequenceMatch(uint8_t* to, int64_t offset, int64_t length) {
    if (offset < 16) {
        copyMatch(to, uint32_t(offset), uint32_t(length));
        return;
    }
    const uint8_t* from = to - offset;
    copy16(to, from);
    for (int64_t k = 16; k < length; k += 32) {
        copy16(to + k, from + k);
        copy16(to + k + 16, from + k + 16);
    }
}

// What readSequences returns for a block whose output does not fit.
constexpr int64_t kNoRoom = -2;

// Decodes the sequences section of a block (rfc 8878 3.1.1.3.2) and executes them, with `literalCount` literals,
// writing the block's output from `start` in the `outSize` bytes at `out`, where a match may reach `reach` bytes back
// before it. Returns where the output ends, -1 for a block that is corrupt, or kNoRoom if `out` ends first.
int64_t readSequences(Zstd& d, const uint8_t* src, int64_t length, int64_t literalCount, uint8_t* out, int64_t outSize,
        int64_t start, int64_t reach) {
    if (length == 0)
        return -1;
    int64_t limit = start + d.blockMax;
    int64_t b0 = src[0];
    int64_t count = b0;
    int64_t at = 1;
    if (b0 == 0) {
        if (length != 1 || literalCount > d.blockMax)
            return -1;
        if (literalCount > outSize - start)
            return kNoRoom;
        if (literalCount != 0)
            prMemCopy(out + start, d.literals, size_t(literalCount));
        return start + literalCount;
    }
    if (b0 == 255) {
        if (length < 3)
            return -1;
        count = prMemReadLeU16(src + 1) + 0x7f00;
        at = 3;
    } else if (b0 >= 128) {
        if (length < 2)
            return -1;
        count = (b0 - 128) << 8 | src[1];
        at = 2;
    }
    if (at >= length)
        return -1;
    uint32_t modes = src[at++];
    if ((modes & 3) != 0)
        return -1;
    at = readSequenceTable(d, SeqCode_LiteralLength, modes >> 6, src, length, at);
    if (at < 0)
        return -1;
    at = readSequenceTable(d, SeqCode_Offset, modes >> 4 & 3, src, length, at);
    if (at < 0)
        return -1;
    at = readSequenceTable(d, SeqCode_MatchLength, modes >> 2 & 3, src, length, at);
    if (at < 0)
        return -1;
    d.hasSequenceTables = true;

    ZstdBackReader r;
    if (!zstdBackReaderStart(src + at, length - at, &r))
        return -1;
    const uint8_t* literals = d.literals;
    const ZstdSeqCell* literalLengths = d.sequenceTables[SeqCode_LiteralLength];
    const ZstdSeqCell* offsets = d.sequenceTables[SeqCode_Offset];
    const ZstdSeqCell* matchLengths = d.sequenceTables[SeqCode_MatchLength];
    int64_t llState = int64_t(zstdBackRead(r, d.literalLengthLog));
    int64_t ofState = int64_t(zstdBackRead(r, d.offsetLog));
    int64_t mlState = int64_t(zstdBackRead(r, d.matchLengthLog));
    int64_t rep0 = d.repeats[0];
    int64_t rep1 = d.repeats[1];
    int64_t rep2 = d.repeats[2];
    // A sequence ending before `fast` copies with slack; one past it, at the end of the block or of `out`, copies
    // exactly.
    int64_t fast = min(limit, outSize - kSlack);
    int64_t origin = start - reach;
    int64_t op = start;
    int64_t lit = 0;
    for (int64_t i = 0; i < count; ++i) {
        // A reload holds at least 56 bits: the extra bits of all three codes and the bits of the next states, unless
        // the extra bits are 31 or more, when the reader reloads before the literal length's.
        zstdBackReload(r);
        ZstdSeqCell lc = literalLengths[llState & 511];
        ZstdSeqCell oc = offsets[ofState & 255];
        ZstdSeqCell mc = matchLengths[mlState & 511];
        int64_t offsetValue = oc.base + int64_t(zstdBackRead(r, oc.extra));
        int64_t matchLength = mc.base + int64_t(zstdBackRead(r, mc.extra));
        if (oc.extra + mc.extra + lc.extra >= 31)
            zstdBackReload(r);
        int64_t literalLength = lc.base + int64_t(zstdBackRead(r, lc.extra));
        if (i + 1 != count) {
            llState = lc.baseline + int64_t(zstdBackRead(r, lc.bits));
            mlState = mc.baseline + int64_t(zstdBackRead(r, mc.bits));
            ofState = oc.baseline + int64_t(zstdBackRead(r, oc.bits));
        }
        // The offset, or one of the last three (rfc 8878 3.1.1.5).
        int64_t offset = 0;
        if (offsetValue > 3) {
            offset = offsetValue - 3;
            rep2 = rep1;
            rep1 = rep0;
            rep0 = offset;
        } else {
            int64_t which = offsetValue - 1 + (literalLength == 0 ? 1 : 0);
            if (which == 0) {
                offset = rep0;
            } else {
                offset = which == 1 ? rep1 : which == 2 ? rep2 : rep0 - 1;
                if (which != 1)
                    rep2 = rep1;
                rep1 = rep0;
                rep0 = offset;
            }
        }
        if (literalLength > literalCount - lit)
            return -1;
        int64_t end = op + literalLength + matchLength;
        if (end <= fast) {
            copyLiterals(out + op, literals + lit, literalLength);
            op += literalLength;
            lit += literalLength;
            if (offset <= 0 || offset > op - origin)
                return -1;
            copySequenceMatch(out + op, offset, matchLength);
        } else {
            if (end > limit)
                return -1;
            if (end > outSize)
                return kNoRoom;
            if (literalLength != 0)
                prMemCopy(out + op, literals + lit, size_t(literalLength));
            op += literalLength;
            lit += literalLength;
            if (offset <= 0 || offset > op - origin)
                return -1;
            for (int64_t k = op; k < end; ++k)
                out[k] = out[k - offset];
        }
        op = end;
    }
    if (!zstdBackDone(r))
        return -1;
    int64_t rest = literalCount - lit;
    if (rest > limit - op)
        return -1;
    if (rest > outSize - op)
        return kNoRoom;
    if (rest != 0)
        prMemCopy(out + op, literals + lit, size_t(rest));
    d.repeats[0] = rep0;
    d.repeats[1] = rep1;
    d.repeats[2] = rep2;
    return op + rest;
}

// Decodes a compressed block (rfc 8878 3.1.1.3) from `start` in `out`, as readSequences does. A block that does not fit
// changes nothing that decoding it again would not change in the same way.
int64_t decodeBlock(Zstd& d, const uint8_t* body, int64_t length, uint8_t* out, int64_t outSize, int64_t start) {
    int64_t literalCount = 0;
    int64_t taken = readLiterals(d, body, length, &literalCount);
    if (taken < 0)
        return -1;
    int64_t reach = min(d.window, d.position - d.frameStart);
    return readSequences(d, body + taken, length - taken, literalCount, out, outSize, start, reach);
}

// Gives the history `size` bytes, keeping what it holds.
void resizeHistory(Zstd& d, int64_t size) {
    uint8_t* history = static_cast<uint8_t*>(prMemoryAlloc(size_t(size)));
    if (d.historySize != 0)
        prMemCopy(history, d.history, size_t(min(size, d.historySize)));
    prMemoryFree(d.history);
    d.history = history;
    d.historySize = size;
}

// Makes room in the history for a block's output and its slack: by sliding the window down to the start once that frees
// half of it, and otherwise by growing it.
void makeRoom(Zstd& d) {
    int64_t end = d.position - d.base;
    if (end + d.blockMax + kSlack <= d.historySize)
        return;
    // What lies before the window and has been flushed.
    int64_t drop = min(end - min(end, d.window), d.flushed - d.base);
    if (drop > 0 && 2 * drop >= d.historySize) {
        prMemMove(d.history, d.history + drop, size_t(end - drop));
        d.base += drop;
    }
    int64_t needed = d.position - d.base + d.blockMax + kSlack;
    if (needed > d.historySize)
        resizeHistory(d, max(needed, 2 * d.historySize));
}

// Copies the history from `flushed` to the output, as far as it has room.
void flush(Zstd& d, uint8_t* PR_NULLABLE output, int64_t capacity, int64_t* written) {
    int64_t count = min(d.position - d.flushed, capacity - *written);
    if (count != 0)
        prMemCopy(output + *written, d.history + (d.flushed - d.base), size_t(count));
    *written += count;
    d.flushed += count;
}

// Makes `need` bytes of a header or a block ready: those of the input from `*at` when the decoder holds none of them,
// or else those it gathers. Returns the bytes, or null if the input ends first.
const uint8_t* PR_NULLABLE gather(Zstd& d, PrByteView input, int64_t* at, int64_t need) {
    static const uint8_t kNone[1] = {};
    int64_t size = int64_t(input.size);
    if (d.heldSize == 0 && size - *at >= need) {
        const uint8_t* unit = need != 0 ? input.data + *at : kNone;
        *at += need;
        return unit;
    }
    int64_t count = min(need - d.heldSize, size - *at);
    if (count != 0)
        prMemCopy(d.held + d.heldSize, input.data + *at, size_t(count));
    d.heldSize += count;
    *at += count;
    if (d.heldSize < need)
        return nullptr;
    d.heldSize = 0;
    return d.held;
}

// The fields of a frame header (rfc 8878 3.1.1.1). The content size is -1 where the header does not give it.
struct FrameHeader {
    int64_t window;
    int64_t contentSize;
    bool hasChecksum;
    uint32_t dictionary;
};

// The size of a frame header, from its first byte.
int64_t frameHeaderSize(uint8_t descriptor) {
    bool single = (descriptor & 32) != 0;
    const int64_t idSizes[4] = {0, 1, 2, 4};
    const int64_t contentSizes[4] = {single ? 1 : 0, 2, 4, 8};
    return 1 + (single ? 0 : 1) + idSizes[descriptor & 3] + contentSizes[descriptor >> 6];
}

// Reads a frame header of frameHeaderSize bytes. Returns false for one that is corrupt.
bool parseFrameHeader(const uint8_t* header, int64_t size, FrameHeader* h) {
    *h = FrameHeader{0, -1, false, 0};
    uint8_t descriptor = header[0];
    bool single = (descriptor & 32) != 0;
    if ((descriptor & 8) != 0)
        return false;
    int64_t k = 1;
    if (!single) {
        int64_t base = int64_t(1) << (10 + (header[1] >> 3));
        h->window = base + (base >> 3) * (header[1] & 7);
        k = 2;
    }
    uint32_t flag = descriptor >> 6;
    int64_t contentBytes = flag != 0 ? int64_t(1) << flag : single ? 1 : 0;
    int64_t idBytes = size - k - contentBytes;
    h->dictionary = uint32_t(zstdLeBytes(header + k, idBytes));
    k += idBytes;
    if (contentBytes != 0) {
        uint64_t value = zstdLeBytes(header + k, contentBytes);
        if (value >= uint64_t(1) << 62)
            return false;
        h->contentSize = int64_t(value) + (contentBytes == 2 ? 256 : 0);
    }
    if (single)
        h->window = h->contentSize;
    h->hasChecksum = (descriptor & 4) != 0;
    return true;
}

// Reads a frame header and starts the frame.
ZstdResult readFrameHeader(Zstd& d, PrByteView input, int64_t* at) {
    if (d.heldSize == 0 && *at == int64_t(input.size))
        return ZstdResult_Input;
    uint8_t descriptor = d.heldSize != 0 ? d.held[0] : input.data[*at];
    int64_t size = frameHeaderSize(descriptor);
    const uint8_t* bytes = gather(d, input, at, size);
    if (bytes == nullptr)
        return ZstdResult_Input;
    FrameHeader h;
    if (!parseFrameHeader(bytes, size, &h))
        return ZstdResult_Invalid;
    if (h.dictionary != 0)
        return ZstdResult_Dictionary;
    if (h.window > d.windowLimit)
        return ZstdResult_Invalid;
    d.window = h.window;
    d.contentSize = h.contentSize;
    d.hasChecksum = h.hasChecksum;
    d.checksum = prXxh64Start(0);
    d.frameStart = d.position;
    d.blockMax = min(d.window, kZstdBlockMax);
    d.repeats[0] = 1;
    d.repeats[1] = 4;
    d.repeats[2] = 8;
    d.hasHuffman = false;
    d.hasSequenceTables = false;
    d.phase = ZstdPhase_BlockHeader;
    return ZstdResult_Done;
}

// Ends a block that wrote the `size` bytes at `produced`: checks the frame's size, and moves to the next block or to
// the frame's end.
bool endBlock(Zstd& d, const uint8_t* produced, int64_t size) {
    prXxh64Update(&d.checksum, PrByteView{produced, size_t(size)});
    d.position += size;
    int64_t content = d.position - d.frameStart;
    if (d.contentSize >= 0 && content > d.contentSize)
        return false;
    if (!d.lastBlock) {
        d.phase = ZstdPhase_BlockHeader;
        return true;
    }
    if (d.contentSize >= 0 && content != d.contentSize)
        return false;
    d.phase = d.hasChecksum ? ZstdPhase_Checksum : ZstdPhase_Magic;
    if (!d.hasChecksum)
        d.frames += 1;
    return true;
}

// Moves a decoder from writing to the caller's output to writing to its history: copies the window there, all of it
// flushed.
void leaveDirect(Zstd& d, const uint8_t* PR_NULLABLE output) {
    int64_t keep = min(d.window, d.position - d.frameStart);
    if (d.historySize < keep)
        resizeHistory(d, keep);
    if (keep != 0)
        prMemCopy(d.history, output + (d.position - keep), size_t(keep));
    d.base = d.position - keep;
    d.flushed = d.position;
    d.direct = false;
    makeRoom(d);
}

ZstdResult readFrames(Zstd& d, PrByteView input, int64_t* at, bool final, uint8_t* PR_NULLABLE output, int64_t capacity,
        int64_t* written) {
    int64_t inputSize = int64_t(input.size);
    for (;;) {
        if (d.direct) {
            *written = d.position;
        } else {
            flush(d, output, capacity, written);
            if (d.flushed != d.position)
                return ZstdResult_Output;
        }
        switch (d.phase) {
            case ZstdPhase_Magic: {
                if (d.frames != 0 && d.heldSize == 0 && *at == inputSize) {
                    if (!final)
                        return ZstdResult_Input;
                    d.phase = ZstdPhase_Done;
                    continue;
                }
                const uint8_t* bytes = gather(d, input, at, 4);
                if (bytes == nullptr)
                    return ZstdResult_Input;
                uint32_t magic = prMemReadLeU32(bytes);
                if (magic == kMagic)
                    d.phase = ZstdPhase_Header;
                else if ((magic & kSkippableMask) == kSkippableMagic)
                    d.phase = ZstdPhase_SkipSize;
                else
                    return ZstdResult_Invalid;
                break;
            }
            case ZstdPhase_SkipSize: {
                const uint8_t* bytes = gather(d, input, at, 4);
                if (bytes == nullptr)
                    return ZstdResult_Input;
                d.skip = prMemReadLeU32(bytes);
                d.phase = ZstdPhase_Skip;
                break;
            }
            case ZstdPhase_Skip: {
                int64_t count = min(d.skip, inputSize - *at);
                *at += count;
                d.skip -= count;
                if (d.skip != 0)
                    return ZstdResult_Input;
                d.frames += 1;
                d.phase = ZstdPhase_Magic;
                break;
            }
            case ZstdPhase_Header: {
                ZstdResult result = readFrameHeader(d, input, at);
                if (result != ZstdResult_Done)
                    return result;
                break;
            }
            case ZstdPhase_BlockHeader: {
                const uint8_t* bytes = gather(d, input, at, 3);
                if (bytes == nullptr)
                    return ZstdResult_Input;
                uint32_t header = prMemReadLeU16(bytes) | uint32_t(bytes[2]) << 16;
                d.lastBlock = (header & 1) != 0;
                d.blockSize = header >> 3;
                if (d.blockSize > d.blockMax)
                    return ZstdResult_Invalid;
                if (!d.direct)
                    makeRoom(d);
                uint32_t type = header >> 1 & 3;
                if (type == 3)
                    return ZstdResult_Invalid;
                d.phase = type == 0 ? ZstdPhase_Raw : type == 1 ? ZstdPhase_Rle : ZstdPhase_Compressed;
                d.blockLeft = d.blockSize;
                break;
            }
            case ZstdPhase_Raw: {
                if (d.direct && d.blockLeft > capacity - d.position)
                    leaveDirect(d, output);
                // As the bytes arrive.
                uint8_t* out = d.direct ? output : d.history;
                int64_t count = min(d.blockLeft, inputSize - *at);
                int64_t to = d.position - d.base;
                if (count != 0)
                    prMemCopy(out + to, input.data + *at, size_t(count));
                *at += count;
                d.blockLeft -= count;
                if (d.blockLeft != 0) {
                    prXxh64Update(&d.checksum, PrByteView{out + to, size_t(count)});
                    d.position += count;
                    return ZstdResult_Input;
                }
                if (!endBlock(d, out + to, count))
                    return ZstdResult_Invalid;
                break;
            }
            case ZstdPhase_Rle: {
                const uint8_t* bytes = gather(d, input, at, 1);
                if (bytes == nullptr)
                    return ZstdResult_Input;
                uint8_t value = bytes[0];
                if (d.direct && d.blockSize > capacity - d.position)
                    leaveDirect(d, output);
                uint8_t* out = d.direct ? output : d.history;
                int64_t to = d.position - d.base;
                if (d.blockSize != 0)
                    prMemFill(out + to, value, size_t(d.blockSize));
                if (!endBlock(d, out + to, d.blockSize))
                    return ZstdResult_Invalid;
                break;
            }
            case ZstdPhase_Compressed: {
                const uint8_t* body = gather(d, input, at, d.blockSize);
                if (body == nullptr)
                    return ZstdResult_Input;
                uint8_t* out = d.direct ? output : d.history;
                int64_t outSize = d.direct ? capacity : d.historySize;
                int64_t start = d.position - d.base;
                int64_t end = decodeBlock(d, body, d.blockSize, out, outSize, start);
                if (end == kNoRoom && d.direct) {
                    // Again, into the history. The whole input is there, so the block came from it rather than being
                    // gathered.
                    *at -= d.blockSize;
                    leaveDirect(d, output);
                } else if (end < 0 || !endBlock(d, out + start, end - start)) {
                    return ZstdResult_Invalid;
                }
                break;
            }
            case ZstdPhase_Checksum: {
                const uint8_t* bytes = gather(d, input, at, 4);
                if (bytes == nullptr)
                    return ZstdResult_Input;
                if (prMemReadLeU32(bytes) != uint32_t(prXxh64Digest(&d.checksum)))
                    return ZstdResult_Invalid;
                d.frames += 1;
                d.phase = ZstdPhase_Magic;
                break;
            }
            case ZstdPhase_Done:
                return ZstdResult_Done;
            case ZstdPhase_Failed:
                return d.failure;
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Encoding
//
// The input is cut into blocks of 128K, and each block into sequences, a run of literals and a match each, by the match
// finder of its level: a hash table of 6-byte strings for Fast, as the reference encoder's level 1; one of 8-byte and
// one of 5-byte strings for Default, as its level 3; and hash chains of 5-byte strings searched 16 deep for Small, each
// match deferred while one of the next two positions has a better one, as its level 9. A block's literals are
// Huffman-coded, and the codes of its sequences FSE-coded with whichever of the predefined tables, one symbol, or
// tables of their own takes the fewest bits. A block that this does not make smaller is stored, or written as one byte
// repeated.

// How a level finds matches.
enum {
    Strategy_Fast,      // one hash table
    Strategy_Double,    // a table of long strings and one of short strings
    Strategy_Lazy,      // hash chains, each match deferred while a later one is better
};
typedef uint8_t Strategy;

// A level's parameters: its window, the bits of its two tables (the second is Default's table of short strings, or
// Small's chains), the bytes of the strings that the first hashes, and the positions that Small tries.
struct Effort {
    Strategy strategy;
    uint32_t windowLog;
    uint32_t hashLog;
    uint32_t chainLog;
    uint32_t hashed;
    uint32_t search;
};

constexpr Effort kEfforts[] = {
    {Strategy_Fast, 19, 14, 0, 6, 0},       // PrZstdLevel_Fast
    {Strategy_Double, 21, 17, 16, 8, 0},    // PrZstdLevel_Default
    {Strategy_Lazy, 22, 21, 20, 5, 16},     // PrZstdLevel_Small
};

inline uint32_t highBit(uint64_t value) {
    return prU64HighestBit(value);
}

// The level's effort for an input of `size` bytes, with tables no larger than an input that fits them.
Effort effortOf(PrZstdLevel level, int64_t size) {
    Effort e = kEfforts[level];
    if (size < int64_t(1) << 22) {
        uint32_t bits = max(highBit(uint64_t(max(size, int64_t(1)))) + 1, 6u);
        e.hashLog = min(e.hashLog, bits);
        e.chainLog = min(e.chainLog, bits);
    }
    return e;
}

// The codes of literal lengths below 64, and of match lengths below 131 by the length less 3 (rfc 8878 3.1.1.3.2.1.1).
struct LengthCodes {
    uint8_t literal[64];
    uint8_t match[128];
};

constexpr LengthCodes lengthCodes() {
    LengthCodes codes{};
    for (uint32_t code = 0; code < kZstdLiteralLengthCodes; ++code) {
        for (uint32_t value = kZstdLiteralLengthBase[code];
                value < kZstdLiteralLengthBase[code] + (1u << kZstdLiteralLengthExtra[code]) && value < 64; ++value)
            codes.literal[value] = uint8_t(code);
    }
    for (uint32_t code = 0; code < kZstdMatchLengthCodes; ++code) {
        for (uint32_t value = kZstdMatchLengthBase[code];
                value < kZstdMatchLengthBase[code] + (1u << kZstdMatchLengthExtra[code]) && value - 3 < 128; ++value)
            codes.match[value - 3] = uint8_t(code);
    }
    return codes;
}

constexpr LengthCodes kLengthCodes = lengthCodes();

inline uint32_t literalLengthCode(uint32_t length) {
    return length < 64 ? kLengthCodes.literal[length] : highBit(length) + 19;
}

inline uint32_t matchLengthCode(uint32_t length) {
    uint32_t base = length - 3;
    return base < 128 ? kLengthCodes.match[base] : highBit(base) + 36;
}

// log2 of 1 to 512 in 256ths of a bit, rounded, for the costs of FSE codes.
struct Log2Table {
    uint32_t values[513];
};

constexpr Log2Table log2Table() {
    Log2Table t{};
    for (uint32_t x = 1; x <= 512; ++x) {
        // The whole bits, then the fraction one bit at a time by squaring.
        uint32_t whole = 0;
        while ((x >> (whole + 1)) != 0)
            ++whole;
        double m = double(x) / double(1u << whole);
        double fraction = 0;
        double bit = 0.5;
        for (int k = 0; k < 24; ++k, bit /= 2) {
            m *= m;
            if (m >= 2) {
                m /= 2;
                fraction += bit;
            }
        }
        t.values[x] = uint32_t((double(whole) + fraction) * 256 + 0.5);
    }
    return t;
}

constexpr Log2Table kLog2 = log2Table();

// The most sequences a block holds: a match is at least three bytes.
constexpr int64_t kSequences = kZstdBlockMax / 3 + 1;

// The bytes past the literals that a copy of them may write.
constexpr int64_t kLiteralSlack = 16;

// An encoder: the input, its level and window, the last three offsets as a decoder will have them, the literals and
// sequences of the block, and the match finder's tables, by position modulo 2^32.
struct Encoder {
    const uint8_t* input;
    int64_t size;
    Effort effort;
    int64_t window;
    int64_t repeats[3];
    int64_t nextInsert;         // Small: the first position not yet in the chains
    int64_t literalCount;
    int64_t sequenceCount;
    uint8_t* literals;
    uint32_t* literalLengths;
    uint32_t* matchLengths;
    uint32_t* offsetValues;
    uint8_t* codes;             // by sequence, its literal length, offset and match length codes
    uint32_t* tables;
};

// The position before `position` that `stored` records, from its low 32 bits, which a table keeps. A position from 2^32
// or more back may be read as a nearer one, whose bytes are then checked as any candidate's are.
inline int64_t earlier(int64_t position, uint32_t stored) {
    return position - int64_t(uint32_t(uint32_t(position) - stored));
}

inline uint64_t load64(const uint8_t* input, int64_t at) {
    return prMemReadLeU64(input + at);
}

inline uint32_t load32(const uint8_t* input, int64_t at) {
    return prMemReadLeU32(input + at);
}

// The hash of the `bytes` bytes at `at`, of `log` bits. There are eight bytes from `at`.
inline int64_t hashAt(const uint8_t* input, int64_t at, uint32_t bytes, uint32_t log) {
    return int64_t((load64(input, at) << (64 - 8 * bytes)) * 0xcf1bbcdcb7a56463ull >> (64 - log));
}

// The length of the match at `at` of the bytes from the earlier `from`, up to `end`: eight bytes at a time, where the
// first that differs is the lowest set bit of their difference.
inline int64_t matchLength(const uint8_t* input, int64_t at, int64_t from, int64_t end) {
    int64_t n = 0;
    while (end - at - n >= 8) {
        uint64_t difference = load64(input, at + n) ^ load64(input, from + n);
        if (difference != 0)
            return n + prU64LowestBit(difference) / 8;
        n += 8;
    }
    while (at + n < end && input[at + n] == input[from + n])
        ++n;
    return n;
}

// The value that names `offset` after a run of `literalLength` literals: one of the last three offsets, as rfc 8878
// 3.1.1.5 numbers them, or the offset plus 3. The last offsets move as a decoder moves them.
uint32_t offsetValueOf(Encoder& e, int64_t offset, int64_t literalLength) {
    const int64_t r0 = e.repeats[0];
    const int64_t r1 = e.repeats[1];
    const int64_t r2 = e.repeats[2];
    int64_t value = offset + 3;
    if (literalLength != 0) {
        if (offset == r0)
            value = 1;
        else if (offset == r1)
            value = 2;
        else if (offset == r2)
            value = 3;
    } else {
        if (offset == r1)
            value = 1;
        else if (offset == r2)
            value = 2;
        else if (offset == r0 - 1)
            value = 3;
    }
    if (value > 3) {
        e.repeats[0] = offset;
        e.repeats[1] = r0;
        e.repeats[2] = r1;
    } else {
        int64_t which = value - 1 + (literalLength == 0 ? 1 : 0);
        if (which == 1) {
            e.repeats[0] = r1;
            e.repeats[1] = r0;
        } else if (which >= 2) {
            e.repeats[0] = offset;
            e.repeats[1] = r0;
            e.repeats[2] = r1;
        }
    }
    return uint32_t(value);
}

// Adds the literals from `from` to `to`, 16 bytes at a time where the input has 16 bytes to read, since the literals
// have room for 16 past their end.
inline void addLiterals(Encoder& e, int64_t from, int64_t to) {
    uint8_t* out = e.literals + e.literalCount;
    if (e.size - to >= 16) {
        for (int64_t k = 0; k < to - from; k += 16)
            prMemCopy(out + k, e.input + from + k, 16);
    } else if (to != from) {
        prMemCopy(out, e.input + from, size_t(to - from));
    }
    e.literalCount += to - from;
}

// Adds the literals from `anchor` to `at` and a match there of `length` bytes from `offset` back.
void addSequence(Encoder& e, int64_t anchor, int64_t at, int64_t offset, int64_t length) {
    int64_t count = at - anchor;
    addLiterals(e, anchor, at);
    int64_t k = e.sequenceCount;
    uint32_t value = offsetValueOf(e, offset, count);
    e.literalLengths[k] = uint32_t(count);
    e.matchLengths[k] = uint32_t(length);
    e.offsetValues[k] = value;
    uint8_t* codes = e.codes + 3 * k;
    codes[0] = uint8_t(literalLengthCode(uint32_t(count)));
    codes[1] = uint8_t(highBit(value));
    codes[2] = uint8_t(matchLengthCode(uint32_t(length)));
    e.sequenceCount += 1;
}

// Whether the last offset matches four bytes at `at`, which has eight bytes after it.
inline bool repeatAt(const Encoder& e, int64_t at) {
    int64_t offset = e.repeats[0];
    return offset <= at && offset <= e.window && load32(e.input, at - offset) == load32(e.input, at);
}

// A candidate's offset from `at`, or zero if it is not one within the window.
inline int64_t offsetTo(const Encoder& e, int64_t at, int64_t candidate) {
    int64_t offset = at - candidate;
    return offset > 0 && offset <= e.window ? offset : 0;
}

// Extends a match at `at` from `offset` back to the bytes before it, down to `anchor`. Returns where it starts.
inline int64_t extendBack(const uint8_t* input, int64_t at, int64_t offset, int64_t anchor) {
    int64_t start = at;
    while (start > anchor && start > offset && input[start - 1] == input[start - 1 - offset])
        --start;
    return start;
}

// The match finders. Each takes the block from `start` to `end`, adds its sequences, and returns where its last
// literals start. A position has eight bytes after it within the block, so the last eight are literals.

int64_t findFast(Encoder& e, int64_t start, int64_t end) {
    const uint8_t* input = e.input;
    uint32_t* table = e.tables;
    const uint32_t log = e.effort.hashLog;
    const uint32_t hashed = e.effort.hashed;
    const int64_t window = e.window;
    const int64_t limit = end - 8;
    int64_t anchor = start;
    int64_t ip = start;
    int64_t repeat = e.repeats[0];
    while (ip < limit) {
        int64_t h = hashAt(input, ip, hashed, log);
        int64_t candidate = earlier(ip, table[h]);
        table[h] = uint32_t(ip);
        int64_t next = ip + 1;
        if (next < limit && repeat <= next && load32(input, next - repeat) == load32(input, next)) {
            int64_t length = 4 + matchLength(input, next + 4, next + 4 - repeat, end);
            addSequence(e, anchor, next, repeat, length);
            ip = next + length;
            anchor = ip;
            continue;
        }
        int64_t offset = ip - candidate;
        if (offset > 0 && offset <= window && load32(input, candidate) == load32(input, ip)) {
            int64_t length = matchLength(input, ip, candidate, end);
            int64_t at = extendBack(input, ip, offset, anchor);
            addSequence(e, anchor, at, offset, length + (ip - at));
            repeat = offset;
            ip += length;
            anchor = ip;
            if (ip - 2 < limit)
                table[hashAt(input, ip - 2, hashed, log)] = uint32_t(ip - 2);
            continue;
        }
        // Faster through what does not match.
        ip += 1 + ((ip - anchor) >> 7);
    }
    return anchor;
}

int64_t findDouble(Encoder& e, int64_t start, int64_t end) {
    const uint8_t* input = e.input;
    uint32_t* tables = e.tables;
    const uint32_t longLog = e.effort.hashLog;
    const uint32_t shortLog = e.effort.chainLog;
    const int64_t shortBase = int64_t(1) << longLog;
    const int64_t limit = end - 8;
    int64_t anchor = start;
    int64_t ip = start;
    while (ip < limit) {
        int64_t hl = hashAt(input, ip, 8, longLog);
        int64_t hs = shortBase + hashAt(input, ip, 5, shortLog);
        int64_t longCandidate = earlier(ip, tables[hl]);
        int64_t shortCandidate = earlier(ip, tables[hs]);
        tables[hl] = uint32_t(ip);
        tables[hs] = uint32_t(ip);
        if (ip + 1 < limit && repeatAt(e, ip + 1)) {
            int64_t offset = e.repeats[0];
            int64_t length = 4 + matchLength(input, ip + 5, ip + 5 - offset, end);
            addSequence(e, anchor, ip + 1, offset, length);
            ip += 1 + length;
            anchor = ip;
            continue;
        }
        int64_t at = ip;
        int64_t offset = offsetTo(e, ip, longCandidate);
        int64_t length = 0;
        if (offset != 0 && load64(input, longCandidate) == load64(input, ip)) {
            length = 8 + matchLength(input, ip + 8, longCandidate + 8, end);
        } else {
            offset = offsetTo(e, ip, shortCandidate);
            if (offset == 0 || load32(input, shortCandidate) != load32(input, ip)) {
                ip += 1 + ((ip - anchor) >> 8);
                continue;
            }
            // A long match at the next position is better than a short one here.
            int64_t next = ip + 1;
            int64_t hn = hashAt(input, next, 8, longLog);
            int64_t nextCandidate = earlier(next, tables[hn]);
            tables[hn] = uint32_t(next);
            int64_t nextOffset = offsetTo(e, next, nextCandidate);
            if (next < limit && nextOffset != 0 && load64(input, nextCandidate) == load64(input, next)) {
                at = next;
                offset = nextOffset;
                length = 8 + matchLength(input, next + 8, nextCandidate + 8, end);
            } else {
                length = 4 + matchLength(input, ip + 4, shortCandidate + 4, end);
            }
        }
        int64_t first = extendBack(input, at, offset, anchor);
        addSequence(e, anchor, first, offset, length + (at - first));
        int64_t after = at + length;
        // Some of the positions that the match covers.
        const int64_t covered[3] = {at + 2, after - 2, after - 1};
        for (int64_t p : covered) {
            if (p < limit) {
                tables[hashAt(input, p, 8, longLog)] = uint32_t(p);
                tables[shortBase + hashAt(input, p, 5, shortLog)] = uint32_t(p);
            }
        }
        ip = after;
        anchor = ip;
    }
    return anchor;
}

// Adds the positions before `target` to the chains.
inline void insertUntil(Encoder& e, int64_t target) {
    const uint8_t* input = e.input;
    const uint32_t log = e.effort.hashLog;
    const int64_t chainBase = int64_t(1) << log;
    const int64_t mask = (int64_t(1) << e.effort.chainLog) - 1;
    const int64_t last = min(target, e.size - 8);
    for (int64_t p = e.nextInsert; p < last; ++p) {
        int64_t h = hashAt(input, p, 5, log);
        e.tables[chainBase + (p & mask)] = e.tables[h];
        e.tables[h] = uint32_t(p);
    }
    e.nextInsert = max(e.nextInsert, target);
}

// A match that a search found: its length, at least four bytes, and its offset, or a length of zero.
struct Found {
    int64_t length;
    int64_t offset;
};

// The longest match at `at` before `end` that the chains give.
Found searchChains(Encoder& e, int64_t at, int64_t end) {
    insertUntil(e, at);
    const uint8_t* input = e.input;
    const int64_t chainBase = int64_t(1) << e.effort.hashLog;
    const int64_t mask = (int64_t(1) << e.effort.chainLog) - 1;
    // Past the chain's size its links have been overwritten.
    const int64_t lowest = max(at - e.window, at - mask);
    int64_t best = 3;
    int64_t bestOffset = 0;
    int64_t candidate = earlier(at, e.tables[hashAt(input, at, 5, e.effort.hashLog)]);
    uint32_t tries = e.effort.search;
    const int64_t room = end - at;
    while (tries != 0 && candidate >= lowest && candidate < at) {
        if (input[candidate + best] == input[at + best]) {
            int64_t length = matchLength(input, at, candidate, end);
            if (length > best) {
                best = length;
                bestOffset = at - candidate;
                if (length == room)
                    break;
            }
        }
        int64_t next = earlier(candidate, e.tables[chainBase + (candidate & mask)]);
        if (next >= candidate)
            break;
        candidate = next;
        --tries;
    }
    return Found{bestOffset != 0 ? best : 0, bestOffset};
}

// The bits that an offset costs, roughly, for weighing two matches.
inline int64_t offsetCost(int64_t offset) {
    return highBit(uint64_t(offset + 3));
}

int64_t findLazy(Encoder& e, int64_t start, int64_t end) {
    const uint8_t* input = e.input;
    const int64_t limit = end - 8;
    int64_t anchor = start;
    int64_t ip = start;
    while (ip < limit) {
        int64_t at = ip;
        int64_t length = 0;
        int64_t offset = 0;
        if (ip + 1 < limit && repeatAt(e, ip + 1)) {
            offset = e.repeats[0];
            length = 4 + matchLength(input, ip + 5, ip + 5 - offset, end);
            at = ip + 1;
        }
        Found found = searchChains(e, ip, end);
        if (found.length > length) {
            length = found.length;
            offset = found.offset;
            at = ip;
        }
        if (length < 4) {
            ip += 1 + ((ip - anchor) >> 8);
            continue;
        }
        // Up to twice, a better match at the next position wins.
        for (int depth = 0; depth < 2; ++depth) {
            int64_t next = at + 1;
            if (next >= limit)
                break;
            bool better = false;
            if (repeatAt(e, next)) {
                int64_t repeat = 4 + matchLength(input, next + 4, next + 4 - e.repeats[0], end);
                if (repeat * 3 > length * 3 - offsetCost(offset) + 1) {
                    length = repeat;
                    offset = e.repeats[0];
                    at = next;
                    better = true;
                }
            }
            Found later = searchChains(e, next, end);
            if (later.length >= 4 && later.length * 4 - offsetCost(later.offset) > length * 4 - offsetCost(offset) + 4) {
                length = later.length;
                offset = later.offset;
                at = next;
                better = true;
            }
            if (!better)
                break;
        }
        int64_t first = extendBack(input, at, offset, anchor);
        addSequence(e, anchor, first, offset, length + (at - first));
        ip = at + length;
        anchor = ip;
    }
    return anchor;
}

// The output's bits, first bit lowest. Bits gather in a register, which a flush stores whole, eight bytes, advancing
// past the whole bytes in it, so the output has eight bytes of room past what it is written. Between flushes the
// register takes at most 56 bits.
struct BitWriter {
    uint8_t* out;
    int64_t at;
    uint64_t bits;
    uint32_t count;
};

inline void putBits(BitWriter& w, uint64_t value, uint32_t count) {
    w.bits |= (value & ((uint64_t(1) << count) - 1)) << w.count;
    w.count += count;
}

inline void flushWhole(BitWriter& w) {
    uint8_t bytes[8];
    for (uint32_t k = 0; k < 8; ++k)
        bytes[k] = uint8_t(w.bits >> (8 * k));
    prMemCopy(w.out + w.at, bytes, 8);
    w.at += w.count >> 3;
    w.bits = (w.count & 56) == 0 ? w.bits : w.bits >> (w.count & 56);
    w.count &= 7;
}

// Writes the bits left, the last byte padded with zeros, and returns where the output ends.
int64_t flushBits(BitWriter& w) {
    flushWhole(w);
    if (w.count > 0) {
        w.out[w.at] = uint8_t(w.bits);
        w.at += 1;
    }
    return w.at;
}

// Ends a stream that a decoder reads backward: the bit above its last.
int64_t closeStream(BitWriter& w) {
    putBits(w, 1, 1);
    return flushBits(w);
}

// An FSE table for encoding, as the reference encoder builds it: by symbol and the state it leaves, the state it moves
// to, from `states`, and the bits that the move writes.
struct FseEncoder {
    uint32_t log;
    uint16_t states[512];
    int64_t deltaBits[64];
    int64_t deltaStates[64];
};

void buildFseEncoder(const int16_t* counts, int64_t symbols, uint32_t log, FseEncoder& t) {
    const int64_t size = int64_t(1) << log;
    uint8_t spread[512];
    bool spreads = zstdFseSpread(counts, symbols, log, spread);
    PR_ASSERT(spreads);
    (void)spreads;
    int64_t cumulative[64] = {};
    int64_t running = 0;
    for (int64_t s = 0; s < symbols; ++s) {
        cumulative[s] = running;
        running += counts[s] == -1 ? 1 : counts[s];
    }
    for (int64_t u = 0; u < size; ++u) {
        uint8_t s = spread[u];
        t.states[cumulative[s] & 511] = uint16_t(size + u);
        cumulative[s] += 1;
    }
    t.log = log;
    int64_t total = 0;
    for (int64_t s = 0; s < symbols; ++s) {
        int64_t count = counts[s];
        if (count == -1 || count == 1) {
            t.deltaBits[s] = (int64_t(log) << 16) - size;
            t.deltaStates[s] = total - 1;
            total += 1;
        } else if (count > 1) {
            int64_t bits = int64_t(log) - highBit(uint64_t(count - 1));
            t.deltaBits[s] = (bits << 16) - (count << bits);
            t.deltaStates[s] = total - count;
            total += count;
        }
    }
}

// The state that starts encoding with `symbol`: of those that decode it, the one that writes the fewest bits.
inline int64_t fseStart(const FseEncoder& t, uint32_t symbol) {
    uint32_t s = symbol & 63;
    int64_t bits = (t.deltaBits[s] + (1 << 15)) >> 16;
    int64_t value = (bits << 16) - t.deltaBits[s];
    return t.states[((value >> bits) + t.deltaStates[s]) & 511];
}

// Moves `state` to one that decodes `symbol`, writing the bits that take a decoder from that state back to this one.
inline void fseEncode(BitWriter& w, const FseEncoder& t, int64_t& state, uint32_t symbol) {
    uint32_t s = symbol & 63;
    int64_t bits = (state + t.deltaBits[s]) >> 16;
    putBits(w, uint64_t(state), uint32_t(bits));
    state = t.states[((state >> bits) + t.deltaStates[s]) & 511];
}

inline void fseFinish(BitWriter& w, const FseEncoder& t, int64_t state) {
    putBits(w, uint64_t(state), t.log);
}

// The accuracy log of an FSE table for `total` symbols, the largest `maxSymbol`, at most `maxLog`, as the reference
// encoder chooses it.
uint32_t fseLog(uint32_t maxLog, int64_t total, int64_t maxSymbol) {
    int64_t log = maxLog;
    int64_t source = int64_t(highBit(uint64_t(max(total - 1, int64_t(1))))) - 2;
    if (source < log)
        log = max(source, int64_t(0));
    int64_t least = min(int64_t(highBit(uint64_t(total))) + 1, int64_t(highBit(uint64_t(max(maxSymbol, int64_t(1))))) + 2);
    if (least > log)
        log = least;
    return uint32_t(min(max(log, int64_t(5)), int64_t(maxLog)));
}

// Scales `counts`, of `total`, to counts that sum to 2^log, each symbol that occurs at least 1.
void normalizeCounts(const uint32_t* counts, int64_t symbols, int64_t total, uint32_t log, int16_t* norm) {
    const int64_t scale = int64_t(1) << log;
    int64_t sum = 0;
    int64_t largest = 0;
    for (int64_t s = 0; s < symbols; ++s) {
        norm[s] = 0;
        if (counts[s] != 0) {
            int64_t n = max((int64_t(counts[s]) * scale + total / 2) / total, int64_t(1));
            norm[s] = int16_t(n);
            sum += n;
            if (counts[s] > counts[largest])
                largest = s;
        }
    }
    int64_t excess = sum - scale;
    if (norm[largest] - excess >= 1) {
        norm[largest] = int16_t(norm[largest] - excess);
        return;
    }
    // Rounding up the rare symbols took more than the largest can give.
    while (excess > 0) {
        int64_t most = 0;
        for (int64_t s = 0; s < symbols; ++s) {
            if (norm[s] > norm[most])
                most = s;
        }
        norm[most] = int16_t(norm[most] - 1);
        excess -= 1;
    }
}

// Writes an FSE table description (rfc 8878 4.1.1), the inverse of the decoder's reading of one, and returns where it
// ends.
int64_t writeFseCounts(BitWriter& w, const int16_t* norm, int64_t symbols, uint32_t log) {
    putBits(w, log - 5, 4);
    int32_t remaining = (1 << log) + 1;
    int32_t threshold = 1 << log;
    uint32_t nb = log + 1;
    int64_t symbol = 0;
    while (remaining > 1) {
        int32_t count = norm[symbol];
        int32_t value = count + 1;
        int32_t most = 2 * threshold - 1 - remaining;
        if (value < most)
            putBits(w, uint64_t(value), nb - 1);
        else if (value >= threshold)
            putBits(w, uint64_t(value + most), nb);
        else
            putBits(w, uint64_t(value), nb);
        flushWhole(w);
        symbol += 1;
        remaining -= count < 0 ? -count : count;
        while (remaining < threshold) {
            nb -= 1;
            threshold >>= 1;
        }
        if (count == 0) {
            // How many more zeros follow, two bits at a time.
            int64_t run = 0;
            while (symbol + run < symbols && norm[symbol + run] == 0)
                ++run;
            symbol += run;
            for (; run >= 3; run -= 3) {
                putBits(w, 3, 2);
                flushWhole(w);
            }
            putBits(w, uint64_t(run), 2);
            flushWhole(w);
        }
    }
    return flushBits(w);
}

// The cost in 256ths of a bit of `counts` in a table of `norm` at `log`.
int64_t tableCost(const uint32_t* counts, int64_t symbols, const int16_t* norm, int64_t normSymbols, uint32_t log) {
    int64_t cost = 0;
    for (int64_t s = 0; s < symbols; ++s) {
        if (counts[s] != 0) {
            if (s >= normSymbols)
                return int64_t(1) << 60;
            int64_t n = norm[s] == -1 ? 1 : norm[s];
            if (n <= 0)
                return int64_t(1) << 60;
            cost += int64_t(counts[s]) * ((int64_t(log) << 8) - kLog2.values[n & 511]);
        }
    }
    return cost;
}

// How one kind of sequence code is coded: its mode (rfc 8878 3.1.1.3.2.1), and the bytes of its description.
struct TableChoice {
    uint32_t mode;
    int64_t size;
};

// Chooses how to code one kind of sequence code, of `counts` over `total` sequences, and builds its table into `t`, and
// its description into `description`.
TableChoice chooseTable(const uint32_t* counts, int64_t symbols, int64_t total, uint32_t maxLog,
        const int16_t* predefined, int64_t predefinedSymbols, uint32_t predefinedLog, FseEncoder& t,
        uint8_t* description) {
    int64_t maxSymbol = 0;
    int64_t distinct = 0;
    for (int64_t s = 0; s < symbols; ++s) {
        if (counts[s] != 0) {
            maxSymbol = s;
            distinct += 1;
        }
    }
    if (distinct == 1) {
        int16_t one[64] = {};
        one[maxSymbol] = 1;
        buildFseEncoder(one, maxSymbol + 1, 0, t);
        description[0] = uint8_t(maxSymbol);
        return TableChoice{1, 1};
    }
    int64_t predefinedCost = tableCost(counts, symbols, predefined, predefinedSymbols, predefinedLog);
    uint32_t log = fseLog(maxLog, total, maxSymbol);
    int16_t norm[64] = {};
    normalizeCounts(counts, maxSymbol + 1, total, log, norm);
    BitWriter w{description, 0, 0, 0};
    int64_t size = writeFseCounts(w, norm, maxSymbol + 1, log);
    int64_t ownCost = size * 8 * 256 + tableCost(counts, symbols, norm, maxSymbol + 1, log);
    if (predefinedCost <= ownCost) {
        buildFseEncoder(predefined, predefinedSymbols, predefinedLog, t);
        return TableChoice{0, 0};
    }
    buildFseEncoder(norm, maxSymbol + 1, log, t);
    return TableChoice{2, size};
}

// Writes the sequences section of a block (rfc 8878 3.1.1.3.2) from `at` in `out`, and returns where it ends.
int64_t writeSequences(const Encoder& e, uint8_t* out, int64_t at) {
    const int64_t n = e.sequenceCount;
    int64_t p = at;
    if (n < 128) {
        out[p++] = uint8_t(n);
    } else if (n < 0x7f00) {
        out[p++] = uint8_t(n >> 8 | 128);
        out[p++] = uint8_t(n);
    } else {
        out[p++] = 255;
        out[p++] = uint8_t(n - 0x7f00);
        out[p++] = uint8_t((n - 0x7f00) >> 8);
    }
    if (n == 0)
        return p;

    uint32_t llCounts[kZstdLiteralLengthCodes] = {};
    uint32_t ofCounts[kZstdOffsetCodes] = {};
    uint32_t mlCounts[kZstdMatchLengthCodes] = {};
    const uint8_t* codes = e.codes;
    for (int64_t k = 0; k < n; ++k) {
        llCounts[codes[3 * k] % kZstdLiteralLengthCodes] += 1;
        ofCounts[codes[3 * k + 1] % kZstdOffsetCodes] += 1;
        mlCounts[codes[3 * k + 2] % kZstdMatchLengthCodes] += 1;
    }
    FseEncoder ll;
    FseEncoder of;
    FseEncoder ml;
    uint8_t llDescription[128];
    uint8_t ofDescription[128];
    uint8_t mlDescription[128];
    TableChoice llChoice = chooseTable(llCounts, kZstdLiteralLengthCodes, n, kZstdLiteralLengthLog,
        kZstdLiteralLengthDefault, 36, 6, ll, llDescription);
    TableChoice ofChoice = chooseTable(ofCounts, kZstdOffsetCodes, n, kZstdOffsetLog, kZstdOffsetDefault, 29, 5, of,
        ofDescription);
    TableChoice mlChoice = chooseTable(mlCounts, kZstdMatchLengthCodes, n, kZstdMatchLengthLog,
        kZstdMatchLengthDefault, 53, 6, ml, mlDescription);
    out[p++] = uint8_t(llChoice.mode << 6 | ofChoice.mode << 4 | mlChoice.mode << 2);
    prMemCopy(out + p, llDescription, size_t(llChoice.size));
    p += llChoice.size;
    prMemCopy(out + p, ofDescription, size_t(ofChoice.size));
    p += ofChoice.size;
    prMemCopy(out + p, mlDescription, size_t(mlChoice.size));
    p += mlChoice.size;

    // The sequences from the last, which a decoder reads first: each one's codes, then its extra bits.
    BitWriter w{out, p, 0, 0};
    int64_t k = n - 1;
    uint32_t llCode = codes[3 * k];
    uint32_t ofCode = codes[3 * k + 1];
    uint32_t mlCode = codes[3 * k + 2];
    int64_t mlState = fseStart(ml, mlCode);
    int64_t ofState = fseStart(of, ofCode);
    int64_t llState = fseStart(ll, llCode);
    // A flush after the lengths' extra bits, at most 32, after the offset's, at most 31, and after the codes', at most
    // 26.
    for (;;) {
        putBits(w, e.literalLengths[k] - kZstdLiteralLengthBase[llCode % 36], kZstdLiteralLengthExtra[llCode % 36]);
        putBits(w, e.matchLengths[k] - kZstdMatchLengthBase[mlCode % 53], kZstdMatchLengthExtra[mlCode % 53]);
        flushWhole(w);
        putBits(w, e.offsetValues[k], ofCode);
        flushWhole(w);
        if (k == 0)
            break;
        k -= 1;
        llCode = codes[3 * k];
        ofCode = codes[3 * k + 1];
        mlCode = codes[3 * k + 2];
        fseEncode(w, of, ofState, ofCode);
        fseEncode(w, ml, mlState, mlCode);
        fseEncode(w, ll, llState, llCode);
        flushWhole(w);
    }
    fseFinish(w, ml, mlState);
    fseFinish(w, of, ofState);
    fseFinish(w, ll, llState);
    return closeStream(w);
}

// A literals section header (rfc 8878 3.1.1.3.1.1) of raw or RLE literals.
int64_t literalsHeader(uint8_t* out, int64_t at, uint32_t kind, int64_t size) {
    if (size < 32) {
        out[at] = uint8_t(size << 3 | kind);
        return at + 1;
    }
    if (size < 4096) {
        out[at] = uint8_t(size << 4 | 4 | kind);
        out[at + 1] = uint8_t(size >> 4);
        return at + 2;
    }
    out[at] = uint8_t(size << 4 | 12 | kind);
    out[at + 1] = uint8_t(size >> 4);
    out[at + 2] = uint8_t(size >> 12);
    return at + 3;
}

// The size of a raw literals section's header.
int64_t literalsHeaderSize(int64_t n) {
    return n < 32 ? 1 : n < 4096 ? 2 : 3;
}

// Sets the lengths of a prefix code for the 256 literals of the given `frequencies`, none longer than `limit`: those of
// the Huffman code, with any code over the limit shortened and others lengthened to keep the code complete. A code of
// one symbol has length one.
void huffmanLengths(const uint32_t* frequencies, uint32_t limit, uint8_t* lengths) {
    uint16_t symbols[256];
    uint32_t used = 0;
    for (uint32_t symbol = 0; symbol < 256; ++symbol) {
        lengths[symbol] = 0;
        if (frequencies[symbol] != 0)
            symbols[used++] = uint16_t(symbol);
    }
    if (used == 0)
        return;
    if (used == 1) {
        lengths[symbols[0]] = 1;
        return;
    }
    sortItems(Span(symbols, used), [&](uint16_t a, uint16_t b) {
        return frequencies[a] != frequencies[b] ? frequencies[a] < frequencies[b] : a < b;
    });
    // The Huffman tree: the leaves in order of frequency, then the internal nodes in the order they form, which is
    // their order of weight.
    uint32_t weights[512];
    uint16_t parents[512];
    for (uint32_t i = 0; i < used; ++i)
        weights[i] = frequencies[symbols[i]];
    uint32_t leaf = 0;
    uint32_t node = used;
    uint32_t next = used;
    auto lightest = [&]() {
        if (leaf < used && (node == next || weights[leaf] <= weights[node]))
            return leaf++;
        return node++;
    };
    for (uint32_t i = 0; i + 1 < used; ++i) {
        const uint32_t a = lightest();
        const uint32_t b = lightest();
        weights[next] = weights[a] + weights[b];
        parents[a] = parents[b] = uint16_t(next);
        ++next;
    }
    // Depths from the root, the last node, down: a parent comes after its children.
    uint8_t depths[512];
    depths[next - 1] = 0;
    uint32_t counts[512] = {};
    for (uint32_t i = next - 1; i-- > 0;) {
        depths[i] = uint8_t(depths[parents[i]] + 1);
        if (i < used)
            ++counts[depths[i]];
    }
    // Codes over the limit move to the limit. Then, one at a time, a code at the limit goes and a shorter one splits in
    // two, until the code is complete again.
    for (uint32_t length = limit + 1; length < 512; ++length) {
        counts[limit] += counts[length];
        counts[length] = 0;
    }
    uint32_t total = 0;
    for (uint32_t length = 1; length <= limit; ++length)
        total += counts[length] << (limit - length);
    while (total != (1u << limit)) {
        --counts[limit];
        for (uint32_t length = limit - 1; length > 0; --length) {
            if (counts[length] != 0) {
                --counts[length];
                counts[length + 1] += 2;
                break;
            }
        }
        --total;
    }
    // The longest codes go to the least frequent symbols.
    uint32_t at = 0;
    for (uint32_t length = limit; length > 0; --length) {
        for (uint32_t k = 0; k < counts[length]; ++k)
            lengths[symbols[at++]] = uint8_t(length);
    }
}

// A Huffman code for the literals: by symbol, its code and its length.
struct HuffmanCode {
    uint16_t codes[256];
    uint8_t lengths[256];
    uint32_t longest;
};

// Writes the weights of `code` (rfc 8878 4.2.1.1), FSE-coded or four bits each, whichever is shorter, and returns where
// they end, or -1 if neither can describe them.
int64_t writeWeights(const HuffmanCode& code, uint8_t* out, int64_t at) {
    int64_t last = 255;
    while (code.lengths[last] == 0)
        --last;
    // The weights of the symbols before the last, whose weight completes the code.
    uint8_t weights[256] = {};
    uint32_t counts[13] = {};
    for (int64_t s = 0; s < last; ++s) {
        uint32_t length = code.lengths[s];
        weights[s] = length != 0 ? uint8_t(code.longest + 1 - length) : 0;
        counts[weights[s]] += 1;
    }
    const int64_t count = last;
    const int64_t direct = count <= 128 ? 1 + (count + 1) / 2 : int64_t(1) << 30;

    // FSE-coded, in two interleaved states, as the decoder reads them: the first state decodes the weights at even
    // positions, and the last two weights are the states' first.
    int64_t distinct = 0;
    int64_t maxWeight = 0;
    for (int64_t w = 0; w < 13; ++w) {
        if (counts[w] != 0) {
            distinct += 1;
            maxWeight = w;
        }
    }
    uint8_t coded[512] = {};
    int64_t codedSize = int64_t(1) << 30;
    if (distinct > 1 && count >= 2) {
        uint32_t log = min(fseLog(6, count, maxWeight), 6u);
        int16_t norm[13] = {};
        normalizeCounts(counts, maxWeight + 1, count, log, norm);
        BitWriter w{coded + 1, 0, 0, 0};
        int64_t header = writeFseCounts(w, norm, maxWeight + 1, log);
        FseEncoder t;
        buildFseEncoder(norm, maxWeight + 1, log, t);
        BitWriter s{coded + 1 + header, 0, 0, 0};
        int64_t k = count;
        int64_t one = 0;
        int64_t two = 0;
        if ((count & 1) != 0) {
            one = fseStart(t, weights[k - 1]);
            two = fseStart(t, weights[k - 2]);
            fseEncode(s, t, one, weights[k - 3]);
            flushWhole(s);
            k -= 3;
        } else {
            two = fseStart(t, weights[k - 1]);
            one = fseStart(t, weights[k - 2]);
            k -= 2;
        }
        for (; k > 0; k -= 2) {
            fseEncode(s, t, two, weights[k - 1]);
            fseEncode(s, t, one, weights[k - 2]);
            flushWhole(s);
        }
        fseFinish(s, t, two);
        fseFinish(s, t, one);
        int64_t stream = closeStream(s);
        if (header + stream < 128) {
            coded[0] = uint8_t(header + stream);
            // The decoder knows where the weights end only when a state's next bits run past the stream's start, which
            // a state that reads no bits does not do: the weights must read back as they were written.
            uint8_t check[256] = {};
            int64_t readCount = 0;
            int64_t read = zstdReadWeights(coded, 1 + header + stream, check, &readCount);
            if (read == 1 + header + stream && readCount == count && prMemEquals(check, weights, size_t(count)))
                codedSize = read;
        }
    }
    if (codedSize < direct) {
        prMemCopy(out + at, coded, size_t(codedSize));
        return at + codedSize;
    }
    if (direct == int64_t(1) << 30)
        return -1;
    out[at] = uint8_t(127 + count);
    for (int64_t k = 0; k < (count + 1) / 2; ++k)
        out[at + 1 + k] = uint8_t(weights[2 * k] << 4 | weights[2 * k + 1]);
    return at + direct;
}

// Codes the `n` literals at `literals` as one Huffman stream, from the last, which a decoder reads first, and returns
// where it ends.
int64_t huffmanEncode(const HuffmanCode& code, const uint8_t* literals, int64_t n, uint8_t* out, int64_t at) {
    // Four codes of at most 11 bits between flushes.
    BitWriter w{out, at, 0, 0};
    int64_t k = n;
    for (; k >= 4; k -= 4) {
        const uint8_t* four = literals + k - 4;
        putBits(w, code.codes[four[3]], code.lengths[four[3]]);
        putBits(w, code.codes[four[2]], code.lengths[four[2]]);
        putBits(w, code.codes[four[1]], code.lengths[four[1]]);
        putBits(w, code.codes[four[0]], code.lengths[four[0]]);
        flushWhole(w);
    }
    while (k > 0) {
        k -= 1;
        putBits(w, code.codes[literals[k]], code.lengths[literals[k]]);
    }
    return closeStream(w);
}

// Writes the literals section of a block (rfc 8878 3.1.1.3.1), Huffman-coded where that is shorter than the literals
// themselves, and returns where it ends.
int64_t writeLiterals(const Encoder& e, uint8_t* out, int64_t at) {
    const uint8_t* literals = e.literals;
    const int64_t n = e.literalCount;
    // Four counts of each byte, so that a byte repeated waits less on its count's last increment.
    uint32_t lanes[1024] = {};
    int64_t k = 0;
    for (; n - k >= 4; k += 4) {
        lanes[literals[k]] += 1;
        lanes[256 + literals[k + 1]] += 1;
        lanes[512 + literals[k + 2]] += 1;
        lanes[768 + literals[k + 3]] += 1;
    }
    for (; k < n; ++k)
        lanes[literals[k]] += 1;
    uint32_t counts[256];
    int64_t distinct = 0;
    for (uint32_t c = 0; c < 256; ++c) {
        counts[c] = lanes[c] + lanes[256 + c] + lanes[512 + c] + lanes[768 + c];
        distinct += counts[c] != 0;
    }
    if (distinct == 1 && n > 2) {
        int64_t p = literalsHeader(out, at, 1, n);
        out[p] = literals[0];
        return p + 1;
    }
    if (n >= 32 && distinct > 1) {
        HuffmanCode code{};
        huffmanLengths(counts, kZstdHuffmanMaxBits, code.lengths);
        uint64_t bits = 0;
        for (uint32_t s = 0; s < 256; ++s) {
            bits += uint64_t(counts[s]) * code.lengths[s];
            code.longest = max(code.longest, uint32_t(code.lengths[s]));
        }
        // Not worth coding unless it saves something.
        if (int64_t(bits / 8) + 16 < n - n / 64) {
            // Canonical codes in the order the decoder's table gives them: by weight, the longest codes first, and by
            // symbol.
            uint32_t starts[13] = {};
            uint32_t perWeight[13] = {};
            for (uint8_t length : code.lengths) {
                if (length != 0)
                    perWeight[code.longest + 1 - length] += 1;
            }
            uint32_t next = 0;
            for (uint32_t w = 1; w < code.longest + 1; ++w) {
                starts[w] = next;
                next += perWeight[w] << (w - 1);
            }
            for (uint32_t s = 0; s < 256; ++s) {
                if (code.lengths[s] != 0) {
                    uint32_t w = code.longest + 1 - code.lengths[s];
                    code.codes[s] = uint16_t(starts[w] >> (w - 1));
                    starts[w] += 1u << (w - 1);
                }
            }
            // Sizes of 10, 14 or 18 bits: one stream below 256 literals, and four from there.
            const int64_t header = n < 1024 ? 3 : n < 16384 ? 4 : 5;
            const int64_t start = at + header;
            int64_t p = writeWeights(code, out, start);
            if (p >= 0) {
                if (n < 256) {
                    p = huffmanEncode(code, literals, n, out, p);
                } else {
                    // Four streams, after their first three sizes.
                    const int64_t segment = (n + 3) / 4;
                    const int64_t table = p;
                    p += 6;
                    for (int64_t s = 0; s < 4; ++s) {
                        const int64_t from = p;
                        const int64_t first = s * segment;
                        p = huffmanEncode(code, literals + first, min((s + 1) * segment, n) - first, out, p);
                        if (s < 3) {
                            out[table + 2 * s] = uint8_t(p - from);
                            out[table + 2 * s + 1] = uint8_t((p - from) >> 8);
                        }
                    }
                }
                const int64_t compressed = p - start;
                const bool fits = header == 3 ? compressed < 1024 : header == 4 ? compressed < 16384 : true;
                if (compressed + header < n + literalsHeaderSize(n) && fits) {
                    const uint64_t format = n < 256 ? 0 : header == 3 ? 1 : header == 4 ? 2 : 3;
                    const uint32_t width = header == 3 ? 10 : header == 4 ? 14 : 18;
                    const uint64_t field = 2 | format << 2 | uint64_t(n) << 4 | uint64_t(compressed) << (4 + width);
                    for (int64_t b = 0; b < header; ++b)
                        out[at + b] = uint8_t(field >> (8 * b));
                    return p;
                }
            }
        }
    }
    const int64_t p = literalsHeader(out, at, 0, n);
    if (n != 0)
        prMemCopy(out + p, literals, size_t(n));
    return p + n;
}

bool allSame(const uint8_t* bytes, int64_t size) {
    for (int64_t k = 1; k < size; ++k) {
        if (bytes[k] != bytes[0])
            return false;
    }
    return true;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrZstdDecoder : Zstd {};

PrZstdDecoder* prZstdDecoderCreate(size_t windowLimit) {
    // Everything but the buffers of a held block and of the literals, 256K that every use writes before it reads, so
    // that a decoder of a small frame costs what the frame does.
    PrZstdDecoder* decoder = alloc<PrZstdDecoder>();
    prMemZero(decoder, offsetof(Zstd, held));
    prMemZero(&decoder->base, sizeof(Zstd) - offsetof(Zstd, base));
    prZstdDecoderSetWindowLimit(decoder, windowLimit);
    prZstdDecoderReset(decoder);
    return decoder;
}

void prZstdDecoderDestroy(PrZstdDecoder* PR_NULLABLE decoder) {
    if (decoder == nullptr)
        return;
    prMemoryFree(decoder->history);
    prMemoryFree(decoder);
}

void prZstdDecoderSetWindowLimit(PrZstdDecoder* decoder, size_t windowLimit) {
    decoder->windowLimit = windowLimit < size_t(INT64_MAX) ? int64_t(windowLimit) : INT64_MAX;
}

void prZstdDecoderReset(PrZstdDecoder* decoder) {
    decoder->phase = ZstdPhase_Magic;
    decoder->direct = false;
    decoder->frames = 0;
    decoder->heldSize = 0;
    decoder->base = 0;
    decoder->position = 0;
    decoder->flushed = 0;
}

PrStatus prZstdDecoderRead(PrZstdDecoder* decoder, PrByteView input, bool final, uint8_t* PR_NULLABLE output,
        size_t capacity, PrZstdRead* out) {
    Zstd& d = *decoder;
    const PrByteView bytes = input;
    // A first read that has the whole stream writes to the output directly, so that matches read back over it.
    if (final && d.phase == ZstdPhase_Magic && d.position == 0 && d.heldSize == 0 && d.frames == 0)
        d.direct = true;
    int64_t at = 0;
    int64_t wrote = 0;
    ZstdResult result = readFrames(d, bytes, &at, final, output, int64_t(capacity), &wrote);
    if (result == ZstdResult_Input && final)
        result = ZstdResult_Truncated;
    if (result != ZstdResult_Input && result != ZstdResult_Output && result != ZstdResult_Done) {
        d.phase = ZstdPhase_Failed;
        d.failure = result;
    }
    *out = PrZstdRead{.consumed = size_t(at), .written = size_t(wrote)};
    switch (result) {
        case ZstdResult_Input: out->progress = PrZstdProgress_Input; return PrStatus_Ok;
        case ZstdResult_Output: out->progress = PrZstdProgress_Output; return PrStatus_Ok;
        case ZstdResult_Done: out->progress = PrZstdProgress_Done; return PrStatus_Ok;
        case ZstdResult_Truncated: return PrStatus_Truncated;
        case ZstdResult_Dictionary: return PrStatus_NotSupported;
        default: return PrStatus_InvalidData;
    }
}

int64_t prZstdContentSize(PrByteView stream) {
    if (stream.size < 5 || prMemReadLeU32(stream.data) != kMagic)
        return -1;
    int64_t size = frameHeaderSize(stream.data[4]);
    if (int64_t(stream.size) < 4 + size)
        return -1;
    FrameHeader h;
    return parseFrameHeader(stream.data + 4, size, &h) ? h.contentSize : -1;
}

size_t prZstdEncodeBound(size_t size) {
    // A frame header, each block stored with its header at worst, and the checksum.
    return size + 3 * (size / size_t(kZstdBlockMax) + 1) + 18 + 4;
}

size_t prZstdEncode(PrZstdLevel level, PrByteView input, uint8_t* output) {
    PR_ASSERT(level <= PrZstdLevel_Small);
    PR_ASSERT(input.data != nullptr || input.size == 0);
    const Effort effort = effortOf(level, int64_t(input.size));
    const int64_t size = int64_t(input.size);
    // A frame no larger than the level's window is one segment, whose window is its size.
    const bool single = size <= int64_t(1) << effort.windowLog;
    const int64_t window = single ? size : int64_t(1) << effort.windowLog;

    // The frame header (rfc 8878 3.1.1.1), with the content size and a checksum.
    const uint32_t flag = size < 256 && single ? 0 : size < 65536 + 256 ? 1 : size < int64_t(1) << 32 ? 2 : 3;
    const uint32_t descriptor = flag << 6 | (single ? 32 : 0) | 4;
    prMemWriteLeU32(output, 0xfd2fb528u);
    output[4] = uint8_t(descriptor);
    int64_t at = 5;
    if (!single)
        output[at++] = uint8_t((effort.windowLog - 10) << 3);
    const int64_t fieldSize = flag == 0 ? 1 : int64_t(1) << flag;
    const uint64_t fieldValue = flag == 1 ? uint64_t(size - 256) : uint64_t(size);
    for (int64_t k = 0; k < fieldSize; ++k)
        output[at + k] = uint8_t(fieldValue >> (8 * k));
    at += fieldSize;

    const int64_t blockSize = min(kZstdBlockMax, max(size, int64_t(1)));
    const int64_t second = effort.strategy == Strategy_Fast ? 0 : int64_t(1) << effort.chainLog;
    const int64_t tableSize = (int64_t(1) << effort.hashLog) + second;
    // A block coded, before it is known to be smaller than stored: its literals at worst 11 bits each and its sequences
    // at most 12 input.
    const int64_t scratchSize = 2 * blockSize + 12 * min(kSequences, blockSize / 3 + 1) + 1024;
    Encoder e{};
    e.input = input.data;
    e.size = size;
    e.effort = effort;
    e.window = window;
    e.repeats[0] = 1;
    e.repeats[1] = 4;
    e.repeats[2] = 8;
    e.literals = static_cast<uint8_t*>(prMemoryAlloc(size_t(blockSize + kLiteralSlack)));
    e.literalLengths = static_cast<uint32_t*>(prMemoryAlloc(size_t(kSequences) * sizeof(uint32_t)));
    e.matchLengths = static_cast<uint32_t*>(prMemoryAlloc(size_t(kSequences) * sizeof(uint32_t)));
    e.offsetValues = static_cast<uint32_t*>(prMemoryAlloc(size_t(kSequences) * sizeof(uint32_t)));
    e.codes = static_cast<uint8_t*>(prMemoryAlloc(size_t(3 * kSequences)));
    e.tables = static_cast<uint32_t*>(prMemoryAlloc(size_t(tableSize) * sizeof(uint32_t)));
    prMemZero(e.tables, size_t(tableSize) * sizeof(uint32_t));
    uint8_t* scratch = static_cast<uint8_t*>(prMemoryAlloc(size_t(scratchSize)));

    int64_t start = 0;
    for (;;) {
        const int64_t end = min(start + kZstdBlockMax, size);
        const uint32_t last = end == size ? 1 : 0;
        int64_t saved[3] = {e.repeats[0], e.repeats[1], e.repeats[2]};
        e.literalCount = 0;
        e.sequenceCount = 0;
        int64_t rest = start;
        if (effort.strategy == Strategy_Fast)
            rest = findFast(e, start, end);
        else if (effort.strategy == Strategy_Double)
            rest = findDouble(e, start, end);
        else
            rest = findLazy(e, start, end);
        addLiterals(e, rest, end);
        const int64_t body = writeSequences(e, scratch, writeLiterals(e, scratch, 0));
        PR_ASSERT(body <= scratchSize);
        const int64_t length = end - start;
        const uint8_t* block = input.data + start;
        // A block of one byte repeated is that byte, and a block that coding does not shorten is stored. Neither has
        // sequences, so the last offsets stay as they were.
        uint32_t kind = 2;
        if (body < 32 && length > 1 && allSame(block, length))
            kind = 1;
        else if (body >= length)
            kind = 0;
        if (kind != 2) {
            e.repeats[0] = saved[0];
            e.repeats[1] = saved[1];
            e.repeats[2] = saved[2];
        }
        const int64_t stored = kind == 2 ? body : kind == 1 ? 1 : length;
        const uint32_t header = kind << 1 | last | uint32_t((kind == 2 ? body : length) << 3);
        output[at] = uint8_t(header);
        output[at + 1] = uint8_t(header >> 8);
        output[at + 2] = uint8_t(header >> 16);
        at += 3;
        if (kind == 2)
            prMemCopy(output + at, scratch, size_t(stored));
        else if (kind == 1)
            output[at] = block[0];
        else if (stored != 0)
            prMemCopy(output + at, block, size_t(stored));
        at += stored;
        start = end;
        if (start == size)
            break;
    }
    prMemoryFree(scratch);
    prMemoryFree(e.tables);
    prMemoryFree(e.codes);
    prMemoryFree(e.offsetValues);
    prMemoryFree(e.matchLengths);
    prMemoryFree(e.literalLengths);
    prMemoryFree(e.literals);

    const uint64_t checksum = prXxh64(input, 0);
    prMemWriteLeU32(output + at, uint32_t(checksum));
    at += 4;
    PR_ASSERT(size_t(at) <= prZstdEncodeBound(input.size));
    return size_t(at);
}
