// Deflate decoding, bare or framed as zlib or gzip, a piece of input at a time.

#include "compress/backends/builtin/src/inflate.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/checksum.h"
#include "pr/count_of.h"
#include "pr/mem.h"

namespace pr_impl {
namespace {

// The longest code in a Deflate Huffman table, and the largest number of symbols in a table, which is the 288 of the
// literal and length alphabet (rfc 1951 3.2.5, 3.2.7).
constexpr uint32_t kCodeMaxLength = 15;
constexpr uint32_t kMaxSymbols = 288;

// The bytes of output that a distance can reach back over (rfc 1951 3.2.5).
constexpr uint32_t kHistorySize = 32768;

// The number of bits that the lookup of a table indexes. A code no longer than this decodes in one lookup. A longer
// code walks the canonical code, starting from the bits that the lookup read.
constexpr uint32_t kFastBits = 10;

// The longest match.
constexpr uint32_t kMaxMatch = 258;

// The number of bytes past the end of a match that its copy may write, and the largest number of bits that one literal
// or match reads. A match reads a 15-bit code and 5 extra bits for the length, and a 15-bit code and 13 extra bits for
// the distance.
constexpr uint32_t kMatchSlack = 8;
constexpr uint32_t kSymbolMaxBits = 48;

// The lengths encoded by the codes of the literal alphabet from symbol 257 up, and the extra bits that each reads (rfc
// 1951 3.2.5).
constexpr uint16_t kLengthBase[] = {
    3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
    31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258,
};
constexpr uint8_t kLengthExtra[] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
    2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0,
};

// The distances encoded by the codes of the distance alphabet, and their extra bits (rfc 1951 3.2.5).
constexpr uint16_t kDistanceBase[] = {
    1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577,
};
constexpr uint8_t kDistanceExtra[] = {
    0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
    6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13,
};

// The order in which the lengths of the code length alphabet are stored (rfc 1951 3.2.7).
constexpr uint8_t kCodeLengthOrder[] = {16, 17, 18, 0, 8,  7, 9,  6, 10, 5, 11, 4,  12, 3, 13, 2, 14, 1, 15};

// A canonical Huffman code: code lengths, symbols in code-assignment order, and a lookup table for the short codes. The
// lookup is indexed by the next bits of the stream, first bit lowest. It indexes as many bits as the longest code has,
// up to kFastBits, so that a small table builds quickly. An entry is symbol << 4 | length, or zero for a longer code.
struct Huffman {
    uint16_t counts[kCodeMaxLength + 1];
    uint16_t symbols[kMaxSymbols];
    uint16_t fast[1u << kFastBits];
    uint32_t fastMask;
    uint32_t maximum;
};

// The two-bit block encoding.
enum {
    InflateBlock_Stored,
    InflateBlock_Fixed,
    InflateBlock_Dynamic,
    InflateBlock_Reserved,
};
typedef uint8_t InflateBlock;

// Resumable fields of a Deflate block.
enum {
    InflatePhase_Block,
    InflatePhase_StoredLength,
    InflatePhase_StoredComplement,
    InflatePhase_StoredBytes,
    InflatePhase_DynamicCounts,
    InflatePhase_CodeLengths,
    InflatePhase_TableLengths,
    InflatePhase_RepeatLength,
    InflatePhase_Symbol,
    InflatePhase_Literal,
    InflatePhase_Length,
    InflatePhase_DistanceSymbol,
    InflatePhase_Distance,
    InflatePhase_Copy,
    InflatePhase_Done,
    InflatePhase_Invalid,
};
typedef uint8_t InflatePhase;

// The state that persists across calls: bits, codes and output history. No input spans are kept across a call. The bit
// buffer holds only the bits of a partly read byte, or, between calls, the bits of an incomplete code.
struct Deflate {
    // The literal and length codes, and the distance codes, that a dynamic block defines.
    Huffman literals;
    Huffman distances;

    // The codes that the current block is decoded with: the fixed codes, or the codes that a dynamic block defines.
    const Huffman* literalCodes;
    const Huffman* distanceCodes;

