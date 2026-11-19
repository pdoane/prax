#include "webp/src/vp8l.h"

#include "pr/mem.h"
#include "pr/memory.h"
#include "pr/raw_array.h"

#include "webp/src/vp8_tables.h"

namespace pr_impl {

bool webpBudgetTake(WebpBudget& budget, uint64_t bytes) {
    if (bytes > budget.limit - budget.used)
        return false;
    budget.used += bytes;
    budget.peak = budget.used > budget.peak ? budget.used : budget.peak;
    return true;
}

void webpBudgetGive(WebpBudget& budget, uint64_t bytes) {
    budget.used -= bytes;
}

bool vp8lReadHeader(PrByteView bytes, Vp8lHeader* out) {
    if (bytes.size < kVp8lHeaderBytes || bytes.data[0] != 0x2F)
        return false;
    const uint32_t bits = prMemReadLeU32(bytes.data + 1);
    if ((bits >> 29) != 0)
        return false;
    out->width = (bits & 0x3FFF) + 1;
    out->height = ((bits >> 14) & 0x3FFF) + 1;
    out->alpha = ((bits >> 28) & 1) != 0;
    return true;
}

namespace {

// The prefix codes' tables: a root table indexed by up to eight bits of the stream, and for longer codes, tables of the
// bits after them (rfc 9649 3.7.2.1).
constexpr uint32_t kRootBits = 8;
constexpr uint32_t kMaxCodeLength = 15;

// The largest table that a complete code of the largest alphabet, 280 symbols and a color cache of 2048, builds with
// eight root bits, as zlib's `enough` counts it.
constexpr uint32_t kMaxTableEntries = 2704;

constexpr uint32_t kLiteralCount = 256;
constexpr uint32_t kLengthCodeCount = 24;
constexpr uint32_t kDistanceCodeCount = 40;
constexpr uint32_t kMaxCacheBits = 11;
constexpr uint32_t kMaxAlphabet = kLiteralCount + kLengthCodeCount + (1u << kMaxCacheBits);

// The order in which a normal code length code gives the lengths of its 19 symbols (rfc 9649 3.7.2.1.2).
constexpr uint32_t kCodeLengthCodes = 19;
constexpr uint8_t kCodeLengthOrder[kCodeLengthCodes] = {
    17, 18, 0, 1, 2, 3, 4, 5, 16, 6, 7, 8, 9, 10, 11, 12, 13, 14,
    15,
};

// The five prefix codes of a group, in the order that the stream gives them (rfc 9649 3.7.2.2.2).
enum {
    Vp8lCode_Green,
    Vp8lCode_Red,
    Vp8lCode_Blue,
    Vp8lCode_Alpha,
    Vp8lCode_Distance,
    Vp8lCode_Count,
};
typedef uint8_t Vp8lCode;

// The transforms (rfc 9649 3.5).
enum {
    Vp8lTransform_Predictor,
    Vp8lTransform_Color,
    Vp8lTransform_SubtractGreen,
    Vp8lTransform_ColorIndexing,
};
typedef uint8_t Vp8lTransform;

// The multiplier of the color cache's hash (rfc 9649 3.6.2.3).
constexpr uint32_t kCacheHash = 0x1E35A7BDu;

// The stream, read low bit first. A read past the end gives zero bits, and leaves `count` below zero: whether the
// stream overran is checked where a decode checks it, not on each read.
struct Bits {
    const uint8_t* data;
    size_t size;
    size_t at;
    uint64_t value;
    int32_t count;

    // Fills `value` with at least 56 bits, or with the rest of the stream. A load of eight bytes may set bits past
    // `count`, which are those that the next fill sets again.
    PR_FORCE_INLINE void fill() {
        if (at + 8 <= size) {
            value |= prMemReadLeU64(data + at) << count;
            at += size_t(63 - count) >> 3;
            count |= 56;
            return;
        }
        fillEnd();
    }

    PR_NOINLINE void fillEnd() {
        while (count <= 56 && at < size) {
            value |= uint64_t(data[at++]) << count;
            count += 8;
        }
    }

    PR_FORCE_INLINE uint32_t peek(uint32_t n) {
        if (count < int32_t(n))
            fill();
        return uint32_t(value & ((uint64_t(1) << n) - 1));
    }

    PR_FORCE_INLINE void skip(uint32_t n) {
        value >>= n;
        count -= int32_t(n);
    }

    bool overrun() const {
        return count < 0;
    }

