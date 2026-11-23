// Basis Universal UASTC decoding.
//
// A block is 128 bits, read from the lowest bit of the first byte. It starts with a mode, in a code of two to seven
// bits. Hints for transcoders follow, and a decoder skips them. Next comes the pattern that assigns texels to subsets,
// or the channel that has weights of its own. Then come the endpoints of each subset and a weight for each texel. A
// texel is the two endpoints of its subset, interpolated by its weight with the ASTC interpolation. Endpoints are
// stored in the way that ASTC stores integers of a range that is not a power of two. The trits or quints of all the
// integers come first, packed five or three to a group, and then the bits of each integer. ASTC holds the same
// endpoints and weights in a different layout, so a block becomes an ASTC block without loss.

#include "pr/basis_uastc.h"

#include "pr/bc.h"
#include "pr/mem.h"

namespace pr_impl {

namespace {

constexpr uint32_t kModeSolid = 8;
constexpr uint32_t kModeCount = 19;

// The mode of a block, indexed by the low seven bits of its first byte. The modes have codes of two to seven bits.
constexpr uint8_t kModes[128] = {
    11, 0, 10, 3, 11, 15, 12, 7, 11, 18, 10, 5, 11, 14, 12, 9, 11, 0, 10, 4, 11, 16, 12, 8, 11, 18, 10, 6, 11, 2, 12, 13,
    11, 0, 10, 3, 11, 17, 12, 7, 11, 18, 10, 5, 11, 14, 12, 9, 11, 0, 10, 4, 11, 1, 12, 8, 11, 18, 10, 6, 11, 2, 12, 13,
    11, 0, 10, 3, 11, 19, 12, 7, 11, 18, 10, 5, 11, 14, 12, 9, 11, 0, 10, 4, 11, 16, 12, 8, 11, 18, 10, 6, 11, 2, 12, 13,
    11, 0, 10, 3, 11, 17, 12, 7, 11, 18, 10, 5, 11, 14, 12, 9, 11, 0, 10, 4, 11, 1, 12, 8, 11, 18, 10, 6, 11, 2, 12, 13,
};

// The subset of each texel in the patterns of two subsets. Each texel has two bits, starting at the lowest bits, in
// raster order.
constexpr uint32_t kPatterns2[30] = {
    0x50505050, 0x40404040, 0x01010101, 0x54505040, 0x05151555, 0x55545450, 0x00010515, 0x01051555,
    0x50400000, 0x00000105, 0x55544000, 0x01155555, 0x00000115, 0x00005555, 0x55555500, 0x00555555,
    0x55551501, 0x40545555, 0x00405054, 0x00004050, 0x15050100, 0x50545555, 0x15050501, 0x00404050,
    0x50545455, 0x14141414, 0x55000055, 0x11111111, 0x00550055, 0x05145041,
};

// The patterns of three subsets.
constexpr uint32_t kPatterns3[11] = {
    0xa5a50000, 0xaa005555, 0xaa000055, 0x0000aa55, 0x25252525, 0x94949494, 0x58585858, 0x56560202,
    0x92929292, 0x55aa0055, 0xa05050a0,
};

// The patterns of two subsets that mode 7 alone uses.
constexpr uint32_t kPatternsMode7[19] = {
    0x00005500, 0x10101010, 0x00010505, 0x50504000, 0x55005555, 0x04040404, 0x55555040, 0x50505054,
    0x05500005, 0x00005454, 0x15150000, 0x05000005, 0x00005054, 0x55554000, 0x14555555, 0x01050505,
    0x01015555, 0x01051450, 0x00005455,
};

// The anchor texel of each subset, by pattern. The weight of an anchor texel is stored with one bit fewer.
constexpr uint8_t kAnchors2[30][2] = {
    {0, 2}, {0, 3}, {1, 0}, {0, 3}, {7, 0}, {0, 2}, {3, 0}, {7, 0}, {0, 11}, {2, 0},
    {0, 7}, {11, 0}, {3, 0}, {8, 0}, {0, 4}, {12, 0}, {1, 0}, {8, 0}, {0, 1}, {0, 2},
    {0, 4}, {8, 0}, {1, 0}, {0, 2}, {4, 0}, {0, 1}, {4, 0}, {1, 0}, {4, 0}, {1, 0},
};

// The anchor texels of the patterns of three subsets.
constexpr uint8_t kAnchors3[11][3] = {
    {0, 8, 10}, {8, 0, 12}, {4, 0, 12}, {8, 0, 4}, {3, 0, 2}, {0, 1, 3}, {0, 2, 1},
    {1, 9, 0}, {1, 2, 0}, {4, 0, 8}, {0, 6, 2},
};

// The anchor texels of the patterns of mode 7.
constexpr uint8_t kAnchorsMode7[19][2] = {
    {0, 4}, {0, 2}, {2, 0}, {0, 7}, {8, 0}, {0, 1}, {0, 3}, {0, 1}, {2, 0}, {0, 1},
    {0, 8}, {2, 0}, {0, 1}, {0, 7}, {12, 0}, {2, 0}, {9, 0}, {0, 2}, {4, 0},
};

// The BC7 pattern for each pattern of kPatterns2, and whether the BC7 pattern has the two subsets swapped.
constexpr uint8_t kToBc7Patterns2[30][2] = {
    {0, 0}, {1, 0}, {2, 1}, {3, 0}, {4, 1}, {5, 0}, {6, 1}, {7, 1},
    {8, 0}, {9, 1}, {10, 0}, {11, 1}, {12, 1}, {13, 1}, {14, 0}, {15, 1},
    {17, 1}, {18, 1}, {19, 0}, {20, 0}, {21, 0}, {22, 1}, {23, 1}, {24, 0},
    {25, 1}, {26, 0}, {29, 1}, {32, 1}, {33, 1}, {52, 1},
};

// The BC7 pattern for each pattern of kPatterns3.
constexpr uint8_t kToBc7Patterns3[11] = {4, 8, 9, 10, 11, 12, 13, 20, 35, 36, 57};

// The BC7 pattern of three subsets for each pattern of kPatternsMode7. Two of the three BC7 subsets together make one
// of the two UASTC subsets.
constexpr uint8_t kToBc7PatternsMode7[19] = {10, 11, 0, 2, 8, 13, 1, 33, 40, 20, 21, 58, 3, 32, 59, 34, 20, 14, 31};

// The properties that a mode fixes. They are the bit counts of its code and of a weight, the range of its endpoints,
// the number of subsets, whether each texel has a second weight, the number of channels, and the hints that its blocks
// carry. A mode of two channels is luminance and alpha.
struct Mode {
    uint8_t codeBits;
    uint8_t weightBits;
    uint8_t endpointRange;
    uint8_t subsets;
    bool dualPlane;
    uint8_t channels;
    bool hasBias;
    bool hasSecondHint;
};
constexpr Mode kModeTable[kModeCount] = {
    {4, 4, 19, 1, false, 3, true, true},
    {6, 2, 20, 1, false, 3, true, true},
    {5, 3, 8, 2, false, 3, true, true},
    {5, 2, 7, 3, false, 3, true, true},
    {5, 2, 12, 2, false, 3, true, true},
    {5, 3, 20, 1, false, 3, true, true},
    {5, 2, 18, 1, true, 3, true, true},
    {5, 2, 12, 2, false, 3, true, true},
    {5, 0, 0, 0, false, 4, false, false},
    {5, 2, 8, 2, false, 4, true, true},
    {3, 4, 13, 1, false, 4, false, false},
    {2, 2, 13, 1, true, 4, false, false},
    {3, 3, 19, 1, false, 4, false, false},
    {5, 1, 20, 1, true, 4, true, true},
    {5, 2, 20, 1, false, 4, true, true},
    {7, 4, 20, 1, false, 2, true, true},
    {6, 2, 20, 2, false, 2, true, true},
    {6, 2, 20, 1, true, 2, true, true},
    {4, 5, 11, 1, false, 3, true, true},
};

// The storage of the integers of one endpoint range, and their expansion to eight bits. `bits` is the bit count of an
// integer, and `radix` says whether a trit or a quint sits above the bits. `factor` and `places` are the ASTC constants
// of the range. `factor` multiplies the trit or quint. `places` gives the positions that each bit except the lowest
// takes in the value added to that product. A range of bits alone repeats its bits instead.
struct Range {
    uint8_t range;
    uint8_t bits;
    uint8_t radix;          // 3 for trits, 5 for quints, 0 for neither
    uint8_t factor;
    uint16_t places[5];     // indexed by bit, from the second lowest bit
};
constexpr Range kRanges[] = {
    {7, 2, 3, 93, {0x116}},
    {8, 4, 0, 0, {}},
    {11, 5, 0, 0, {}},
    {12, 3, 5, 26, {0x082, 0x105}},
    {13, 4, 3, 22, {0x041, 0x082, 0x104}},
    {18, 5, 5, 6, {0x020, 0x040, 0x080, 0x101}},
    {19, 6, 3, 5, {0x010, 0x020, 0x040, 0x080, 0x101}},
    {20, 8, 0, 0, {}},
};

// The weight for each index, by the bit count of the index. Weights are out of 64.
constexpr uint8_t kWeights1[2] = {0, 64};
constexpr uint8_t kWeights2[4] = {0, 21, 43, 64};
constexpr uint8_t kWeights3[8] = {0, 9, 18, 27, 37, 46, 55, 64};
constexpr uint8_t kWeights4[16] = {0, 4, 8, 12, 17, 21, 25, 29, 35, 39, 43, 47, 52, 56, 60, 64};
constexpr uint8_t kWeights5[32] = {
    0, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24, 26, 28, 30, 34, 36, 38, 40, 42, 44, 46, 48, 50, 52, 54, 56, 58, 60,
    62, 64,
};
constexpr const uint8_t* kWeights[6] = {nullptr, kWeights1, kWeights2, kWeights3, kWeights4, kWeights5};

// The bit count of a group of trits or quints, by the number of integers that the group holds.
constexpr uint8_t kTritGroupBits[6] = {0, 2, 4, 5, 7, 8};
constexpr uint8_t kQuintGroupBits[4] = {0, 3, 5, 7};

// The index in kRanges of the endpoint range of each mode.
constexpr uint8_t kModeRanges[kModeCount] = {6, 7, 1, 0, 3, 7, 5, 3, 0, 1, 4, 4, 6, 7, 7, 7, 7, 7, 2};

// The digits of each group of trits or quints. A group is indexed as a number of base 3 or 5, with the first digit as
// the lowest. An entry holds two bits per trit, or three bits per quint, starting at the lowest bits.
struct GroupDigits {
    uint16_t trits[243];
    uint16_t quints[125];
};

constexpr GroupDigits groupDigits() {
    GroupDigits made = {};
    for (uint32_t packed = 0; packed < 243; ++packed) {
        uint32_t rest = packed;
        for (uint32_t i = 0; i < 5; ++i, rest /= 3)
            made.trits[packed] = uint16_t(made.trits[packed] | (rest % 3) << (2 * i));
    }
    for (uint32_t packed = 0; packed < 125; ++packed) {
        uint32_t rest = packed;
        for (uint32_t i = 0; i < 3; ++i, rest /= 5)
            made.quints[packed] = uint16_t(made.quints[packed] | (rest % 5) << (3 * i));
    }
    return made;
}
constexpr GroupDigits kGroupDigits = groupDigits();

constexpr bool modeRangesMatch() {
    for (uint32_t m = 0; m < kModeCount; ++m) {
        if (m != kModeSolid && kRanges[kModeRanges[m]].range != kModeTable[m].endpointRange)
            return false;
    }
    return true;
}
static_assert(modeRangesMatch(), "kModeRanges must give the endpoint range of each mode in kModeTable");

// The bits of a block, read from the lowest bit.
struct BlockBits {
    uint64_t low;
    uint64_t high;
    uint32_t at;

