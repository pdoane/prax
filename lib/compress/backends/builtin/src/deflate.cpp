// Deflate encoding, bare or framed as zlib or gzip, of a whole input at once.

#include "compress/backends/builtin/src/deflate.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/checksum.h"
#include "pr/count_of.h"
#include "pr/mem.h"
#include "pr/sort.h"

namespace pr_impl {
namespace {

// The size of the window that a match reaches back over, and the shortest and longest match lengths (rfc 1951 3.2.5).
constexpr uint32_t kWindowSize = 32768;
constexpr uint32_t kMinMatch = 3;
constexpr uint32_t kMaxMatch = 258;

// The number of bits in the hash of the three bytes at a position. Each hash heads a chain of earlier positions with
// the same hash.
constexpr uint32_t kHashBits = 15;

// The number of literals and matches that a block holds before it is written.
constexpr uint32_t kBlockTokens = 16384;

// The sizes of the alphabets (rfc 1951 3.2.5, 3.2.7), the end-of-block symbol of the literal and length alphabet, and
// the longest code in each alphabet.
constexpr uint32_t kLiteralSymbols = 286;
constexpr uint32_t kDistanceSymbols = 30;
constexpr uint32_t kCodeLengthSymbols = 19;
constexpr uint32_t kEndOfBlock = 256;
constexpr uint32_t kMaxCodeLength = 15;
constexpr uint32_t kMaxCodeLengthCodeLength = 7;

// The largest stored block (rfc 1951 3.2.4).
constexpr uint32_t kStoredMax = 65535;

// The lengths and distances that each code encodes, and their extra bits (rfc 1951 3.2.5).
constexpr uint16_t kLengthBase[29] = {
    3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
    31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258,
};
constexpr uint8_t kLengthExtra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
    2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0,
};
constexpr uint16_t kDistanceBase[30] = {
    1,    2,    3,    4,    5,    7,     9,     13,    17,  25,
    33,   49,   65,   97,   129,  193,   257,   385,   513, 769,
    1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577,
};
constexpr uint8_t kDistanceExtra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
    6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13,
};

// The order in which a dynamic block stores the lengths of the code length code (rfc 1951 3.2.7).
constexpr uint8_t kCodeLengthOrder[kCodeLengthSymbols] = {
    16, 17, 18, 0, 8,  7, 9,  6, 10, 5,
    11, 4,  12, 3, 13, 2, 14, 1, 15,
};

// The code of each match length and each distance, built at compile time. Distances up to 256 are indexed by distance
// minus one. Longer distances are indexed by distance minus one, divided by 128.
struct SymbolTables {
    uint8_t length[kMaxMatch + 1];  // by length
    uint8_t nearDistance[256];      // by distance minus one
    uint8_t farDistance[256];       // by distance minus one, divided by 128
};

constexpr SymbolTables symbolTables() {
    SymbolTables tables{};
    for (uint32_t code = 0; code < 29; ++code)
        for (uint32_t length = kLengthBase[code]; length < kLengthBase[code] + (1u << kLengthExtra[code]); ++length)
            if (length <= kMaxMatch)
                tables.length[length] = uint8_t(code);
    tables.length[kMaxMatch] = 28;
    for (uint32_t code = 0; code < 30; ++code) {
        for (uint32_t distance =
                kDistanceBase[code]; distance < kDistanceBase[code] + (1u << kDistanceExtra[code]); ++distance) {
            if (distance <= 256)
                tables.nearDistance[distance - 1] = uint8_t(code);
            else
                tables.farDistance[(distance - 1) >> 7] = uint8_t(code);
        }
    }
    return tables;
}

constexpr SymbolTables kSymbols = symbolTables();

inline uint32_t distanceSymbol(uint32_t distance) {
    return distance <= 256 ? kSymbols.nearDistance[distance - 1] : kSymbols.farDistance[(distance - 1) >> 7];
}

// How long each level searches for matches. `chain` is the number of earlier positions it tries, and `nice` is the
// match length at which it stops. If `lazy` is set, it defers a match to look for a longer one at the next position.
struct Effort {
    uint32_t chain;
    uint32_t nice;
    bool lazy;
};