    Huffman codes;
    uint8_t lengths[kMaxSymbols + 32];
    uint8_t codeLengths[19];
    uint8_t history[kHistorySize];
    uint32_t bits;
    uint32_t bitCount;
    uint32_t historyAt;
    uint32_t historySize;
    uint32_t windowLimit;
    uint32_t literalCount;
    uint32_t distanceCount;
    uint32_t codeCount;
    uint32_t tableAt;
    uint32_t remaining;
    uint32_t distance;
    uint32_t extra;
    uint8_t literal;
    bool finalBlock;
    InflatePhase phase;
};

// The caller's current spans and the progress through them. The Deflate data of this call starts at offset `base` of
// the output. A match that reaches back no further than `base` reads only the output. A match that reaches further also
// reads the history.
struct InflateSpan {
    PrByteView input;
    size_t consumed;
    uint8_t* output;
    size_t capacity;
    size_t written;
    size_t base;
};

// The container fields that surround Deflate data (RFC 1950 2.2, RFC 1952 2.3).
enum {
    InflateFrame_ZlibHeader,
    InflateFrame_DictionaryId,
    InflateFrame_Dictionary,
    InflateFrame_GzipHeader,
    InflateFrame_ExtraLength,
    InflateFrame_Extra,
    InflateFrame_Name,
    InflateFrame_Comment,
    InflateFrame_HeaderCrc,
    InflateFrame_Data,
    InflateFrame_Trailer,
    InflateFrame_Member,
    InflateFrame_Done,
    InflateFrame_Invalid,
};
typedef uint8_t InflateFrame;

// Container checksums and bounded scratch storage. Names, comments and extra fields are skipped.
struct InflateState {
    Deflate deflate;
    InflateFormat format;
    InflateFrame frame;
    uint8_t header[10];
    uint32_t headerSize;
    uint32_t headerCrc;
    uint32_t checksum;
    uint32_t size;
    uint32_t remaining;
    uint32_t dictionary;
};

void resetDeflate(Deflate& parser) {
    parser.bits = parser.bitCount = 0;
    parser.historyAt = parser.historySize = 0;
    parser.windowLimit = kHistorySize;
    parser.phase = InflatePhase_Block;
}

bool readBits(Deflate& parser, InflateSpan& span, uint32_t count, uint32_t& value) {
    while (parser.bitCount < count) {
        if (span.consumed == span.input.size)
            return false;
        parser.bits |= uint32_t(span.input.data[span.consumed++]) << parser.bitCount;
        parser.bitCount += 8;
    }
    value = parser.bits & ((1u << count) - 1);
    parser.bits >>= count;
    parser.bitCount -= count;
    return true;
}

// Returns the byte `distance` back from the end of the output. The history holds it if this call has not written that
// far.
uint8_t byteBack(const Deflate& parser, const InflateSpan& span, uint32_t distance) {
    size_t produced = span.written - span.base;
    if (distance <= produced)
        return span.output[span.written - distance];
    return parser.history[(parser.historyAt + kHistorySize - (distance - uint32_t(produced))) % kHistorySize];
}

// Returns true if a match `distance` back reaches only data that the stream produced, within its window.
bool reaches(const Deflate& parser, const InflateSpan& span, uint32_t distance) {
    return distance <= parser.historySize + (span.written - span.base) && distance <= parser.windowLimit;
}

// Appends this call's output to the history.
void keepHistory(Deflate& parser, const uint8_t* data, size_t size) {
    if (size >= kHistorySize) {
        prMemCopy(parser.history, data + size - kHistorySize, kHistorySize);
        parser.historyAt = 0;
        parser.historySize = kHistorySize;
        return;
    }
    uint32_t count = uint32_t(size);
    uint32_t first = kHistorySize - parser.historyAt < count ? kHistorySize - parser.historyAt : count;
    prMemCopy(parser.history + parser.historyAt, data, first);
    prMemCopy(parser.history, data + first, count - first);
    parser.historyAt = (parser.historyAt + count) % kHistorySize;
    parser.historySize = parser.historySize + count < kHistorySize ? parser.historySize + count : kHistorySize;
}

// Copies a match of `length` bytes from `distance` back in the output. The copy runs forward, so a match longer than
// its distance repeats a pattern. It copies eight bytes at a time and may write up to kMatchSlack bytes past the match.
// A match nearer than eight bytes is a pattern with period `distance`. After its first eight bytes, it copies from the
// nearest multiple of the period that is at least eight bytes back.
void copyMatch(uint8_t* out, uint32_t distance, uint32_t length) {
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

constexpr bool buildHuffman(Huffman& table, const uint8_t* lengths, uint32_t count) {
    for (uint16_t& c : table.counts)
        c = 0;
    table.maximum = 0;
    for (uint32_t symbol = 0; symbol < count; ++symbol) {
        ++table.counts[lengths[symbol]];
        if (lengths[symbol] > table.maximum)
            table.maximum = lengths[symbol];
    }
    int32_t left = 1;
    for (uint32_t length = 1; length <= kCodeMaxLength; ++length) {
        left = (left << 1) - table.counts[length];
        if (left < 0)
            return false;
    }
    // RFC 1951 3.2.7 permits a one-bit singleton and an unused distance table.
    if (left && table.maximum > 1)
        return false;
    uint16_t offsets[kCodeMaxLength + 1]{};
    for (uint32_t length = 1; length < kCodeMaxLength; ++length)
        offsets[length + 1] = uint16_t(offsets[length] + table.counts[length]);
    for (uint32_t symbol = 0; symbol < count; ++symbol)
        if (lengths[symbol])
            table.symbols[offsets[lengths[symbol]]++] = uint16_t(symbol);
    // Each short code fills the entries whose low bits are the code, because the stream sends the first bit first.
    const uint32_t fastBits = table.maximum < kFastBits ? table.maximum : kFastBits;
    table.fastMask = (1u << fastBits) - 1;
    for (uint32_t at = 0; at <= table.fastMask; ++at)
        table.fast[at] = 0;
    uint32_t code = 0;
    uint32_t index = 0;
    for (uint32_t length = 1; length <= fastBits; ++length) {
        for (uint32_t k = 0; k < table.counts[length]; ++k, ++code, ++index) {
            uint32_t reversed = 0;
            for (uint32_t bit = 0; bit < length; ++bit)
                reversed |= ((code >> bit) & 1) << (length - 1 - bit);
            uint16_t entry = uint16_t(uint32_t(table.symbols[index]) << 4 | length);
            for (uint32_t at = reversed; at <= table.fastMask; at += 1u << length)
                table.fast[at] = entry;
        }
        code <<= 1;
    }
    return true;
}

// The outcome of decoding one Huffman symbol. It is distinct from a terminal stream result.
enum {
    SymbolResult_Decoded,
    SymbolResult_Input,
    SymbolResult_Invalid,
};
typedef uint8_t SymbolResult;

// Decodes the symbol whose code starts `bits`, of which the low `available` bits come from the stream. Sets `length` to
// the length of the code.
SymbolResult decodeSymbol(const Huffman& table, uint64_t bits, uint32_t available, uint32_t& symbol, uint32_t& length) {
    const uint32_t entry = table.fast[bits & table.fastMask];
    if (entry != 0) {
        length = entry & 15;
        symbol = entry >> 4;
        return length <= available ? SymbolResult_Decoded : SymbolResult_Input;
    }
    uint32_t code = 0;
    uint32_t first = 0;
    uint32_t index = 0;
    for (length = 1; length <= table.maximum; ++length) {
        if (length > available)
            return SymbolResult_Input;
        code |= uint32_t(bits >> (length - 1)) & 1;
        const uint32_t count = table.counts[length];
        if (code >= first && code - first < count) {
            symbol = table.symbols[index + code - first];
            return SymbolResult_Decoded;
        }
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return SymbolResult_Invalid;
}

// Reads a symbol from the bits held and the input that follows. If there is not enough input, the bits of the
// incomplete code are held for the next call.
SymbolResult readSymbol(Deflate& parser, InflateSpan& span, const Huffman& table, uint32_t& symbol) {
    uint64_t bits = parser.bits;
    uint32_t available = parser.bitCount;
    for (size_t at = span.consumed; available <= kCodeMaxLength && at < span.input.size; ++at) {
        bits |= uint64_t(span.input.data[at]) << available;
        available += 8;
    }
    uint32_t length = 0;
    const SymbolResult result = decodeSymbol(table, bits, available, symbol, length);
    if (result == SymbolResult_Decoded) {
        uint32_t value = 0;
        readBits(parser, span, length, value);
    } else if (result == SymbolResult_Input) {
        // Having fewer bits than the code needs means that every byte of the input was peeked.
        parser.bits = uint32_t(bits);
        parser.bitCount = available;
        span.consumed = span.input.size;
    }
    return result;
}

// The fixed codes (rfc 1951 3.2.6), built at compile time.
struct FixedCodes {
    Huffman literals;
    Huffman distances;
};

constexpr FixedCodes fixedCodes() {
    FixedCodes codes{};
    uint8_t lengths[288]{};
    for (uint32_t i = 0; i < 288; ++i)
        lengths[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
    buildHuffman(codes.literals, lengths, 288);
    for (uint32_t i = 0; i < 32; ++i)
        lengths[i] = 5;
    buildHuffman(codes.distances, lengths, 32);
    return codes;
}

constexpr FixedCodes kFixedCodes = fixedCodes();

void useFixed(Deflate& parser) {
    parser.literalCodes = &kFixedCodes.literals;
    parser.distanceCodes = &kFixedCodes.distances;
}

void endBlock(Deflate& parser) {
    parser.phase = parser.finalBlock ? InflatePhase_Done : InflatePhase_Block;
}

// Decodes literals and matches with the bits in a register, while the input holds more than one literal or match can
// read and the output has room for the longest match. It stops at the end of the block, and sets the phase to Invalid
// for bad data. Whole bytes that it read ahead go back to the input, so the bits held are a partial byte as before.
void decodeFast(Deflate& parser, InflateSpan& span) {
    const uint8_t* in = span.input.data;
    const size_t inputEnd = span.input.size;
    size_t at = span.consumed;
    uint8_t* out = span.output;
    size_t written = span.written;
    uint64_t bits = parser.bits;
    uint32_t count = parser.bitCount;
    size_t fetched = 0;
    auto take = [&](uint32_t n) {
        const uint32_t value = uint32_t(bits & ((uint64_t(1) << n) - 1));
        bits >>= n;
        count -= n;
        return value;
    };
    while (span.capacity - written >= kMaxMatch + kMatchSlack) {
        // Fills the register to at least 56 bits with one read of eight bytes, or one byte at a time near the end of
        // the input. It stops if the rest of the input may not hold a whole literal or match.
        if (inputEnd - at >= 8) {
            bits |= prMemReadLeU64(in + at) << count;
            const uint32_t added = (63 - count) / 8;
            at += added;
            fetched += added;
            count += added * 8;
        } else {
            for (; count <= 56 && at < inputEnd; ++at, ++fetched, count += 8)
                bits |= uint64_t(in[at]) << count;
            if (count < kSymbolMaxBits)
                break;
        }
        uint32_t symbol = 0;
        uint32_t length = 0;
        if (decodeSymbol(*parser.literalCodes, bits, count, symbol, length) != SymbolResult_Decoded) {
            parser.phase = InflatePhase_Invalid;
            break;
        }
        take(length);
        if (symbol < 256) {
            out[written++] = uint8_t(symbol);
            continue;
        }
        if (symbol == 256) {
            endBlock(parser);
            break;
        }
        if (symbol > 285) {
            parser.phase = InflatePhase_Invalid;
            break;
        }
        const uint32_t matchLength = kLengthBase[symbol - 257] + take(kLengthExtra[symbol - 257]);
        if (decodeSymbol(*parser.distanceCodes, bits, count, symbol, length) != SymbolResult_Decoded ||
                symbol >= countOf(kDistanceBase)) {
            parser.phase = InflatePhase_Invalid;
            break;
        }
        take(length);
        const uint32_t distance = kDistanceBase[symbol] + take(kDistanceExtra[symbol]);
        span.written = written;
        if (!reaches(parser, span, distance)) {
            parser.phase = InflatePhase_Invalid;
            break;
        }
        const size_t produced = written - span.base;
        if (distance <= produced) {
            copyMatch(out + written, distance, matchLength);
        } else {
            for (uint32_t k = 0; k < matchLength; ++k) {
                span.written = written + k;
                out[written + k] = byteBack(parser, span, distance);
            }
        }
        written += matchLength;
    }
    const uint32_t whole = count / 8 < fetched ? count / 8 : uint32_t(fetched);
    at -= whole;
    count -= whole * 8;
    parser.bits = uint32_t(bits & ((uint64_t(1) << count) - 1));
    parser.bitCount = count;
    span.consumed = at;
    span.written = written;
}

InflateResult decodeDeflate(Deflate& parser, InflateSpan& span) {
    for (;;) {
        uint32_t value;
        switch (parser.phase) {
            case InflatePhase_Block: {
                if (!readBits(parser, span, 3, value))
                    return InflateResult_Input;
                parser.finalBlock = (value & 1) != 0;
                const InflateBlock block = uint8_t(value >> 1);
                switch (block) {
                    case InflateBlock_Stored:
                        // A code that waited for input took every bit held, so only a partial byte is left.
                        PR_ASSERT(parser.bitCount < 8);
                        parser.bits = parser.bitCount = 0;
                        parser.phase = InflatePhase_StoredLength;
                        break;
                    case InflateBlock_Fixed:
                        useFixed(parser);
                        parser.phase = InflatePhase_Symbol;
                        break;
                    case InflateBlock_Dynamic:
                        parser.phase = InflatePhase_DynamicCounts;
                        break;
                    case InflateBlock_Reserved:
                        return InflateResult_Invalid;
                }
                break;
            }
            case InflatePhase_StoredLength:
                if (!readBits(parser, span, 16, parser.remaining))
                    return InflateResult_Input;
                parser.phase = InflatePhase_StoredComplement;
                break;
            case InflatePhase_StoredComplement:
                if (!readBits(parser, span, 16, value))
                    return InflateResult_Input;
                if ((parser.remaining ^ 0xffffu) != value)
                    return InflateResult_Invalid;
                parser.phase = InflatePhase_StoredBytes;
                break;
            case InflatePhase_StoredBytes:
                if (!parser.remaining) {
                    endBlock(parser);
                    break;
                }
                if (span.written == span.capacity)
                    return InflateResult_Output;
                if (parser.bitCount == 0) {
                    size_t count = parser.remaining;
                    count = count < span.input.size - span.consumed ? count : span.input.size - span.consumed;
                    count = count < span.capacity - span.written ? count : span.capacity - span.written;
                    if (count == 0)
                        return InflateResult_Input;
                    prMemCopy(span.output + span.written, span.input.data + span.consumed, count);
                    span.consumed += count;
                    span.written += count;
                    parser.remaining -= uint32_t(count);
                    break;
                }
                if (!readBits(parser, span, 8, value))
                    return InflateResult_Input;
                span.output[span.written++] = uint8_t(value);
                --parser.remaining;
                break;
            case InflatePhase_DynamicCounts:
                if (!readBits(parser, span, 14, value))
                    return InflateResult_Input;
                parser.literalCount = (value & 31) + 257;
                parser.distanceCount = ((value >> 5) & 31) + 1;
                parser.codeCount = (value >> 10) + 4;
                if (parser.literalCount > 286)
                    return InflateResult_Invalid;
                prMemZero(parser.codeLengths, sizeof(parser.codeLengths));
                parser.tableAt = 0;
                parser.phase = InflatePhase_CodeLengths;
                break;
            case InflatePhase_CodeLengths:
                if (parser.tableAt < parser.codeCount) {
                    if (!readBits(parser, span, 3, value))
                        return InflateResult_Input;
                    parser.codeLengths[kCodeLengthOrder[parser.tableAt++]] = uint8_t(value);
                    break;
                }
                if (!buildHuffman(parser.codes, parser.codeLengths, uint32_t(countOf(parser.codeLengths))) ||
                        !parser.codes.maximum || (parser.codes.maximum == 1 && parser.codes.counts[1] != 2))
                    return InflateResult_Invalid;
                parser.tableAt = 0;
                parser.phase = InflatePhase_TableLengths;
                break;
            case InflatePhase_TableLengths: {
                if (parser.tableAt == parser.literalCount + parser.distanceCount) {
                    if (!parser.lengths[256] || !buildHuffman(parser.literals, parser.lengths, parser.literalCount) ||
                            !buildHuffman(parser.distances, parser.lengths + parser.literalCount, parser.distanceCount))
                        return InflateResult_Invalid;
                    parser.literalCodes = &parser.literals;
                    parser.distanceCodes = &parser.distances;
                    parser.phase = InflatePhase_Symbol;
                    break;
                }
                const SymbolResult result = readSymbol(parser, span, parser.codes, value);
                if (result != SymbolResult_Decoded)
                    return result == SymbolResult_Input ? InflateResult_Input : InflateResult_Invalid;
                if (value < 16) {
                    parser.lengths[parser.tableAt++] = uint8_t(value);
                } else {
                    if (value == 16 && !parser.tableAt)
                        return InflateResult_Invalid;
                    parser.literal = value == 16 ? parser.lengths[parser.tableAt - 1] : 0;
                    parser.remaining = value == 18 ? 11 : 3;
                    parser.extra = value == 16 ? 2 : value == 17 ? 3 : 7;
                    parser.phase = InflatePhase_RepeatLength;
                }
                break;
            }
            case InflatePhase_RepeatLength:
                if (!readBits(parser, span, parser.extra, value))
                    return InflateResult_Input;
                parser.remaining += value;
                if (parser.remaining > parser.literalCount + parser.distanceCount - parser.tableAt)
                    return InflateResult_Invalid;
                while (parser.remaining) {
                    parser.lengths[parser.tableAt++] = parser.literal;
                    --parser.remaining;
                }
                parser.phase = InflatePhase_TableLengths;
                break;
            case InflatePhase_Symbol: {
                if (parser.bitCount + (span.input.size - span.consumed) * 8 >= kSymbolMaxBits &&
                        span.capacity - span.written >= kMaxMatch + kMatchSlack) {
                    const size_t consumed = span.consumed;
                    const size_t written = span.written;
                    const InflatePhase phase = parser.phase;
                    decodeFast(parser, span);
                    if (span.consumed != consumed || span.written != written || parser.phase != phase)
                        break;
                }
                const SymbolResult result = readSymbol(parser, span, *parser.literalCodes, value);
                if (result != SymbolResult_Decoded)
                    return result == SymbolResult_Input ? InflateResult_Input : InflateResult_Invalid;
                if (value < 256) {
                    parser.literal = uint8_t(value);
                    parser.phase = InflatePhase_Literal;
                } else if (value == 256) {
                    endBlock(parser);
                } else {
                    if (value > 285)
                        return InflateResult_Invalid;
                    parser.remaining = kLengthBase[value - 257];
                    parser.extra = kLengthExtra[value - 257];
                    parser.phase = InflatePhase_Length;
                }
                break;
            }
            case InflatePhase_Literal:
                if (span.written == span.capacity)
                    return InflateResult_Output;
                span.output[span.written++] = parser.literal;
                parser.phase = InflatePhase_Symbol;
                break;
            case InflatePhase_Length:
                if (!readBits(parser, span, parser.extra, value))
                    return InflateResult_Input;
                parser.remaining += value;
                parser.phase = InflatePhase_DistanceSymbol;
                break;
            case InflatePhase_DistanceSymbol: {
                const SymbolResult result = readSymbol(parser, span, *parser.distanceCodes, value);
                if (result != SymbolResult_Decoded)
                    return result == SymbolResult_Input ? InflateResult_Input : InflateResult_Invalid;
                if (value >= countOf(kDistanceBase))
                    return InflateResult_Invalid;
                parser.distance = kDistanceBase[value];
                parser.extra = kDistanceExtra[value];
                parser.phase = InflatePhase_Distance;
                break;
            }
            case InflatePhase_Distance:
                if (!readBits(parser, span, parser.extra, value))
                    return InflateResult_Input;
                parser.distance += value;
                if (!reaches(parser, span, parser.distance))
                    return InflateResult_Invalid;
                parser.phase = InflatePhase_Copy;
                break;
            case InflatePhase_Copy:
                if (!parser.remaining) {
                    parser.phase = InflatePhase_Symbol;
                    break;
                }
                if (span.written == span.capacity)
                    return InflateResult_Output;
                // Copies as much of the match as the output has room for. The copy reads from the history while the
                // match reaches back past this call's output, and then from the output.
                while (parser.remaining != 0 && span.written != span.capacity &&
                        parser.distance > span.written - span.base) {
                    span.output[span.written] = byteBack(parser, span, parser.distance);
                    ++span.written;
                    --parser.remaining;
                }
                for (; parser.remaining != 0 && span.written != span.capacity; --parser.remaining, ++span.written)
                    span.output[span.written] = span.output[span.written - parser.distance];
                break;
            case InflatePhase_Done:
                return InflateResult_Done;
            case InflatePhase_Invalid:
                return InflateResult_Invalid;
        }
    }
}

// Decodes the Deflate data, and adds the output of this call to the history.
InflateResult readDeflate(Deflate& parser, InflateSpan& span) {
    span.base = span.written;
    const InflateResult result = decodeDeflate(parser, span);
    keepHistory(parser, span.output + span.base, span.written - span.base);
    return result;
}

bool readHeader(InflateState& parser, InflateSpan& span, uint32_t size) {
    while (parser.headerSize < size) {
        if (span.consumed == span.input.size)
            return false;
        parser.header[parser.headerSize++] = span.input.data[span.consumed++];
    }
    parser.headerSize = 0;
    return true;
}

void checksumHeader(InflateState& parser, InflateSpan& span, size_t first) {
    const PrByteView bytes{span.input.data ? span.input.data + first : nullptr, span.consumed - first};
    parser.headerCrc = prCrc32Update(parser.headerCrc, bytes);
}

InflateResult readFrame(InflateState& parser, InflateSpan& span, bool final) {
    for (;;) {
        const size_t first = span.consumed;
        switch (parser.frame) {
            case InflateFrame_ZlibHeader:
                if (!readHeader(parser, span, 2))
                    return InflateResult_Input;
                if ((parser.header[0] & 15) != 8 || (parser.header[0] >> 4) > 7 || prMemReadBeU16(parser.header) % 31)
                    return InflateResult_Invalid;
                parser.deflate.windowLimit = 1u << ((parser.header[0] >> 4) + 8);
                parser.frame = (parser.header[1] & 32) ? InflateFrame_DictionaryId : InflateFrame_Data;
                break;
            case InflateFrame_DictionaryId:
                if (!readHeader(parser, span, 4))
                    return InflateResult_Input;
                parser.dictionary = prMemReadBeU32(parser.header);
                parser.frame = InflateFrame_Dictionary;
                break;
            case InflateFrame_Dictionary:
                return InflateResult_Dictionary;
            case InflateFrame_GzipHeader: {
                const bool ready = readHeader(parser, span, 10);
                checksumHeader(parser, span, first);
                if (!ready)
                    return InflateResult_Input;
                if (parser.header[0] != 0x1f || parser.header[1] != 0x8b || parser.header[2] != 8 || (parser.header[3] & 0xe0))
                    return InflateResult_Invalid;
                // Preserve FLG while the scratch header receives the extra length and CRC.
                parser.frame = InflateFrame_ExtraLength;
                break;
            }
            case InflateFrame_ExtraLength:
                if (parser.header[3] & 4) {
                    const bool ready = readHeader(parser, span, 2);
                    checksumHeader(parser, span, first);
                    if (!ready)
                        return InflateResult_Input;
                    parser.remaining = prMemReadLeU16(parser.header);
                } else {
                    parser.remaining = 0;
                }
                parser.frame = InflateFrame_Extra;
                break;
            case InflateFrame_Extra: {
                const size_t available = span.input.size - span.consumed;
                const uint32_t count = available < parser.remaining ? uint32_t(available) : parser.remaining;
                span.consumed += count;
                parser.remaining -= count;
                checksumHeader(parser, span, first);
                if (parser.remaining)
                    return InflateResult_Input;
                parser.frame = InflateFrame_Name;
                break;
            }
            case InflateFrame_Name:
            case InflateFrame_Comment: {
                const uint8_t flag = parser.frame == InflateFrame_Name ? 8 : 16;
                if (parser.header[3] & flag) {
                    while (span.consumed < span.input.size) {
                        if (!span.input.data[span.consumed++]) {
                            parser.header[3] &= uint8_t(~flag);
                            break;
                        }
                    }
                    checksumHeader(parser, span, first);
                    if (parser.header[3] & flag)
                        return InflateResult_Input;
                }
                parser.frame = parser.frame == InflateFrame_Name ? InflateFrame_Comment : InflateFrame_HeaderCrc;
                break;
            }
            case InflateFrame_HeaderCrc:
                if (parser.header[3] & 2) {
                    if (!readHeader(parser, span, 2))
                        return InflateResult_Input;
                    if (prMemReadLeU16(parser.header) != (parser.headerCrc & 0xffff))
                        return InflateResult_Invalid;
                }
                parser.frame = InflateFrame_Data;
                break;
            case InflateFrame_Data: {
                const size_t start = span.written;
                const InflateResult result = readDeflate(parser.deflate, span);
                const PrByteView bytes{span.output ? span.output + start : nullptr, span.written - start};
                if (parser.format == InflateFormat_Zlib)
                    parser.checksum = prAdler32Update(parser.checksum, bytes);
                else if (parser.format == InflateFormat_Gzip)
                    parser.checksum = prCrc32Update(parser.checksum, bytes);
                parser.size += uint32_t(bytes.size);
                if (result != InflateResult_Done)
                    return result;
                parser.frame = parser.format == InflateFormat_Raw ? InflateFrame_Done : InflateFrame_Trailer;
                break;
            }
            case InflateFrame_Trailer:
                if (!readHeader(parser, span, parser.format == InflateFormat_Zlib ? 4 : 8))
                    return InflateResult_Input;
                if (parser.format == InflateFormat_Zlib) {
                    if (prMemReadBeU32(parser.header) != parser.checksum)
                        return InflateResult_Invalid;
                    parser.frame = InflateFrame_Done;
                } else {
                    uint32_t storedChecksum = prMemReadLeU32(parser.header);
                    uint32_t storedSize = prMemReadLeU32(parser.header + 4);
                    if (storedChecksum != parser.checksum || storedSize != parser.size)
                        return InflateResult_Invalid;
                    parser.frame = InflateFrame_Member;
                }
                break;
            case InflateFrame_Member:
                if (span.consumed == span.input.size) {
                    if (!final)
                        return InflateResult_Input;
                    parser.frame = InflateFrame_Done;
                    break;
                }
                resetDeflate(parser.deflate);
                parser.checksum = parser.size = parser.headerCrc = 0;
                parser.frame = InflateFrame_GzipHeader;
                break;
            case InflateFrame_Done:
                return InflateResult_Done;
            case InflateFrame_Invalid:
                return InflateResult_Invalid;
        }
    }
}

} // namespace

struct Inflate : InflateState {};

Inflate* inflateCreate(InflateFormat format) {
    Inflate* parser = allocZeroed<Inflate>();
    inflateReset(parser, format);
    return parser;
}

void inflateReset(Inflate* parser, InflateFormat format) {
    PR_ASSERT(parser && format <= InflateFormat_Gzip);
    resetDeflate(parser->deflate);
    parser->format = format;
    static constexpr InflateFrame frames[] = {InflateFrame_Data, InflateFrame_ZlibHeader, InflateFrame_GzipHeader};
    parser->frame = frames[format];
    parser->headerSize = parser->headerCrc = parser->size = parser->remaining = parser->dictionary = 0;
    parser->checksum = format == InflateFormat_Zlib ? 1 : 0;
}

void inflateDestroy(Inflate* parser) {
    prMemoryFree(parser);
}

uint32_t inflateDictionaryChecksum(const Inflate* parser) {
    PR_ASSERT(parser->frame == InflateFrame_Dictionary);
    return parser->dictionary;
}

bool inflateSetDictionary(Inflate* parser, PrByteView dictionary) {
    PR_ASSERT(parser->frame == InflateFrame_Dictionary);
    PR_ASSERT(dictionary.data != nullptr || dictionary.size == 0);
    if (prAdler32Update(1, dictionary) != parser->dictionary)
        return false;
    Deflate& deflate = parser->deflate;
    uint32_t count = dictionary.size < deflate.windowLimit ? uint32_t(dictionary.size) : deflate.windowLimit;
    if (count != 0)
        prMemCopy(deflate.history, dictionary.data + dictionary.size - count, count);
    deflate.historySize = count;
    deflate.historyAt = count % kHistorySize;
    parser->frame = InflateFrame_Data;
    return true;
}

InflateResult inflateRead(Inflate* parser, PrByteView bytes, bool final, size_t* consumed,
        uint8_t* output, size_t capacity, size_t* written) {
    PR_ASSERT(parser && (bytes.data || !bytes.size));
    PR_ASSERT(consumed && written && (output || !capacity));
    InflateSpan span{bytes, 0, output, capacity, 0, 0};
    InflateResult result = readFrame(*parser, span, final);
    if (result == InflateResult_Input && final)
        result = InflateResult_Truncated;
    if (result == InflateResult_Invalid || result == InflateResult_Truncated)
        parser->frame = InflateFrame_Invalid;
    *consumed = span.consumed;
    *written = span.written;
    return result;
}

} // namespace pr_impl