    uint32_t get(uint32_t count) {
        if (count == 0)
            return 0;
        uint64_t value = at < 64 ? low >> at : high >> (at - 64);
        if (at < 64 && at + count > 64)
            value |= high << (64 - at);
        at += count;
        return uint32_t(value & ((uint64_t(1) << count) - 1));
    }
};

// Returns the eight-bit value that the stored integer `value` of `range` stands for.
constexpr uint32_t unquantize(const Range& range, uint32_t value) {
    uint32_t bits = range.bits;
    if (range.radix == 0)
        return bits == 8 ? value : bits == 5 ? value << 3 | value >> 2 : value << 4 | value;
    uint32_t digit = value >> bits;
    uint32_t mask = (value & 1) != 0 ? 0x1FF : 0;
    uint32_t added = 0;
    for (uint32_t bit = 1; bit < bits; ++bit) {
        if ((value >> bit & 1) != 0)
            added |= range.places[bit - 1];
    }
    uint32_t result = (digit * range.factor + added) ^ mask;
    return (mask & 0x80) | result >> 2;
}

// The eight-bit value for each stored integer of each range. Ranges are indexed as in kRanges.
struct Unquantized {
    uint8_t values[8][256];
};

constexpr Unquantized unquantized() {
    Unquantized made = {};
    for (uint32_t r = 0; r < 8; ++r) {
        uint32_t count = kRanges[r].radix != 0 ? uint32_t(kRanges[r].radix) << kRanges[r].bits : 1u << kRanges[r].bits;
        for (uint32_t value = 0; value < count; ++value)
            made.values[r][value] = uint8_t(unquantize(kRanges[r], value));
    }
    return made;
}
constexpr Unquantized kUnquantized = unquantized();

// The contents of a stored block. `pattern` is the subset of each texel. `anchors` is the texel of each subset whose
// weight has one bit fewer. `ownChannel` is the channel with weights of its own, or 4 if there is none. `stored` is the
// endpoints of each subset as integers of their range, low then high for each channel. `endpoints` is the same
// endpoints as eight-bit values. `indices` is the weight index of each texel in each plane.
struct Unpacked {
    uint32_t mode;
    uint32_t patternIndex;
    uint32_t pattern;
    const uint8_t* PR_NULLABLE anchors;
    uint32_t ownChannel;
    uint8_t solid[4];
    uint8_t stored[18];
    uint8_t endpoints[3][4][2];
    uint8_t indices[16][2];
};

bool unpack(const uint8_t* block, Unpacked* out) {
    BlockBits bits = {prMemReadLeU64(block), prMemReadLeU64(block + 8), 0};
    uint32_t modeIndex = kModes[block[0] & 127];
    if (modeIndex >= kModeCount)
        return false;
    const Mode& mode = kModeTable[modeIndex];
    out->mode = modeIndex;
    bits.at = mode.codeBits;
    if (modeIndex == kModeSolid) {
        for (uint8_t& channel : out->solid)
            channel = uint8_t(bits.get(8));
        return true;
    }
    // The hints are one or two for BC1, eight bits for ETC1 with five more for its bias, and eight bits for the ETC2
    // alpha.
    bool hasAlpha = mode.channels != 3;
    bits.at += 1 + (mode.hasSecondHint ? 1 : 0) + 8 + (mode.hasBias ? 5 : 0) + (hasAlpha ? 8 : 0);

    out->pattern = 0;
    out->patternIndex = 0;
    out->anchors = nullptr;
    if (mode.subsets == 2) {
        uint32_t index = bits.get(5);
        bool own = modeIndex == 7;
        if (index >= (own ? 19u : 30u))
            return false;
        out->patternIndex = index;
        out->pattern = own ? kPatternsMode7[index] : kPatterns2[index];
        out->anchors = own ? kAnchorsMode7[index] : kAnchors2[index];
    } else if (mode.subsets == 3) {
        uint32_t index = bits.get(4);
        if (index >= 11)
            return false;
        out->patternIndex = index;
        out->pattern = kPatterns3[index];
        out->anchors = kAnchors3[index];
    }
    // The channel with weights of its own. It is alpha if the block is luminance and alpha.
    out->ownChannel = 4;
    if (mode.dualPlane)
        out->ownChannel = mode.channels == 2 ? 3 : bits.get(2);

    uint32_t rangeIndex = kModeRanges[modeIndex];
    const Range& range = kRanges[rangeIndex];
    const uint8_t* unquantize = kUnquantized.values[rangeIndex];
    uint32_t valueCount = uint32_t(mode.channels) * 2 * mode.subsets;
    // The trit or quint digit of each integer, starting at the lowest bits. A trit takes two bits and a quint three.
    uint64_t digits = 0;
    uint32_t digitBits = range.radix == 3 ? 2 : 3;
    if (range.radix != 0) {
        uint32_t group = range.radix == 3 ? 5 : 3;
        for (uint32_t first = 0; first < valueCount; first += group) {
            uint32_t held = valueCount - first < group ? valueCount - first : group;
            uint32_t packed = bits.get(range.radix == 3 ? kTritGroupBits[held] : kQuintGroupBits[held]);
            // The bits of a group can hold a larger number than its digits make. The digits are the remainders of that
            // number.
            uint64_t groupDigits = 0;
            if (packed < (range.radix == 3 ? 243u : 125u)) {
                groupDigits = range.radix == 3 ? kGroupDigits.trits[packed] : kGroupDigits.quints[packed];
            } else {
                for (uint32_t i = 0; i < held; ++i, packed /= range.radix)
                    groupDigits |= uint64_t(packed % range.radix) << (i * digitBits);
            }
            groupDigits &= (uint64_t(1) << (held * digitBits)) - 1;
            digits |= groupDigits << (first * digitBits);
        }
    }
    uint32_t perSubset = mode.channels * 2u;
    uint32_t digitMask = (1u << digitBits) - 1;
    for (uint32_t i = 0; i < valueCount; ++i) {
        uint32_t digit = uint32_t(digits >> (i * digitBits)) & digitMask;
        uint32_t value = digit << range.bits | bits.get(range.bits);
        out->stored[i] = uint8_t(value);
        out->endpoints[i / perSubset][i / 2 % mode.channels][i & 1] = unquantize[value];
    }

    // The weights follow. The anchor texel of each subset has one bit fewer than the others, because the highest bit of
    // its weight is zero. The weights of a block take at most 64 bits, except in mode 18, which is read bit by bit.
    uint32_t anchorMask = 1;
    for (uint32_t s = 0; out->anchors != nullptr && s < mode.subsets; ++s)
        anchorMask |= 1u << out->anchors[s];
    uint32_t planes = mode.dualPlane ? 2 : 1;
    if (modeIndex == 18) {
        for (uint32_t texel = 0; texel < 16; ++texel)
            out->indices[texel][0] = uint8_t(bits.get(mode.weightBits - (anchorMask >> texel & 1)));
        return true;
    }
    uint32_t at = bits.at;
    uint64_t weights = at >= 64 ? bits.high >> (at - 64) : bits.low >> at | bits.high << (64 - at);
    for (uint32_t texel = 0; texel < 16; ++texel) {
        uint32_t count = mode.weightBits - (anchorMask >> texel & 1);
        uint32_t mask = (1u << count) - 1;
        for (uint32_t plane = 0; plane < planes; ++plane) {
            out->indices[texel][plane] = uint8_t(weights & mask);
            weights >>= count;
        }
    }
    return true;
}

// The bits of a BC7 block, written from the lowest bit.
struct Bc7Bits {
    uint64_t words[2];
    uint32_t at;