constexpr Effort kEfforts[] = {
    {8, 32, false},     // PrCompressLevel_Fast
    {64, 128, true},    // PrCompressLevel_Default
    {1024, 258, true},  // PrCompressLevel_Small
};

// A literal, or a match of `length` bytes at `distance`. A literal's distance is zero and its length is the byte.
struct Token {
    uint16_t length;
    uint16_t distance;
};

// The bits of the output, first bit lowest.
struct BitWriter {
    uint8_t* out;
    size_t at;
    uint64_t bits;
    uint32_t count;
};

inline void put(BitWriter& w, uint32_t value, uint32_t count) {
    w.bits |= uint64_t(value) << w.count;
    w.count += count;
    while (w.count >= 8) {
        w.out[w.at++] = uint8_t(w.bits);
        w.bits >>= 8;
        w.count -= 8;
    }
}

void alignToByte(BitWriter& w) {
    if (w.count != 0)
        put(w, 0, 8 - w.count);
}

// The code of a symbol and its length. The bits of the code are reversed so that the stream sends the first bit first.
struct Code {
    uint16_t bits;
    uint8_t length;
};

inline void putCode(BitWriter& w, Code code) {
    put(w, code.bits, code.length);
}

// Sets the lengths of a prefix code for symbols with the given `frequencies`, with no code longer than `limit`. The
// lengths are those of the Huffman code. Any code over the limit is shortened, and others are lengthened to keep the
// code complete. A code with one symbol has length one.
void buildLengths(const uint32_t* frequencies, uint32_t count, uint32_t limit, uint8_t* lengths) {
    uint16_t symbols[kLiteralSymbols];
    uint32_t used = 0;
    for (uint32_t symbol = 0; symbol < count; ++symbol) {
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
    sortItems(symbols, used, [&](uint16_t a, uint16_t b) {
        return frequencies[a] != frequencies[b] ? frequencies[a] < frequencies[b] : a < b;
    });

    // The Huffman tree. The leaves come first, in order of frequency, and then the internal nodes in the order that
    // they form, which is also their order of weight.
    uint32_t weights[2 * kLiteralSymbols];
    uint16_t parents[2 * kLiteralSymbols];
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
    // Depths are assigned from the root, which is the last node, down. A parent comes after its children.
    uint8_t depths[2 * kLiteralSymbols];
    depths[next - 1] = 0;
    uint32_t counts[2 * kLiteralSymbols]{};
    for (uint32_t i = next - 1; i-- > 0;) {
        depths[i] = uint8_t(depths[parents[i]] + 1);
        if (i < used)
            ++counts[depths[i]];
    }

    // Codes over the limit move to the limit. Then, one at a time, a code at the limit is dropped and a shorter code
    // splits in two, until the code is complete again.
    for (uint32_t length = limit + 1; length < 2 * kLiteralSymbols; ++length) {
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
    for (uint32_t length = limit; length > 0; --length)
        for (uint32_t k = 0; k < counts[length]; ++k)
            lengths[symbols[at++]] = uint8_t(length);
}

// Builds the canonical codes for `lengths` (rfc 1951 3.2.2).
void buildCodes(const uint8_t* lengths, uint32_t count, Code* codes) {
    uint32_t perLength[kMaxCodeLength + 1]{};
    for (uint32_t symbol = 0; symbol < count; ++symbol)
        ++perLength[lengths[symbol]];
    perLength[0] = 0;
    uint32_t nextCode[kMaxCodeLength + 2]{};
    uint32_t code = 0;
    for (uint32_t length = 1; length <= kMaxCodeLength; ++length) {
        code = (code + perLength[length - 1]) << 1;
        nextCode[length] = code;
    }
    for (uint32_t symbol = 0; symbol < count; ++symbol) {
        const uint32_t length = lengths[symbol];
        uint32_t reversed = 0;
        if (length != 0) {
            const uint32_t value = nextCode[length]++;
            for (uint32_t bit = 0; bit < length; ++bit)
                reversed |= ((value >> bit) & 1) << (length - 1 - bit);
        }
        codes[symbol] = Code{uint16_t(reversed), uint8_t(length)};
    }
}

// The lengths of the fixed codes (rfc 1951 3.2.6).
struct FixedLengths {
    uint8_t literals[288];
    uint8_t distances[kDistanceSymbols];
};

constexpr FixedLengths fixedLengths() {
    FixedLengths fixed{};
    for (uint32_t i = 0; i < 288; ++i)
        fixed.literals[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
    for (uint8_t& length : fixed.distances)
        length = 5;
    return fixed;
}

constexpr FixedLengths kFixedLengths = fixedLengths();

// The encoder's state: the chains of earlier positions by hash, and the block being gathered.
struct Encoder {
    PrByteView input;
    Effort effort;
    int32_t heads[1u << kHashBits];     // by hash, the last position with that hash, or -1
    int32_t chain[kWindowSize];         // by position within the window, the previous position with the same hash
    Token tokens[kBlockTokens];
    uint32_t tokenCount;
    uint32_t literalFrequencies[kLiteralSymbols];
    uint32_t distanceFrequencies[kDistanceSymbols];
    size_t blockStart;                  // the input position where the block's bytes start
    BitWriter out;
};

inline uint32_t hashAt(const uint8_t* bytes) {
    const uint32_t three = uint32_t(bytes[0]) << 16 | uint32_t(bytes[1]) << 8 | bytes[2];
    return (three * 2654435761u) >> (32 - kHashBits);
}

inline void insert(Encoder& e, size_t position) {
    if (position + kMinMatch > e.input.size)
        return;
    const uint32_t hash = hashAt(e.input.data + position);
    e.chain[position & (kWindowSize - 1)] = e.heads[hash];
    e.heads[hash] = int32_t(position);
}

// Returns the length of the longest match at `position` that is longer than `shorter`, and sets `distance` to its
// distance. Returns zero if no earlier position gives such a match.
uint32_t findMatch(const Encoder& e, size_t position, uint32_t shorter, uint32_t* distance) {
    const size_t left = e.input.size - position;
    if (left < kMinMatch)
        return 0;
    const uint32_t limit = left < kMaxMatch ? uint32_t(left) : kMaxMatch;
    const uint8_t* here = e.input.data + position;
    uint32_t best = shorter < kMinMatch - 1 ? kMinMatch - 1 : shorter;
    if (best >= limit)
        return 0;
    uint32_t found = 0;
    int32_t candidate = e.heads[hashAt(here)];
    for (uint32_t tries = e.effort.chain; tries != 0 && candidate >= 0; --tries) {
        const size_t back = position - size_t(candidate);
        if (back == 0 || back > kWindowSize)
            break;
        const uint8_t* there = e.input.data + candidate;
        if (there[best] == here[best] && there[0] == here[0]) {
            uint32_t length = 0;
            while (length < limit && there[length] == here[length])
                ++length;
            if (length > best) {
                best = length;
                found = length;
                *distance = uint32_t(back);
                if (length >= e.effort.nice || length == limit)
                    break;
            }
        }
        const int32_t earlier = e.chain[size_t(candidate) & (kWindowSize - 1)];
        if (earlier >= candidate)
            break;
        candidate = earlier;
    }
    return found;
}

void writeBlock(Encoder& e, size_t end, bool final);

void addToken(Encoder& e, Token token, size_t end) {
    e.tokens[e.tokenCount++] = token;
    if (token.distance == 0) {
        ++e.literalFrequencies[token.length];
    } else {
        ++e.literalFrequencies[257 + kSymbols.length[token.length]];
        ++e.distanceFrequencies[distanceSymbol(token.distance)];
    }
    if (e.tokenCount == kBlockTokens)
        writeBlock(e, end, false);
}

// Returns the number of bits that the tokens take when coded with the code lengths `literals` and `distances`. The
// count includes the end-of-block symbol, whose frequency is one.
uint64_t dataBits(const Encoder& e, const uint8_t* literals, const uint8_t* distances) {
    uint64_t bits = 0;
    for (uint32_t symbol = 0; symbol < kLiteralSymbols; ++symbol) {
        const uint64_t extra = symbol > 256 ? kLengthExtra[symbol - 257] : 0;
        bits += uint64_t(e.literalFrequencies[symbol]) * (literals[symbol] + extra);
    }
    for (uint32_t symbol = 0; symbol < kDistanceSymbols; ++symbol)
        bits += uint64_t(e.distanceFrequencies[symbol]) * (distances[symbol] + kDistanceExtra[symbol]);
    return bits;
}

void writeTokens(Encoder& e, const Code* literals, const Code* distances) {
    for (uint32_t i = 0; i < e.tokenCount; ++i) {
        const Token token = e.tokens[i];
        if (token.distance == 0) {
            putCode(e.out, literals[token.length]);
            continue;
        }
        const uint32_t lengthCode = kSymbols.length[token.length];
        putCode(e.out, literals[257 + lengthCode]);
        put(e.out, token.length - kLengthBase[lengthCode], kLengthExtra[lengthCode]);
        const uint32_t distanceCode = distanceSymbol(token.distance);
        putCode(e.out, distances[distanceCode]);
        put(e.out, token.distance - kDistanceBase[distanceCode], kDistanceExtra[distanceCode]);
    }
    putCode(e.out, literals[kEndOfBlock]);
}

// A run-length coded code length: a code length symbol and its extra bits (rfc 1951 3.2.7).
struct LengthToken {
    uint8_t symbol;
    uint8_t extra;
};

// Codes the code lengths with the repeat symbols. Returns the number of tokens written to `out`.
uint32_t runLengths(const uint8_t* lengths, uint32_t count, LengthToken* out) {
    uint32_t tokens = 0;
    for (uint32_t i = 0; i < count;) {
        const uint8_t length = lengths[i];
        uint32_t run = 1;
        while (i + run < count && lengths[i + run] == length)
            ++run;
        i += run;
        if (length == 0) {
            while (run >= 11) {
                const uint32_t take = run < 138 ? run : 138;
                out[tokens++] = LengthToken{18, uint8_t(take - 11)};
                run -= take;
            }
            if (run >= 3) {
                out[tokens++] = LengthToken{17, uint8_t(run - 3)};
                run = 0;
            }
        } else {
            out[tokens++] = LengthToken{length, 0};
            --run;
            while (run >= 3) {
                const uint32_t take = run < 6 ? run : 6;
                out[tokens++] = LengthToken{16, uint8_t(take - 3)};
                run -= take;
            }
        }
        while (run-- > 0)
            out[tokens++] = LengthToken{length, 0};
    }
    return tokens;
}

// Writes the gathered tokens as a block that covers the input from the start of the block to `end`. The block is
// stored, coded with the fixed codes, or coded with its own codes, whichever takes the fewest bits.
void writeBlock(Encoder& e, size_t end, bool final) {
    e.literalFrequencies[kEndOfBlock] = 1;

    // The block's own codes. Each has at least two symbols, so that each is complete.
    uint32_t literalCounts[kLiteralSymbols];
    memCopy(literalCounts, e.literalFrequencies, sizeof(literalCounts));
    uint32_t distanceCounts[kDistanceSymbols];
    memCopy(distanceCounts, e.distanceFrequencies, sizeof(distanceCounts));
    if (literalCounts[0] == 0 && literalCounts[1] == 0)
        literalCounts[0] = 1;
    uint32_t usedDistances = 0;
    for (uint32_t count : distanceCounts)
        usedDistances += count != 0;
    if (usedDistances < 2) {
        distanceCounts[0] += distanceCounts[0] == 0;
        distanceCounts[1] += distanceCounts[1] == 0;
    }
    uint8_t lengths[kLiteralSymbols + kDistanceSymbols];
    buildLengths(literalCounts, kLiteralSymbols, kMaxCodeLength, lengths);
    uint8_t* distanceLengths = lengths + kLiteralSymbols;
    buildLengths(distanceCounts, kDistanceSymbols, kMaxCodeLength, distanceLengths);
    uint32_t literalCount = kLiteralSymbols;
    while (literalCount > 257 && lengths[literalCount - 1] == 0)
        --literalCount;
    uint32_t distanceCount = kDistanceSymbols;
    while (distanceCount > 1 && distanceLengths[distanceCount - 1] == 0)
        --distanceCount;
    uint8_t sequence[kLiteralSymbols + kDistanceSymbols];
    memCopy(sequence, lengths, literalCount);
    memCopy(sequence + literalCount, distanceLengths, distanceCount);
    LengthToken lengthTokens[kLiteralSymbols + kDistanceSymbols];
    const uint32_t lengthTokenCount = runLengths(sequence, literalCount + distanceCount, lengthTokens);
    uint32_t lengthFrequencies[kCodeLengthSymbols]{};
    for (uint32_t i = 0; i < lengthTokenCount; ++i)
        ++lengthFrequencies[lengthTokens[i].symbol];
    uint8_t lengthLengths[kCodeLengthSymbols];
    buildLengths(lengthFrequencies, kCodeLengthSymbols, kMaxCodeLengthCodeLength, lengthLengths);
    uint32_t lengthCount = kCodeLengthSymbols;
    while (lengthCount > 4 && lengthLengths[kCodeLengthOrder[lengthCount - 1]] == 0)
        --lengthCount;
    uint64_t dynamicBits = 3 + 5 + 5 + 4 + 3 * uint64_t(lengthCount) + dataBits(e, lengths, distanceLengths);
    static constexpr uint8_t kRepeatExtra[3] = {2, 3, 7};
    for (uint32_t i = 0; i < lengthTokenCount; ++i) {
        const uint8_t symbol = lengthTokens[i].symbol;
        dynamicBits += lengthLengths[symbol] + (symbol >= 16 ? kRepeatExtra[symbol - 16] : 0);
    }

    const uint64_t fixedBits = 3 + dataBits(e, kFixedLengths.literals, kFixedLengths.distances);

    // Stored blocks hold at most kStoredMax bytes. Each has a three-bit header, padding bits to the byte boundary, and
    // the length and its complement.
    const size_t size = end - e.blockStart;
    const size_t chunks = size == 0 ? 1 : (size + kStoredMax - 1) / kStoredMax;
    const uint64_t firstPad = (8 - (e.out.count + 3) % 8) % 8;
    const uint64_t storedBits = 3 + firstPad + 32 + (chunks - 1) * (3 + 5 + 32) + 8 * uint64_t(size);

    if (storedBits <= fixedBits && storedBits <= dynamicBits) {
        size_t at = e.blockStart;
        for (size_t c = 0; c < chunks; ++c) {
            const uint32_t length = uint32_t(end - at < kStoredMax ? end - at : kStoredMax);
            put(e.out, final && c + 1 == chunks ? 1 : 0, 1);
            put(e.out, 0, 2);
            alignToByte(e.out);
            put(e.out, length, 16);
            put(e.out, ~length & 0xffff, 16);
            if (length != 0)
                memCopy(e.out.out + e.out.at, e.input.data + at, length);
            e.out.at += length;
            at += length;
        }
    } else if (fixedBits <= dynamicBits) {
        Code literals[288];
        Code distances[kDistanceSymbols];
        buildCodes(kFixedLengths.literals, 288, literals);
        buildCodes(kFixedLengths.distances, kDistanceSymbols, distances);
        put(e.out, final ? 1 : 0, 1);
        put(e.out, 1, 2);
        writeTokens(e, literals, distances);
    } else {
        Code literals[kLiteralSymbols];
        Code distances[kDistanceSymbols];
        Code lengthCodes[kCodeLengthSymbols];
        buildCodes(lengths, kLiteralSymbols, literals);
        buildCodes(distanceLengths, kDistanceSymbols, distances);
        buildCodes(lengthLengths, kCodeLengthSymbols, lengthCodes);
        put(e.out, final ? 1 : 0, 1);
        put(e.out, 2, 2);
        put(e.out, literalCount - 257, 5);
        put(e.out, distanceCount - 1, 5);
        put(e.out, lengthCount - 4, 4);
        for (uint32_t i = 0; i < lengthCount; ++i)
            put(e.out, lengthLengths[kCodeLengthOrder[i]], 3);
        for (uint32_t i = 0; i < lengthTokenCount; ++i) {
            const LengthToken token = lengthTokens[i];
            putCode(e.out, lengthCodes[token.symbol]);
            if (token.symbol >= 16)
                put(e.out, token.extra, kRepeatExtra[token.symbol - 16]);
        }
        writeTokens(e, literals, distances);
    }

    e.tokenCount = 0;
    memZero(e.literalFrequencies, sizeof(e.literalFrequencies));
    memZero(e.distanceFrequencies, sizeof(e.distanceFrequencies));
    e.blockStart = end;
}

// Writes the Deflate data of the input as literals and matches. Matches are found greedily, or with each match deferred
// while the next position has a longer one.
void encodeData(Encoder& e) {
    const size_t size = e.input.size;
    const uint8_t* in = e.input.data;
    size_t position = 0;
    while (position < size) {
        uint32_t distance = 0;
        uint32_t length = findMatch(e, position, 0, &distance);
        insert(e, position);
        if (length == 0) {
            addToken(e, Token{in[position], 0}, position + 1);
            ++position;
            continue;
        }
        while (e.effort.lazy && length < e.effort.nice && position + 1 < size) {
            uint32_t laterDistance = 0;
            const uint32_t later = findMatch(e, position + 1, length, &laterDistance);
            if (later == 0)
                break;
            addToken(e, Token{in[position], 0}, position + 1);
            ++position;
            insert(e, position);
            length = later;
            distance = laterDistance;
        }
        for (size_t covered = position + 1; covered < position + length; ++covered)
            insert(e, covered);
        position += length;
        addToken(e, Token{uint16_t(length), uint16_t(distance)}, position);
    }
    writeBlock(e, size, true);
}

} // namespace

size_t deflateBound(InflateFormat format, size_t size) {
    // Each block takes no more space than it would if stored, and each block other than the last covers at least
    // kBlockTokens bytes. A stored block of up to kStoredMax bytes adds at most six bytes: its header, the padding bits
    // to the byte boundary, and its length and complement.
    const size_t blocks = size / kBlockTokens + 1;
    const size_t data = size + 6 * (size / kStoredMax + blocks) + 1;
    const size_t frame = format == InflateFormat_Gzip ? 18 : format == InflateFormat_Zlib ? 6 : 0;
    return data + frame;
}

size_t deflateEncode(InflateFormat format, PrCompressLevel level, PrByteView input, uint8_t* output) {
    PR_ASSERT(level < countOf(kEfforts));
    Encoder* e = alloc<Encoder>();
    e->input = input;
    e->effort = kEfforts[level];
    memFill(e->heads, 0xff, sizeof(e->heads));
    e->tokenCount = 0;
    memZero(e->literalFrequencies, sizeof(e->literalFrequencies));
    memZero(e->distanceFrequencies, sizeof(e->distanceFrequencies));
    e->blockStart = 0;
    e->out = BitWriter{output, 0, 0, 0};

    if (format == InflateFormat_Zlib) {
        // rfc 1950 2.2: Deflate over a 32K window, the level, and the check bits that make the header a multiple of 31.
        static constexpr uint8_t kLevelBits[] = {1, 2, 3};
        const uint32_t header = 0x7800u | uint32_t(kLevelBits[level]) << 6;
        const uint32_t checked = header + (31 - header % 31) % 31;
        output[0] = uint8_t(checked >> 8);
        output[1] = uint8_t(checked);
        e->out.at = 2;
    } else if (format == InflateFormat_Gzip) {
        // rfc 1952 2.3: the Deflate method, no flags or time, the effort, and an unknown operating system.
        static constexpr uint8_t kEffortFlags[] = {4, 0, 2};
        const uint8_t header[10] = {0x1f, 0x8b, 8, 0, 0, 0, 0, 0, kEffortFlags[level], 255};
        memCopy(output, header, sizeof(header));
        e->out.at = sizeof(header);
    }

    encodeData(*e);
    alignToByte(e->out);
    size_t at = e->out.at;
    prMemoryFree(e);

    if (format == InflateFormat_Zlib) {
        writeBeU32(output + at, adler32Update(1, input));
        at += 4;
    } else if (format == InflateFormat_Gzip) {
        const uint32_t crc = crc32Update(0, input);
        const uint32_t size = uint32_t(input.size);
        for (uint32_t i = 0; i < 4; ++i) {
            output[at + i] = uint8_t(crc >> (8 * i));
            output[at + 4 + i] = uint8_t(size >> (8 * i));
        }
        at += 8;
    }
    PR_ASSERT(at <= deflateBound(format, input.size));
    return at;
}

} // namespace pr_impl