    uint32_t read(uint32_t n) {
        const uint32_t bits = peek(n);
        skip(n);
        return bits;
    }
};

// An entry of a code's table. In a root table, `bits` past the root's bits link to a second table, `value` entries past
// this entry, that the next `bits` minus the root's bits index. Otherwise `bits` is the length of the code and `value`
// its symbol.
struct Entry {
    uint16_t value;
    uint8_t bits;
};

// A prefix code: the offset of its table in a group set's storage, and the bits that index its root table.
struct Code {
    uint32_t offset;
    uint32_t rootBits;
};

struct Group {
    Code codes[Vp8lCode_Count];
};

uint32_t divRoundUp(uint32_t size, uint32_t bits) {
    return (size + (1u << bits) - 1) >> bits;
}

// Returns `key`, a code reversed in `length` bits, incremented as the code is before reversal.
uint32_t nextKey(uint32_t key, uint32_t length) {
    uint32_t step = 1u << (length - 1);
    while ((key & step) != 0)
        step >>= 1;
    return step != 0 ? (key & (step - 1)) + step : key;
}

// Writes `entry` to every `step`th entry of `table` below `end`.
void replicate(Entry* table, uint32_t step, uint32_t end, Entry entry) {
    do {
        end -= step;
        table[end] = entry;
    } while (end > 0);
}

// Returns the bits of a second table whose codes begin at `length`, which has room for the codes that are left of each
// length.
uint32_t secondTableBits(const uint32_t* counts, uint32_t length, uint32_t rootBits) {
    int32_t left = int32_t(1u << (length - rootBits));
    while (length < kMaxCodeLength) {
        left -= int32_t(counts[length]);
        if (left <= 0)
            break;
        ++length;
        left <<= 1;
    }
    return length - rootBits;
}

// Builds the table of the canonical code that `lengths` give to `count` symbols into `table`, which holds
// kMaxTableEntries, and writes its root bits. Returns the entries that it uses, or zero if the code has no symbol or is
// not complete. A code of one symbol reads no bits.
uint32_t buildCode(const uint8_t* lengths, uint32_t count, Entry* table, uint32_t* outRootBits) {
    uint32_t counts[kMaxCodeLength + 1] = {};
    for (uint32_t symbol = 0; symbol < count; ++symbol)
        counts[lengths[symbol]] += 1;
    counts[0] = 0;
    uint32_t offsets[kMaxCodeLength + 2] = {};
    for (uint32_t length = 1; length <= kMaxCodeLength; ++length)
        offsets[length + 1] = offsets[length] + counts[length];
    const uint32_t symbols = offsets[kMaxCodeLength + 1];
    if (symbols == 0)
        return 0;
    uint16_t sorted[kMaxAlphabet];
    for (uint32_t symbol = 0; symbol < count; ++symbol) {
        if (lengths[symbol] != 0)
            sorted[offsets[lengths[symbol]]++] = uint16_t(symbol);
    }
    if (symbols == 1) {
        table[0] = Entry{sorted[0], 0};
        *outRootBits = 0;
        return 1;
    }
    int32_t open = 1;
    uint32_t longest = 0;
    for (uint32_t length = 1; length <= kMaxCodeLength; ++length) {
        open = open * 2 - int32_t(counts[length]);
        if (open < 0)
            return 0;
        if (counts[length] != 0)
            longest = length;
    }
    if (open != 0)
        return 0;
    const uint32_t rootBits = longest < kRootBits ? longest : kRootBits;
    const uint32_t rootSize = 1u << rootBits;
    uint32_t next = 0;
    uint32_t key = 0;
    for (uint32_t length = 1; length <= rootBits; ++length) {
        for (uint32_t n = 0; n < counts[length]; ++n) {
            replicate(table + key, 1u << length, rootSize, Entry{sorted[next++], uint8_t(length)});
            key = nextKey(key, length);
        }
    }
    uint32_t total = rootSize;
    Entry* second = table;
    uint32_t secondSize = rootSize;
    uint32_t low = 0xFFFFFFFFu;
    const uint32_t mask = rootSize - 1;
    for (uint32_t length = rootBits + 1; length <= kMaxCodeLength; ++length) {
        for (; counts[length] > 0; --counts[length]) {
            if ((key & mask) != low) {
                second += secondSize;
                const uint32_t bits = secondTableBits(counts, length, rootBits);
                secondSize = 1u << bits;
                total += secondSize;
                if (total > kMaxTableEntries)
                    return 0;
                low = key & mask;
                table[low] = Entry{uint16_t(size_t(second - table) - low), uint8_t(bits + rootBits)};
            }
            replicate(second + (key >> rootBits), 1u << (length - rootBits), secondSize,
                Entry{sorted[next++], uint8_t(length - rootBits)});
            key = nextKey(key, length);
        }
    }
    *outRootBits = rootBits;
    return total;
}

PR_FORCE_INLINE uint32_t readSymbol(Bits& in, const Entry* table, uint32_t rootBits) {
    if (in.count < kMaxCodeLength)
        in.fill();
    const uint32_t bits = uint32_t(in.value);
    const Entry* entry = table + (bits & ((1u << rootBits) - 1));
    if (entry->bits > rootBits) {
        const uint32_t extra = entry->bits - rootBits;
        entry += entry->value + ((bits >> rootBits) & ((1u << extra) - 1));
        in.skip(rootBits);
    }
    in.skip(entry->bits);
    return entry->value;
}

// The groups of prefix codes of one image, and the storage of their tables.
struct Groups {
    RawArray<Entry> entries;
    RawArray<Group> groups;
    uint64_t heldBytes;     // what `budget` counts for the two arrays
};

void groupsFree(Groups& groups, WebpBudget& budget) {
    groups.entries.reset();
    groups.groups.reset();
    webpBudgetGive(budget, groups.heldBytes);
    groups.heldBytes = 0;
}

// The working storage of reading prefix codes.
struct CodeScratch {
    Entry table[kMaxTableEntries];
    uint8_t lengths[kMaxAlphabet];
};

// Reads one prefix code of `alphabet` symbols (rfc 9649 3.7.2.1) into `scratch`, and writes the entries that its table
// uses and its root bits. Returns false for a code that breaks the format.
bool readCode(Bits& in, uint32_t alphabet, CodeScratch& scratch, uint32_t* outSize, uint32_t* outRootBits) {
    uint8_t* lengths = scratch.lengths;
    prMemZero(lengths, alphabet);
    if (in.read(1) != 0) {
        const uint32_t symbols = in.read(1) + 1;
        const uint32_t first = in.read(in.read(1) != 0 ? 8 : 1);
        if (first < alphabet)
            lengths[first] = 1;
        if (symbols == 2) {
            const uint32_t second = in.read(8);
            if (second < alphabet)
                lengths[second] = 1;
        }
    } else {
        uint8_t codeLengths[kCodeLengthCodes] = {};
        const uint32_t given = in.read(4) + 4;
        for (uint32_t i = 0; i < given; ++i)
            codeLengths[kCodeLengthOrder[i]] = uint8_t(in.read(3));
        // A code length code's lengths are at most 7, which its root table indexes whole.
        Entry lengthTable[1u << 7];
        uint32_t lengthRoot = 0;
        if (in.overrun() || buildCode(codeLengths, kCodeLengthCodes, lengthTable, &lengthRoot) == 0)
            return false;
        uint32_t tokens = alphabet;
        if (in.read(1) != 0) {
            const uint32_t bits = 2 + 2 * in.read(3);
            tokens = 2 + in.read(bits);
            if (tokens > alphabet)
                return false;
        }
        uint32_t symbol = 0;
        uint8_t previous = 8;
        // `tokens` counts the code lengths and repeats that the stream gives, which may end before the alphabet does.
        while (symbol < alphabet && tokens-- > 0) {
            const uint32_t length = readSymbol(in, lengthTable, lengthRoot);
            if (length < 16) {
                lengths[symbol++] = uint8_t(length);
                if (length != 0)
                    previous = uint8_t(length);
            } else {
                const uint32_t extraBits = length == 16 ? 2 : length == 17 ? 3 : 7;
                const uint32_t repeat = in.read(extraBits) + (length == 18 ? 11 : 3);
                if (repeat > alphabet - symbol)
                    return false;
                prMemFill(lengths + symbol, length == 16 ? previous : 0, repeat);
                symbol += repeat;
            }
            if (in.overrun())
                return false;
        }
    }
    if (in.overrun())
        return false;
    *outSize = buildCode(lengths, alphabet, scratch.table, outRootBits);
    return *outSize != 0;
}

// Reads `count` groups of codes, keeping those that `map` gives an index, or all of them in order if `map` is null.
PrStatus readGroups(Bits& in, uint32_t count, const uint32_t* PR_NULLABLE map, uint32_t kept, uint32_t cacheBits,
        WebpBudget& budget, Groups& out) {
    CodeScratch* scratch = static_cast<CodeScratch*>(prMemoryAlloc(sizeof(CodeScratch)));
    const uint64_t groupBytes = uint64_t(kept) * sizeof(Group);
    if (!webpBudgetTake(budget, groupBytes + sizeof(CodeScratch))) {
        prMemoryFree(scratch);
        return PrStatus_Limit;
    }
    out.heldBytes += groupBytes;
    out.groups.resize(kept);
    const uint32_t alphabets[Vp8lCode_Count] = {
        kLiteralCount + kLengthCodeCount + (cacheBits == 0 ? 0 : 1u << cacheBits),
        kLiteralCount,
        kLiteralCount,
        kLiteralCount,
        kDistanceCodeCount,
    };
    PrStatus status = PrStatus_Ok;
    for (uint32_t index = 0; index < count && status == PrStatus_Ok; ++index) {
        const uint32_t into = map == nullptr ? index : map[index];
        for (uint32_t code = 0; code < Vp8lCode_Count; ++code) {
            uint32_t size = 0;
            uint32_t rootBits = 0;
            if (!readCode(in, alphabets[code], *scratch, &size, &rootBits)) {
                status = PrStatus_InvalidData;
                break;
            }
            if (into == 0xFFFFFFFFu)
                continue;
            if (!webpBudgetTake(budget, uint64_t(size) * sizeof(Entry))) {
                status = PrStatus_Limit;
                break;
            }
            out.heldBytes += uint64_t(size) * sizeof(Entry);
            out.groups.data[into].codes[code] = Code{out.entries.count, rootBits};
            out.entries.append(scratch->table, size);
        }
    }
    webpBudgetGive(budget, sizeof(CodeScratch));
    prMemoryFree(scratch);
    return status;
}

// Takes the bytes of `count` pixels from `budget` and allocates them, or returns null if they do not fit.
uint32_t* PR_NULLABLE takePixels(WebpBudget& budget, uint64_t count) {
    if (!webpBudgetTake(budget, count * 4))
        return nullptr;
    return static_cast<uint32_t*>(prMemoryAlloc(size_t(count) * 4));
}

void givePixels(WebpBudget& budget, uint32_t* PR_NULLABLE pixels, uint64_t count) {
    if (pixels != nullptr) {
        prMemoryFree(pixels);
        webpBudgetGive(budget, count * 4);
    }
}

// Returns the length or distance that a prefix symbol and its extra bits give (rfc 9649 3.6.2.2).
uint32_t prefixValue(Bits& in, uint32_t symbol) {
    if (symbol < 4)
        return symbol + 1;
    const uint32_t extraBits = (symbol - 2) >> 1;
    const uint32_t offset = (2 + (symbol & 1)) << extraBits;
    return offset + in.read(extraBits) + 1;
}

struct Decoder {
    Bits in;
    WebpBudget* budget;
};

PrStatus decodeImage(Decoder& decoder, uint32_t width, uint32_t height, bool main, uint32_t* PR_NULLABLE out);

// Decodes a subimage, which has no transforms and one group of codes, into pixels that `budget` counts.
PrStatus decodeSubimage(Decoder& decoder, uint32_t width, uint32_t height, uint32_t* PR_NULLABLE* PR_NONNULL out) {
    *out = takePixels(*decoder.budget, uint64_t(width) * height);
    if (*out == nullptr)
        return PrStatus_Limit;
    const PrStatus status = decodeImage(decoder, width, height, false, *out);
    if (status != PrStatus_Ok) {
        givePixels(*decoder.budget, *out, uint64_t(width) * height);
        *out = nullptr;
    }
    return status;
}

// Decodes an entropy-coded image: its color cache, its groups of codes, and its coded pixels (rfc 9649 3.6, 3.7). The
// main image may have an entropy image that chooses among several groups. With a null `out`, the main image stops
// before its pixels.
PrStatus decodeImage(Decoder& decoder, uint32_t width, uint32_t height, bool main, uint32_t* PR_NULLABLE out) {
    Bits& in = decoder.in;
    WebpBudget& budget = *decoder.budget;
    uint32_t cacheBits = 0;
    if (in.read(1) != 0) {
        cacheBits = in.read(4);
        if (cacheBits < 1 || cacheBits > kMaxCacheBits)
            return PrStatus_InvalidData;
    }
    uint32_t prefixBits = 0;
    uint32_t* entropy = nullptr;
    uint32_t entropyWidth = 0;
    uint64_t entropyCount = 0;
    uint32_t groupCount = 1;
    uint32_t keptCount = 1;
    uint32_t* map = nullptr;
    uint64_t mapCount = 0;
    PrStatus status = PrStatus_Ok;
    if (main && in.read(1) != 0) {
        prefixBits = in.read(3) + 2;
        entropyWidth = divRoundUp(width, prefixBits);
        const uint32_t entropyHeight = divRoundUp(height, prefixBits);
        entropyCount = uint64_t(entropyWidth) * entropyHeight;
        status = decodeSubimage(decoder, entropyWidth, entropyHeight, &entropy);
        if (status != PrStatus_Ok)
            return status;
        uint32_t largest = 0;
        for (uint64_t i = 0; i < entropyCount; ++i) {
            const uint32_t group = (entropy[i] >> 8) & 0xFFFF;
            largest = group > largest ? group : largest;
        }
        groupCount = largest + 1;
        // The groups that no block names are read and checked, and not kept. Each block's pixel then names the index of
        // its group among those kept.
        mapCount = groupCount;
        map = takePixels(budget, mapCount);
        if (map == nullptr) {
            givePixels(budget, entropy, entropyCount);
            return PrStatus_Limit;
        }
        prMemFill(map, 0xFF, size_t(mapCount) * 4);
        keptCount = 0;
        for (uint64_t i = 0; i < entropyCount; ++i) {
            const uint32_t group = (entropy[i] >> 8) & 0xFFFF;
            if (map[group] == 0xFFFFFFFFu)
                map[group] = keptCount++;
            entropy[i] = map[group];
        }
    }
    Groups groups = {};
    status = readGroups(in, groupCount, map, keptCount, cacheBits, budget, groups);
    givePixels(budget, map, mapCount);
    if (status == PrStatus_Ok && in.overrun())
        status = PrStatus_InvalidData;
    uint32_t* cache = nullptr;
    const uint64_t cacheCount = cacheBits == 0 ? 0 : uint64_t(1) << cacheBits;
    if (status == PrStatus_Ok && cacheCount != 0) {
        cache = takePixels(budget, cacheCount);
        if (cache == nullptr)
            status = PrStatus_Limit;
        else
            prMemZero(cache, size_t(cacheCount) * 4);
    }
    if (status != PrStatus_Ok || out == nullptr) {
        groupsFree(groups, budget);
        givePixels(budget, entropy, entropyCount);
        givePixels(budget, cache, cacheCount);
        return status;
    }

    const Entry* entries = groups.entries.data;
    const uint32_t cacheShift = 32 - cacheBits;
    const uint32_t blockMask = prefixBits == 0 ? 0xFFFFFFFFu : (1u << prefixBits) - 1;
    const size_t total = size_t(width) * height;
    size_t at = 0;
    size_t cached = 0;
    uint32_t x = 0;
    uint32_t y = 0;
    const Group* group = groups.groups.data;
    const auto groupAt = [&](uint32_t column, uint32_t row) {
        if (entropy == nullptr)
            return groups.groups.data;
        return groups.groups.data + entropy[(row >> prefixBits) * entropyWidth + (column >> prefixBits)];
    };
    // The bits are read from a copy of the decoder's, which stays in registers.
    Bits bits = decoder.in;
    while (at < total) {
        if ((x & blockMask) == 0)
            group = groupAt(x, y);
        const Code& green = group->codes[Vp8lCode_Green];
        const uint32_t symbol = readSymbol(bits, entries + green.offset, green.rootBits);
        if (symbol < kLiteralCount) {
            const Code& red = group->codes[Vp8lCode_Red];
            const Code& blue = group->codes[Vp8lCode_Blue];
            const Code& alpha = group->codes[Vp8lCode_Alpha];
            const uint32_t r = readSymbol(bits, entries + red.offset, red.rootBits);
            const uint32_t b = readSymbol(bits, entries + blue.offset, blue.rootBits);
            const uint32_t a = readSymbol(bits, entries + alpha.offset, alpha.rootBits);
            out[at++] = (a << 24) | (r << 16) | (symbol << 8) | b;
            if (++x == width) {
                x = 0;
                ++y;
            }
        } else if (symbol < kLiteralCount + kLengthCodeCount) {
            const uint32_t length = prefixValue(bits, symbol - kLiteralCount);
            const Code& distance = group->codes[Vp8lCode_Distance];
            const uint32_t code = prefixValue(bits, readSymbol(bits, entries + distance.offset, distance.rootBits));
            int64_t offset = 0;
            if (code > 120) {
                offset = int64_t(code) - 120;
            } else {
                offset = int64_t(kVp8lDistanceMap[code - 1][0]) + int64_t(kVp8lDistanceMap[code - 1][1]) * width;
                offset = offset < 1 ? 1 : offset;
            }
            if (uint64_t(offset) > at || length > total - at) {
                status = PrStatus_InvalidData;
                break;
            }
            const uint32_t* from = out + at - size_t(offset);
            for (uint32_t i = 0; i < length; ++i)
                out[at + i] = from[i];
            at += length;
            x += length;
            while (x >= width) {
                x -= width;
                ++y;
            }
            if (at < total)
                group = groupAt(x, y);
        } else {
            const uint32_t index = symbol - kLiteralCount - kLengthCodeCount;
            if (cache == nullptr || index >= cacheCount) {
                status = PrStatus_InvalidData;
                break;
            }
            // The cache holds every pixel before this one.
            while (cached < at) {
                cache[(out[cached] * kCacheHash) >> cacheShift] = out[cached];
                ++cached;
            }
            out[at++] = cache[index];
            if (++x == width) {
                x = 0;
                ++y;
            }
        }
        if (bits.overrun()) {
            status = PrStatus_InvalidData;
            break;
        }
    }
    decoder.in = bits;
    groupsFree(groups, budget);
    givePixels(budget, entropy, entropyCount);
    givePixels(budget, cache, cacheCount);
    return status;
}

// A transform as the stream gives it: its type, the width of the image that its inverse produces, and its data.
struct Transform {
    Vp8lTransform type;
    uint32_t bits;
    uint32_t width;
    uint32_t* data;
    uint64_t dataCount;
};

uint32_t addPixels(uint32_t a, uint32_t b) {
    return (((a & 0xFF00FF00u) + (b & 0xFF00FF00u)) & 0xFF00FF00u) |
        (((a & 0x00FF00FFu) + (b & 0x00FF00FFu)) & 0x00FF00FFu);
}

uint32_t average2(uint32_t a, uint32_t b) {
    return (((a ^ b) & 0xFEFEFEFEu) >> 1) + (a & b);
}

// Clamps a channel's sum, from -255 to 510, to 0 to 255 without a branch: below zero, the complement's top byte is
// zero, and above 255 it is all ones.
uint32_t clamp255(int32_t value) {
    const uint32_t v = uint32_t(value);
    return v < 256 ? v : (~v >> 24);
}

uint32_t channel(uint32_t pixel, uint32_t shift) {
    return (pixel >> shift) & 0xFF;
}

uint32_t select(uint32_t left, uint32_t top, uint32_t topLeft) {
    int32_t towardTop = 0;
    for (uint32_t shift = 0; shift < 32; shift += 8) {
        const int32_t l = int32_t(channel(left, shift));
        const int32_t t = int32_t(channel(top, shift));
        const int32_t c = int32_t(channel(topLeft, shift));
        towardTop += (l - c < 0 ? c - l : l - c) - (t - c < 0 ? c - t : t - c);
    }
    // The estimate L + T - TL is as far from T as L is from TL, and as far from L as T is from TL.
    return towardTop <= 0 ? top : left;
}

uint32_t clampAddSubtractChannel(uint32_t a, uint32_t b, uint32_t c, uint32_t shift) {
    return clamp255(int32_t(channel(a, shift)) + int32_t(channel(b, shift)) - int32_t(channel(c, shift))) << shift;
}

uint32_t clampAddSubtractFull(uint32_t a, uint32_t b, uint32_t c) {
    return clampAddSubtractChannel(a, b, c, 0) | clampAddSubtractChannel(a, b, c, 8) |
        clampAddSubtractChannel(a, b, c, 16) | clampAddSubtractChannel(a, b, c, 24);
}

uint32_t clampAddSubtractHalf(uint32_t a, uint32_t b) {
    uint32_t out = 0;
    for (uint32_t shift = 0; shift < 32; shift += 8) {
        const int32_t x = int32_t(channel(a, shift));
        out |= clamp255(x + (x - int32_t(channel(b, shift))) / 2) << shift;
    }
    return out;
}

// The prediction of one mode (rfc 9649 3.5.1). Modes 14 and 15, which the format leaves undefined, predict black, as
// the reference decoder's do.
PR_FORCE_INLINE uint32_t predict(uint32_t mode, uint32_t left, const uint32_t* top) {
    switch (mode) {
        case 1: return left;
        case 2: return top[0];
        case 3: return top[1];
        case 4: return top[-1];
        case 5: return average2(average2(left, top[1]), top[0]);
        case 6: return average2(left, top[-1]);
        case 7: return average2(left, top[0]);
        case 8: return average2(top[-1], top[0]);
        case 9: return average2(top[0], top[1]);
        case 10: return average2(average2(left, top[-1]), average2(top[0], top[1]));
        case 11: return select(left, top[0], top[-1]);
        case 12: return clampAddSubtractFull(left, top[0], top[-1]);
        case 13: return clampAddSubtractHalf(average2(left, top[0]), top[-1]);
        default: return 0xFF000000u;
    }
}

// Adds one mode's prediction to the pixels of `row` from `begin` to `end`, whose row above is `top`. The mode is a
// template argument, so that the loop of each is its own: those that predict from the row above alone have no chain
// from one pixel to the next, which the compiler vectorizes.
template <uint32_t mode>
void predictRun(uint32_t* row, const uint32_t* top, uint32_t begin, uint32_t end) {
    // The pixel to the left stays in a register from one pixel to the next.
    uint32_t left = row[begin - 1];
    for (uint32_t x = begin; x < end; ++x) {
        left = addPixels(row[x], predict(mode, left, top + x));
        row[x] = left;
    }
}

void predictRun(uint32_t mode, uint32_t* row, const uint32_t* top, uint32_t begin, uint32_t end) {
    switch (mode) {
        case 1: predictRun<1>(row, top, begin, end); break;
        case 2: predictRun<2>(row, top, begin, end); break;
        case 3: predictRun<3>(row, top, begin, end); break;
        case 4: predictRun<4>(row, top, begin, end); break;
        case 5: predictRun<5>(row, top, begin, end); break;
        case 6: predictRun<6>(row, top, begin, end); break;
        case 7: predictRun<7>(row, top, begin, end); break;
        case 8: predictRun<8>(row, top, begin, end); break;
        case 9: predictRun<9>(row, top, begin, end); break;
        case 10: predictRun<10>(row, top, begin, end); break;
        case 11: predictRun<11>(row, top, begin, end); break;
        case 12: predictRun<12>(row, top, begin, end); break;
        case 13: predictRun<13>(row, top, begin, end); break;
        default: predictRun<0>(row, top, begin, end); break;
    }
}

void inversePredictor(const Transform& transform, uint32_t height, uint32_t* pixels) {
    const uint32_t width = transform.width;
    const uint32_t tiles = divRoundUp(width, transform.bits);
    pixels[0] = addPixels(pixels[0], 0xFF000000u);
    for (uint32_t x = 1; x < width; ++x)
        pixels[x] = addPixels(pixels[x], pixels[x - 1]);
    for (uint32_t y = 1; y < height; ++y) {
        uint32_t* row = pixels + size_t(y) * width;
        const uint32_t* top = row - width;
        const uint32_t* modes = transform.data + size_t(y >> transform.bits) * tiles;
        row[0] = addPixels(row[0], top[0]);
        // Each tile of the row is a run of one mode.
        for (uint32_t tile = 0; tile < tiles; ++tile) {
            const uint32_t begin = tile == 0 ? 1 : tile << transform.bits;
            const uint32_t next = (tile + 1) << transform.bits;
            predictRun((modes[tile] >> 8) & 0xF, row, top, begin, next < width ? next : width);
        }
    }
}

int32_t colorDelta(int32_t multiplier, int32_t color) {
    return (multiplier * color) >> 5;
}

// Undoes the color transform over each tile's run of a row with the tile's multipliers, which the compiler vectorizes.
void inverseColor(const Transform& transform, uint32_t height, uint32_t* pixels) {
    const uint32_t width = transform.width;
    const uint32_t tiles = divRoundUp(width, transform.bits);
    for (uint32_t y = 0; y < height; ++y) {
        uint32_t* row = pixels + size_t(y) * width;
        const uint32_t* elements = transform.data + size_t(y >> transform.bits) * tiles;
        for (uint32_t tile = 0; tile < tiles; ++tile) {
            const uint32_t element = elements[tile];
            const int32_t greenToRed = int8_t(uint8_t(element));
            const int32_t greenToBlue = int8_t(uint8_t(element >> 8));
            const int32_t redToBlue = int8_t(uint8_t(element >> 16));
            const uint32_t begin = tile << transform.bits;
            const uint32_t next = (tile + 1) << transform.bits;
            const uint32_t end = next < width ? next : width;
            for (uint32_t x = begin; x < end; ++x) {
                const uint32_t argb = row[x];
                const int32_t green = int8_t(uint8_t(argb >> 8));
                const uint32_t red = uint32_t(int32_t((argb >> 16) & 0xFF) + colorDelta(greenToRed, green)) & 0xFF;
                const uint32_t blue = uint32_t(int32_t(argb & 0xFF) + colorDelta(greenToBlue, green) +
                    colorDelta(redToBlue, int8_t(uint8_t(red)))) & 0xFF;
                row[x] = (argb & 0xFF00FF00u) | (red << 16) | blue;
            }
        }
    }
}

void inverseSubtractGreen(const Transform& transform, uint32_t height, uint32_t* pixels) {
    const size_t count = size_t(transform.width) * height;
    for (size_t i = 0; i < count; ++i) {
        const uint32_t green = (pixels[i] >> 8) & 0xFF;
        pixels[i] = addPixels(pixels[i], (green << 16) | green);
    }
}

// Expands the indices, packed several to a pixel, into the colors of the palette. The rows go from last to first and
// each from right to left, so that each packed pixel is read before an expanded one overwrites it.
void inverseColorIndexing(const Transform& transform, uint32_t height, uint32_t* pixels) {
    const uint32_t width = transform.width;
    const uint32_t packed = divRoundUp(width, transform.bits);
    const uint32_t perPixel = 1u << transform.bits;
    const uint32_t indexBits = 8 >> transform.bits;
    const uint32_t mask = (1u << indexBits) - 1;
    for (uint32_t y = height; y-- > 0;) {
        const uint32_t* from = pixels + size_t(y) * packed;
        uint32_t* to = pixels + size_t(y) * width;
        for (uint32_t x = width; x-- > 0;) {
            const uint32_t index = ((from[x >> transform.bits] >> 8) >> ((x & (perPixel - 1)) * indexBits)) & mask;
            to[x] = transform.data[index];
        }
    }
}

} // namespace

PrStatus vp8lDecode(PrByteView stream, uint32_t width, uint32_t height, WebpBudget& budget, uint32_t* argb) {
    Decoder decoder = {};
    decoder.in.data = stream.data;
    decoder.in.size = stream.size;
    decoder.budget = &budget;
    Bits& in = decoder.in;
    Transform transforms[4] = {};
    uint32_t transformCount = 0;
    bool seen[4] = {};
    uint32_t coded = width;
    PrStatus status = PrStatus_Ok;
    while (status == PrStatus_Ok && in.read(1) != 0) {
        const Vp8lTransform type = Vp8lTransform(in.read(2));
        if (seen[type] || in.overrun()) {
            status = PrStatus_InvalidData;
            break;
        }
        seen[type] = true;
        Transform& transform = transforms[transformCount++];
        transform.type = type;
        transform.width = coded;
        if (type == Vp8lTransform_Predictor || type == Vp8lTransform_Color) {
            transform.bits = in.read(3) + 2;
            const uint32_t tilesX = divRoundUp(coded, transform.bits);
            const uint32_t tilesY = divRoundUp(height, transform.bits);
            transform.dataCount = uint64_t(tilesX) * tilesY;
            status = decodeSubimage(decoder, tilesX, tilesY, &transform.data);
        } else if (type == Vp8lTransform_ColorIndexing) {
            const uint32_t colors = in.read(8) + 1;
            transform.bits = colors > 16 ? 0 : colors > 4 ? 1 : colors > 2 ? 2 : 3;
            // The palette has an entry for each index that a packed pixel can hold. Those past the colors are
            // transparent black.
            uint32_t* palette = nullptr;
            status = decodeSubimage(decoder, colors, 1, &palette);
            if (status == PrStatus_Ok) {
                transform.dataCount = 256;
                transform.data = takePixels(budget, transform.dataCount);
                if (transform.data == nullptr) {
                    status = PrStatus_Limit;
                } else {
                    prMemZero(transform.data, 256 * 4);
                    transform.data[0] = palette[0];
                    for (uint32_t i = 1; i < colors; ++i)
                        transform.data[i] = addPixels(palette[i], transform.data[i - 1]);
                }
                givePixels(budget, palette, colors);
            }
            coded = divRoundUp(coded, transform.bits);
        }
    }
    if (status == PrStatus_Ok && in.overrun())
        status = PrStatus_InvalidData;
    if (status == PrStatus_Ok)
        status = decodeImage(decoder, coded, height, true, argb);
    if (status == PrStatus_Ok && argb != nullptr) {
        for (uint32_t i = transformCount; i-- > 0;) {
            const Transform& transform = transforms[i];
            if (transform.type == Vp8lTransform_Predictor)
                inversePredictor(transform, height, argb);
            else if (transform.type == Vp8lTransform_Color)
                inverseColor(transform, height, argb);
            else if (transform.type == Vp8lTransform_SubtractGreen)
                inverseSubtractGreen(transform, height, argb);
            else
                inverseColorIndexing(transform, height, argb);
        }
    }
    for (uint32_t i = 0; i < transformCount; ++i)
        givePixels(budget, transforms[i].data, transforms[i].dataCount);
    return status;
}

} // namespace pr_impl