    void put(uint32_t value, uint32_t count) {
        if (count == 0)
            return;
        uint32_t shift = at & 63;
        words[at >> 6] |= uint64_t(value) << shift;
        if (shift + count > 64)
            words[1] |= uint64_t(value) >> (64 - shift);
        at += count;
    }

    void write(uint8_t* out) const {
        for (uint32_t i = 0; i < 16; ++i)
            out[i] = uint8_t(words[i >> 3] >> (8 * (i & 7)));
    }
};

// The BC7 weights of two and of four bits, out of 64, and tables that convert indices. A table gives the BC7 index
// nearest to each UASTC index, for a BC7 mode whose indices have more or fewer bits than those of the UASTC mode that
// it replaces.
constexpr uint32_t kBc7Weight4Index5 = 21;
constexpr uint8_t kIndex1To2[2] = {0, 3};
constexpr uint8_t kIndex2To4[4] = {0, 5, 10, 15};
constexpr uint8_t kIndex3To4[8] = {0, 2, 4, 6, 9, 11, 13, 15};
constexpr uint8_t kIndex5To4[32] = {
    0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 6, 7, 8, 9, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13, 14, 14, 15, 15,
};

// The BC7 endpoints that come nearest to one 8-bit value, and the error that they leave. Every texel has index 1 in
// mode 5, or index 5 in mode 6. Mode 6 has one entry for each value of its extra bit.
struct SolidEndpoints {
    uint8_t low;
    uint8_t high;
    uint8_t error;
};
struct SolidTables {
    SolidEndpoints mode5[256];
    SolidEndpoints mode6[256][2];
};

SolidEndpoints solidEndpoints(uint32_t value, uint32_t extraBit, bool mode6) {
    SolidEndpoints best = {0, 0, 255};
    uint32_t bestError = 0xFFFFFFFFu;
    for (uint32_t low = 0; low < 128; ++low) {
        uint32_t l = mode6 ? low << 1 | extraBit : low << 1 | low >> 6;
        for (uint32_t high = 0; high < 128; ++high) {
            uint32_t h = mode6 ? high << 1 | extraBit : high << 1 | high >> 6;
            int32_t made = int32_t((l * (64 - kBc7Weight4Index5) + h * kBc7Weight4Index5 + 32) >> 6);
            uint32_t error = uint32_t((made - int32_t(value)) * (made - int32_t(value)));
            if (error < bestError) {
                bestError = error;
                best = SolidEndpoints{uint8_t(low), uint8_t(high), uint8_t(error > 255 ? 255 : error)};
            }
        }
    }
    return best;
}

const SolidTables& solidTables() {
    static const SolidTables tables = [] {
        SolidTables made;
        for (uint32_t value = 0; value < 256; ++value) {
            made.mode5[value] = solidEndpoints(value, 0, false);
            made.mode6[value][0] = solidEndpoints(value, 0, true);
            made.mode6[value][1] = solidEndpoints(value, 1, true);
        }
        return made;
    }();
    return tables;
}

int32_t clampInt(int32_t value, int32_t low, int32_t high) {
    return value < low ? low : value > high ? high : value;
}

// Chooses the extra bit of each endpoint of a subset, or the one bit that both endpoints share if `shared` is set. It
// also chooses the endpoints of `bits` bits that go with the extra bits. The choice leaves the least error over the
// first `channels` channels of the 8-bit endpoints at `low` and `high`. The arithmetic follows the format's
// specification, so that a block transcodes as the encoder expected.
void chooseExtraBits(bool shared, uint32_t channels, uint32_t bits, const uint8_t* low, const uint8_t* high, uint8_t* outLow,
        uint8_t* outHigh, uint32_t* extraBits) {
    uint32_t totalBits = bits + 1;
    int32_t scale = (1 << totalBits) - 1;
    float bestLow = 1e+9f;
    float bestHigh = 1e+9f;
    for (int32_t p = 0; p < 2; ++p) {
        uint8_t quantized[2][4];
        float error[2] = {0, 0};
        float both = 0;
        for (uint32_t c = 0; c < 4; ++c) {
            float squares[2];
            for (uint32_t end = 0; end < 2; ++end) {
                float x = float(end == 0 ? low[c] : high[c]) / 255.0f;
                int32_t q = clampInt(int32_t((x * float(scale) - float(p)) / 2.0f + .5f) * 2 + p, p, scale - 1 + p);
                quantized[end][c] = uint8_t(q);
                uint32_t scaled = uint32_t(q) << (8 - totalBits);
                scaled |= scaled >> totalBits;
                // The reference measures the error of a shared bit in [0, 1], and the error of an endpoint's own bit in
                // [0, 255].
                float difference = shared ? float(scaled) / 255.0f - x : float(scaled) - x * 255.0f;
                squares[end] = difference * difference;
            }
            // Each sum is a separate statement. That keeps the compiler from fusing a product into the sum, so the sum
            // rounds as it does in the reference.
            if (c < channels) {
                error[0] += squares[0];
                error[1] += squares[1];
                float pair = squares[0] + squares[1];
                both += pair;
            }
        }
        if (shared) {
            error[0] = both;
            error[1] = both;
        }
        if (error[0] < bestLow) {
            bestLow = error[0];
            extraBits[0] = uint32_t(p);
            for (uint32_t c = 0; c < 4; ++c)
                outLow[c] = quantized[0][c] >> 1;
        }
        if (error[1] < bestHigh) {
            bestHigh = error[1];
            extraBits[1] = uint32_t(p);
            for (uint32_t c = 0; c < 4; ++c)
                outHigh[c] = quantized[1][c] >> 1;
        }
    }
}

// The properties of a BC7 mode with one plane of indices. They are the bit counts of an endpoint and of an index,
// whether the endpoints have alpha, and whether each endpoint has an extra bit. If `sharedExtraBits` is set, the
// endpoints of a subset share one extra bit.
struct Bc7Mode {
    uint8_t endpointBits;
    uint8_t indexBits;
    bool alpha;
    bool extraBits;
    bool sharedExtraBits;
};
constexpr Bc7Mode kBc7Modes[8] = {
    {}, {6, 3, false, true, true}, {5, 2, false, false, false}, {7, 2, false, true, false}, {}, {},
    {7, 4, true, true, false}, {5, 2, true, true, false},
};

// Writes a BC7 block in mode 1, 2, 3, 6 or 7. The block has `subsets` subsets in the BC7 pattern `partition`. `pattern`
// gives the subset of each texel. `anchors` gives the anchor texel of each subset after the first. The block holds the
// 8-bit endpoints of each subset and an index of the mode's bit count for each texel.
void writeBc7(uint32_t modeIndex, uint32_t subsets, uint32_t partition, uint32_t pattern, const uint8_t* anchors,
        const uint8_t (*endpoints)[4][2], bool opaque, uint8_t* indices, uint8_t* out) {
    const Bc7Mode& mode = kBc7Modes[modeIndex];
    uint32_t channels = mode.alpha ? 4 : 3;
    uint8_t low[3][4] = {};
    uint8_t high[3][4] = {};
    uint32_t extraBits[3][2] = {};
    uint32_t top = (1u << mode.indexBits) - 1;
    for (uint32_t s = 0; s < subsets; ++s) {
        uint8_t from[2][4];
        for (uint32_t c = 0; c < 4; ++c) {
            from[0][c] = endpoints[s][c][0];
            from[1][c] = endpoints[s][c][1];
        }
        if (mode.extraBits) {
            // The extra bits of a block without alpha are chosen for its color alone.
            chooseExtraBits(mode.sharedExtraBits, opaque ? 3 : channels, mode.endpointBits, from[0], from[1], low[s],
                high[s], extraBits[s]);
        } else {
            for (uint32_t c = 0; c < 3; ++c) {
                low[s][c] = uint8_t((from[0][c] * 31u + 127) / 255);
                high[s][c] = uint8_t((from[1][c] * 31u + 127) / 255);
            }
        }
        // BC7 does not store the highest bit of an anchor's index. If that bit is set, the subset swaps its endpoints.
        uint32_t anchor = s == 0 ? 0 : anchors[s - 1];
        if ((indices[anchor] >> (mode.indexBits - 1)) == 0)
            continue;
        for (uint32_t c = 0; c < 4; ++c) {
            uint8_t kept = low[s][c];
            low[s][c] = high[s][c];
            high[s][c] = kept;
        }
        uint32_t keptBit = extraBits[s][0];
        extraBits[s][0] = extraBits[s][1];
        extraBits[s][1] = keptBit;
        for (uint32_t texel = 0; texel < 16; ++texel) {
            if ((pattern >> (2 * texel) & 3) == s)
                indices[texel] = uint8_t(top - indices[texel]);
        }
    }
    Bc7Bits bits = {};
    bits.put(1u << modeIndex, modeIndex + 1);
    if (subsets > 1)
        bits.put(partition, 6);
    for (uint32_t c = 0; c < channels; ++c) {
        for (uint32_t s = 0; s < subsets; ++s) {
            bits.put(low[s][c], mode.endpointBits);
            bits.put(high[s][c], mode.endpointBits);
        }
    }
    for (uint32_t s = 0; mode.extraBits && s < subsets; ++s) {
        bits.put(extraBits[s][0], 1);
        if (!mode.sharedExtraBits)
            bits.put(extraBits[s][1], 1);
    }
    uint32_t anchorMask = 1;
    for (uint32_t s = 1; s < subsets; ++s)
        anchorMask |= 1u << anchors[s - 1];
    for (uint32_t texel = 0; texel < 16; ++texel)
        bits.put(indices[texel], mode.indexBits - (anchorMask >> texel & 1));
    bits.write(out);
}

// Writes a BC7 block in mode 5. The block has color endpoints of 7 bits and alpha endpoints of 8 bits. Color and alpha
// each have their own indices of two bits. `rotation` is the channel that swapped places with alpha, as BC7 numbers it.
void writeBc7Mode5(uint32_t rotation, uint8_t (*color)[2], uint8_t* alpha, uint8_t* colorIndices, uint8_t* alphaIndices,
        uint8_t* out) {
    if (colorIndices[0] >= 2) {
        for (uint32_t c = 0; c < 3; ++c) {
            uint8_t kept = color[c][0];
            color[c][0] = color[c][1];
            color[c][1] = kept;
        }
        for (uint32_t texel = 0; texel < 16; ++texel)
            colorIndices[texel] = uint8_t(3 - colorIndices[texel]);
    }
    if (alphaIndices[0] >= 2) {
        uint8_t kept = alpha[0];
        alpha[0] = alpha[1];
        alpha[1] = kept;
        for (uint32_t texel = 0; texel < 16; ++texel)
            alphaIndices[texel] = uint8_t(3 - alphaIndices[texel]);
    }
    Bc7Bits bits = {};
    bits.put(1u << 5, 6);
    bits.put(rotation, 2);
    for (uint32_t c = 0; c < 3; ++c) {
        bits.put(color[c][0], 7);
        bits.put(color[c][1], 7);
    }
    bits.put(alpha[0], 8);
    bits.put(alpha[1], 8);
    for (uint32_t texel = 0; texel < 16; ++texel)
        bits.put(colorIndices[texel], texel == 0 ? 1 : 2);
    for (uint32_t texel = 0; texel < 16; ++texel)
        bits.put(alphaIndices[texel], texel == 0 ? 1 : 2);
    bits.write(out);
}

// The ASTC 4x4 equivalents of each mode. The block mode has 11 bits, and it fixes the range of the weights and the
// number of planes. The color endpoint mode is 8 for color, 12 for color and alpha, and 4 for luminance and alpha.
constexpr uint16_t kAstcBlockModes[kModeCount] = {
    0x242, 0x42, 0x53, 0x42, 0x42, 0x53, 0x442, 0x42, 0, 0x42, 0x242, 0x442, 0x53, 0x441, 0x42, 0x242, 0x42, 0x442, 0x253,
};
constexpr uint8_t kAstcEndpointModes[kModeCount] = {8, 8, 8, 8, 8, 8, 8, 8, 0, 12, 12, 12, 12, 12, 12, 4, 4, 4, 8};

// The seeds of the ASTC partitions that make each pattern of kPatterns2, kPatterns3 and kPatternsMode7.
constexpr uint16_t kAstcSeeds2[30] = {
    28, 20, 16, 29, 91, 9, 107, 72, 149, 204, 50, 114, 496, 17, 78, 39, 252, 828, 43, 156, 116, 210, 476, 273, 684, 359,
    246, 195, 694, 524,
};
constexpr uint16_t kAstcSeeds3[11] = {260, 74, 32, 156, 183, 15, 745, 0, 335, 902, 254};
constexpr uint16_t kAstcSeedsMode7[19] = {
    36, 48, 61, 137, 161, 183, 226, 281, 302, 307, 479, 495, 593, 594, 605, 799, 812, 988, 993,
};

// The ASTC encodings of five trits in 8 bits and of three quints in 7 bits. A group is indexed as a number of base 3 or
// 5, with the first digit as the lowest. If several encodings decode to the same trits, the table holds the least. Four
// encodings decode to three quints of 4, and the table holds the greatest. Basis Universal's transcoder writes the same
// encodings.
struct AstcIntegerCodes {
    uint8_t trits[243];
    uint8_t quints[125];
};

constexpr AstcIntegerCodes astcIntegerCodes() {
    AstcIntegerCodes codes = {};
    // Decodes each encoding, as in Khronos Data Format Specification 1.3, section 23.12. The loop visits the trit
    // encodings from the greatest down, and the quint encodings from the least up.
    for (uint32_t t = 256; t-- > 0;) {
        uint32_t trits[5] = {};
        uint32_t c = 0;
        if ((t >> 2 & 7) == 7) {
            c = (t >> 5 & 7) << 2 | (t & 3);
            trits[4] = trits[3] = 2;
        } else {
            c = t & 31;
            bool high = (t >> 5 & 3) == 3;
            trits[4] = high ? 2 : t >> 7 & 1;
            trits[3] = high ? t >> 7 & 1 : t >> 5 & 3;
        }
        if ((c & 3) == 3) {
            trits[2] = 2;
            trits[1] = c >> 4 & 1;
            trits[0] = (c >> 3 & 1) << 1 | ((c >> 2 & 1) & ~(c >> 3) & 1);
        } else if ((c >> 2 & 3) == 3) {
            trits[2] = trits[1] = 2;
            trits[0] = c & 3;
        } else {
            trits[2] = c >> 4 & 1;
            trits[1] = c >> 2 & 3;
            trits[0] = (c >> 1 & 1) << 1 | ((c & 1) & ~(c >> 1) & 1);
        }
        codes.trits[trits[0] + 3 * trits[1] + 9 * trits[2] + 27 * trits[3] + 81 * trits[4]] = uint8_t(t);
    }
    for (uint32_t q = 0; q < 128; ++q) {
        uint32_t quints[3] = {};
        if ((q >> 1 & 3) == 3 && (q >> 5 & 3) == 0) {
            uint32_t low = q & 1;
            quints[2] = low << 2 | ((q >> 4 & 1) & ~low) << 1 | ((q >> 3 & 1) & ~low);
            quints[1] = quints[0] = 4;
        } else {
            uint32_t c = q & 31;
            if ((q >> 1 & 3) == 3) {
                quints[2] = 4;
                c = (q >> 3 & 3) << 3 | (~q >> 5 & 3) << 1 | (q & 1);
            } else {
                quints[2] = q >> 5 & 3;
            }
            if ((c & 7) == 5) {
                quints[1] = 4;
                quints[0] = c >> 3 & 3;
            } else {
                quints[1] = c >> 3 & 3;
                quints[0] = c & 7;
            }
        }
        codes.quints[quints[0] + 5 * quints[1] + 25 * quints[2]] = uint8_t(q);
    }
    return codes;
}
constexpr AstcIntegerCodes kAstcIntegerCodes = astcIntegerCodes();

// The bits of an ASTC block. `set` writes a value of `count` bits, at most 32, at any bit position counted from the
// lowest. Bits past the end of the block are dropped.
struct AstcBits {
    uint64_t words[2];

    void set(uint32_t at, uint32_t value, uint32_t count) {
        if (at >= 128)
            return;
        uint32_t shift = at & 63;
        words[at >> 6] |= uint64_t(value) << shift;
        if (at < 64 && shift + count > 64)
            words[1] |= uint64_t(value) >> (64 - shift);
    }
};

uint64_t reverseBits(uint64_t value) {
    value = (value >> 1 & 0x5555555555555555ull) | (value & 0x5555555555555555ull) << 1;
    value = (value >> 2 & 0x3333333333333333ull) | (value & 0x3333333333333333ull) << 2;
    value = (value >> 4 & 0x0F0F0F0F0F0F0F0Full) | (value & 0x0F0F0F0F0F0F0F0Full) << 4;
    value = (value >> 8 & 0x00FF00FF00FF00FFull) | (value & 0x00FF00FF00FF00FFull) << 8;
    value = (value >> 16 & 0x0000FFFF0000FFFFull) | (value & 0x0000FFFF0000FFFFull) << 16;
    return value >> 32 | value << 32;
}

// Writes `count` integers of `range` starting at bit `at`, as ASTC stores them. Integers with trits are in groups of
// five, and integers with quints in groups of three. The trits or quints of a group are encoded together, and the
// encoding is spread between the bits of the group's integers. A last group with fewer integers is written as though
// the missing integers were zero. The encoding bits past the last integer are those of zeros.
void putAstcIntegers(AstcBits* bits, uint32_t at, const uint8_t* values, uint32_t count, const Range& range) {
    uint32_t n = range.bits;
    uint32_t low = (1u << n) - 1;
    if (range.radix == 0) {
        for (uint32_t i = 0; i < count; ++i, at += n)
            bits->set(at, values[i], n);
        return;
    }
    uint32_t group = range.radix == 3 ? 5 : 3;
    // The number of bits of the encoding that follow the bits of each integer of a group. They are two, two, one, two
    // and one for trits, and three, two and two for quints.
    constexpr uint8_t kTritSpans[5] = {2, 2, 1, 2, 1};
    constexpr uint8_t kQuintSpans[3] = {3, 2, 2};
    const uint8_t* spans = range.radix == 3 ? kTritSpans : kQuintSpans;
    for (uint32_t first = 0; first < count; first += group) {
        uint8_t held[5] = {};
        for (uint32_t i = 0; i < group && first + i < count; ++i)
            held[i] = values[first + i];
        uint32_t digits = 0;
        for (uint32_t i = group; i-- > 0;)
            digits = digits * range.radix + (held[i] >> n);
        uint32_t code = range.radix == 3 ? kAstcIntegerCodes.trits[digits] : kAstcIntegerCodes.quints[digits];
        for (uint32_t i = 0; i < group; ++i) {
            bits->set(at, held[i] & low, n);
            at += n;
            bits->set(at, code & ((1u << spans[i]) - 1), spans[i]);
            code >>= spans[i];
            at += spans[i];
        }
    }
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

bool prBasisUastcDecodeBlock(const uint8_t* block, uint8_t* texels) {
    Unpacked unpacked;
    if (!unpack(block, &unpacked))
        return false;
    if (unpacked.mode == kModeSolid) {
        for (uint32_t i = 0; i < 16; ++i)
            prMemCopy(texels + i * 4, unpacked.solid, 4);
        return true;
    }
    const Mode& mode = kModeTable[unpacked.mode];
    const uint8_t* weightTable = kWeights[mode.weightBits];
    uint32_t second = mode.dualPlane ? 1 : 0;
    // The endpoints of each subset as red, green, blue and alpha, expanded to 16 bits. Luminance fills the first three
    // channels. Alpha is opaque if the block has no alpha. Each channel then takes the weight of its plane.
    uint32_t ends[3][2][4];
    for (uint32_t s = 0; s < mode.subsets; ++s) {
        for (uint32_t c = 0; c < 4; ++c) {
            for (uint32_t end = 0; end < 2; ++end) {
                uint32_t value = 255;
                if (mode.channels == 2)
                    value = unpacked.endpoints[s][c == 3 ? 1 : 0][end];
                else if (c < mode.channels)
                    value = unpacked.endpoints[s][c][end];
                ends[s][end][c] = value * 257;
            }
        }
    }
    uint32_t own = mode.channels == 2 ? 3 : unpacked.ownChannel;
    for (uint32_t texel = 0; texel < 16; ++texel) {
        const uint32_t (*subset)[4] = ends[unpacked.pattern >> (2 * texel) & 3];
        uint32_t first = weightTable[unpacked.indices[texel][0]];
        uint32_t other = weightTable[unpacked.indices[texel][second]];
        uint8_t* out = texels + texel * 4;
        // Each channel is its two endpoints interpolated by a weight out of 64. ASTC does the same interpolation at 16
        // bits.
        for (uint32_t c = 0; c < 4; ++c) {
            uint32_t weight = c == own ? other : first;
            out[c] = uint8_t((subset[0][c] * (64 - weight) + subset[1][c] * weight + 32) >> 14);
        }
    }
    return true;
}

bool prBasisUastcTranscodeBc7(const uint8_t* block, uint8_t* out) {
    Unpacked unpacked;
    if (!unpack(block, &unpacked))
        return false;
    if (unpacked.mode == kModeSolid) {
        // Uses mode 6 if it makes the color exactly with either value of the extra bit. Otherwise uses mode 5, whose
        // alpha is exact.
        const SolidTables& tables = solidTables();
        const uint8_t* solid = unpacked.solid;
        uint32_t errors[2] = {0, 0};
        for (uint32_t c = 0; c < 4; ++c) {
            errors[0] += tables.mode6[solid[c]][0].error;
            errors[1] += tables.mode6[solid[c]][1].error;
        }
        Bc7Bits bits = {};
        if (errors[0] > 0 && errors[1] > 0) {
            bits.put(1u << 5, 6);
            bits.put(0, 2);
            for (uint32_t c = 0; c < 3; ++c) {
                bits.put(tables.mode5[solid[c]].low, 7);
                bits.put(tables.mode5[solid[c]].high, 7);
            }
            bits.put(solid[3], 8);
            bits.put(solid[3], 8);
            // Every color index is 1, and the first index has one bit. Every alpha index is zero.
            bits.put(1, 1);
            for (uint32_t texel = 1; texel < 16; ++texel)
                bits.put(1, 2);
        } else {
            uint32_t extraBit = errors[1] < errors[0] ? 1 : 0;
            bits.put(1u << 6, 7);
            for (uint32_t c = 0; c < 4; ++c) {
                bits.put(tables.mode6[solid[c]][extraBit].low, 7);
                bits.put(tables.mode6[solid[c]][extraBit].high, 7);
            }
            bits.put(extraBit, 1);
            bits.put(extraBit, 1);
            // Every index is 5, and the first index has three bits.
            bits.put(5, 3);
            for (uint32_t texel = 1; texel < 16; ++texel)
                bits.put(5, 4);
        }
        bits.write(out);
        return true;
    }
    const Mode& mode = kModeTable[unpacked.mode];
    // The endpoints as red, green, blue and alpha. Luminance fills the first three channels. Alpha is 255 if the block
    // has no alpha.
    uint8_t endpoints[3][4][2];
    for (uint32_t s = 0; s < mode.subsets; ++s) {
        for (uint32_t c = 0; c < 4; ++c) {
            for (uint32_t end = 0; end < 2; ++end) {
                uint8_t value = 255;
                if (mode.channels == 2)
                    value = unpacked.endpoints[s][c == 3 ? 1 : 0][end];
                else if (c < mode.channels)
                    value = unpacked.endpoints[s][c][end];
                endpoints[s][c][end] = value;
            }
        }
    }
    if (mode.dualPlane) {
        // BC7 stores the channel that has its own indices in the alpha position, and stores alpha in the position of
        // that channel.
        uint32_t own = unpacked.ownChannel;
        uint8_t color[3][2];
        uint8_t alpha[2] = {endpoints[0][own][0], endpoints[0][own][1]};
        for (uint32_t c = 0; c < 3; ++c) {
            const uint8_t* from = endpoints[0][c == own ? 3 : c];
            color[c][0] = uint8_t((from[0] * 127u + 127) / 255);
            color[c][1] = uint8_t((from[1] * 127u + 127) / 255);
        }
        uint8_t colorIndices[16];
        uint8_t alphaIndices[16];
        for (uint32_t texel = 0; texel < 16; ++texel) {
            uint8_t first = unpacked.indices[texel][0];
            uint8_t second = unpacked.indices[texel][1];
            colorIndices[texel] = mode.weightBits == 1 ? kIndex1To2[first] : first;
            alphaIndices[texel] = mode.weightBits == 1 ? kIndex1To2[second] : second;
        }
        writeBc7Mode5((own + 1) & 3, color, alpha, colorIndices, alphaIndices, out);
        return true;
    }

    uint8_t indices[16];
    for (uint32_t texel = 0; texel < 16; ++texel)
        indices[texel] = unpacked.indices[texel][0];
    if (mode.subsets == 1 && unpacked.mode != 1) {
        // Mode 6 has indices of four bits.
        const uint8_t* convert = mode.weightBits == 2 ? kIndex2To4 : mode.weightBits == 3 ? kIndex3To4 : kIndex5To4;
        for (uint32_t texel = 0; mode.weightBits != 4 && texel < 16; ++texel)
            indices[texel] = convert[indices[texel]];
        writeBc7(6, 1, 0, 0, nullptr, endpoints, mode.channels == 3, indices, out);
        return true;
    }
    // The subsets of the BC7 pattern, and the UASTC subset that each BC7 subset takes its endpoints from.
    uint32_t partition = 0;
    uint32_t pattern = unpacked.pattern;
    uint32_t subsets = mode.subsets;
    uint8_t anchors[2] = {15, 15};
    uint32_t bc7Mode = 3;
    if (unpacked.mode == 1) {
        // Writes the one subset as the two subsets of the first BC7 pattern, both with the same endpoints.
        pattern = kPatterns2[0];
        subsets = 2;
    } else if (mode.subsets == 3 || unpacked.mode == 7) {
        partition =
            unpacked.mode == 7 ? kToBc7PatternsMode7[unpacked.patternIndex] : kToBc7Patterns3[unpacked.patternIndex];
        const PrBcPartition bc7 = prBcPartition(3, partition);
        pattern = bc7.subsets;
        subsets = 3;
        anchors[0] = bc7.anchors[1];
        anchors[1] = bc7.anchors[2];
        bc7Mode = 2;
    } else {
        partition = kToBc7Patterns2[unpacked.patternIndex][0];
        if (kToBc7Patterns2[unpacked.patternIndex][1] != 0)
            pattern ^= 0x55555555u;
        anchors[0] = prBcPartition(2, partition).anchors[1];
        bc7Mode = unpacked.mode == 2 ? 1 : unpacked.mode == 4 ? 3 : 7;
    }
    uint32_t source[3] = {0, 0, 0};
    for (uint32_t texel = 0; texel < 16; ++texel)
        source[pattern >> (2 * texel) & 3] = unpacked.mode == 1 ? 0 : unpacked.pattern >> (2 * texel) & 3;
    uint8_t arranged[3][4][2];
    for (uint32_t s = 0; s < subsets; ++s)
        prMemCopy(arranged[s], endpoints[source[s]], sizeof(arranged[s]));
    writeBc7(bc7Mode, subsets, partition, pattern, anchors, arranged, mode.channels == 3, indices, out);
    return true;
}

bool prBasisUastcTranscodeAstc(const uint8_t* block, uint8_t* out) {
    Unpacked unpacked;
    if (!unpack(block, &unpacked))
        return false;
    AstcBits bits = {};
    if (unpacked.mode == kModeSolid) {
        // A block of one color, with 16 bits per channel. It has the block mode of a constant color and no extent.
        bits.words[0] = 0xFFFFFFFFFFFFFDFCull;
        for (uint32_t c = 0; c < 4; ++c)
            bits.words[1] |= uint64_t(unpacked.solid[c] * 0x101u) << (16 * c);
        prMemWriteLeU64(out, bits.words[0]);
        prMemWriteLeU64(out + 8, bits.words[1]);
        return true;
    }
    const Mode& mode = kModeTable[unpacked.mode];
    uint32_t planes = mode.dualPlane ? 2 : 1;
    uint32_t valuesPerSubset = mode.channels * 2u;
    uint8_t values[18];
    prMemCopy(values, unpacked.stored, sizeof(values));
    uint8_t weights[32];
    for (uint32_t texel = 0; texel < 16; ++texel) {
        for (uint32_t plane = 0; plane < planes; ++plane)
            weights[texel * planes + plane] = unpacked.indices[texel][plane];
    }
    // If the red, green and blue of the high endpoint sum to less than those of the low endpoint, ASTC reads the color
    // endpoints in the other order, with blue contraction. Such a subset has its endpoints swapped and its weights
    // inverted.
    for (uint32_t s = 0; mode.channels >= 3 && s < mode.subsets; ++s) {
        uint32_t sums[2] = {0, 0};
        for (uint32_t c = 0; c < 3; ++c) {
            sums[0] += unpacked.endpoints[s][c][0];
            sums[1] += unpacked.endpoints[s][c][1];
        }
        if (sums[1] >= sums[0])
            continue;
        for (uint32_t c = 0; c < mode.channels; ++c) {
            uint8_t* pair = values + s * valuesPerSubset + c * 2;
            uint8_t kept = pair[0];
            pair[0] = pair[1];
            pair[1] = kept;
        }
        uint32_t top = (1u << mode.weightBits) - 1;
        for (uint32_t texel = 0; texel < 16; ++texel) {
            if ((unpacked.pattern >> (2 * texel) & 3) != s)
                continue;
            for (uint32_t plane = 0; plane < planes; ++plane)
                weights[texel * planes + plane] = uint8_t(top - weights[texel * planes + plane]);
        }
    }
    // The block mode, the partitions, and the endpoint mode. Every partition shares the endpoint mode.
    uint32_t cem = kAstcEndpointModes[unpacked.mode];
    bits.set(0, kAstcBlockModes[unpacked.mode], 11);
    bits.set(11, mode.subsets - 1u, 2);
    uint32_t at = 13;
    if (mode.subsets == 1) {
        bits.set(at, cem, 4);
        at += 4;
    } else {
        const uint16_t* seeds = mode.subsets == 3 ? kAstcSeeds3 : unpacked.mode == 7 ? kAstcSeedsMode7 : kAstcSeeds2;
        bits.set(at, seeds[unpacked.patternIndex], 10);
        bits.set(at + 10, cem << 2 & 63, 6);
        at += 16;
    }
    // The weights, written from the highest bit down. Below them is the channel that takes the weights of the second
    // plane.
    uint32_t weightCount = 16 * planes;
    if (mode.dualPlane)
        bits.set(128 - weightCount * mode.weightBits - 2, unpacked.ownChannel, 2);
    putAstcIntegers(&bits, at, values, valuesPerSubset * mode.subsets, kRanges[kModeRanges[unpacked.mode]]);
    AstcBits forward = {};
    for (uint32_t i = 0; i < weightCount; ++i)
        forward.set(i * mode.weightBits, weights[i], mode.weightBits);
    prMemWriteLeU64(out, bits.words[0] | reverseBits(forward.words[1]));
    prMemWriteLeU64(out + 8, bits.words[1] | reverseBits(forward.words[0]));
    return true;
}
