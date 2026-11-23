// BC decoding.
//
// A block is 8 or 16 bytes, and it decodes without any other block. BC stores its integers little-endian and reads the
// bits of a block from the lowest bit of the first byte.

#include "pr/bc.h"

#include "pr/assert.h"
#include "pr/float16.h"
#include "pr/mem.h"
#include "pr/pixels.h"

#include "bc/src/bc7_partitions.h"

namespace pr_impl {

namespace {

// Decodes the BC1 colors at `block` to the texels. If `punchthrough` is set, a block whose first color is not the
// greater has three colors and a fourth that is transparent black. Otherwise the block has four colors, as the color
// blocks of BC2 and BC3 always do (Khronos Data Format Specification 1.4, sections 18.1 to 18.4). The ends are widened
// to 8 bits by repeating their bits, and the colors between them are their thirds or their half in 8-bit integers.
void decodeBc1(const uint8_t* block, bool punchthrough, uint8_t* texels) {
    uint32_t c0 = prMemReadLeU16(block);
    uint32_t c1 = prMemReadLeU16(block + 2);
    uint32_t palette[4][4];
    for (uint32_t i = 0; i < 2; ++i) {
        uint32_t c = i == 0 ? c0 : c1;
        palette[i][0] = prPixelChannel8(c >> 11, 5);
        palette[i][1] = prPixelChannel8(c >> 5 & 63, 6);
        palette[i][2] = prPixelChannel8(c & 31, 5);
        palette[i][3] = 255;
    }
    bool four = c0 > c1 || !punchthrough;
    for (uint32_t c = 0; c < 3; ++c) {
        palette[2][c] = four ? (2 * palette[0][c] + palette[1][c]) / 3 : (palette[0][c] + palette[1][c]) / 2;
        palette[3][c] = four ? (palette[0][c] + 2 * palette[1][c]) / 3 : 0;
    }
    palette[2][3] = 255;
    palette[3][3] = four ? 255 : 0;
    uint32_t indices = prMemReadLeU32(block + 4);
    for (uint32_t i = 0; i < 16; ++i) {
        const uint32_t* color = palette[indices >> (2 * i) & 3];
        for (uint32_t c = 0; c < 4; ++c)
            texels[i * 4 + c] = uint8_t(color[c]);
    }
}

// Decodes the BC4 values at `block` to channel `channel` of the texels: the ends, and the sevenths or the fifths
// between them in 8-bit integers (Khronos Data Format Specification 1.4, section 19.1). BC3's alpha is the same
// (section 18.4).
void decodeBc4(const uint8_t* block, uint32_t channel, uint8_t* texels) {
    uint32_t values[8] = {block[0], block[1]};
    if (values[0] > values[1]) {
        for (uint32_t i = 1; i < 7; ++i)
            values[i + 1] = (values[0] * (7 - i) + values[1] * i) / 7;
    } else {
        for (uint32_t i = 1; i < 5; ++i)
            values[i + 1] = (values[0] * (5 - i) + values[1] * i) / 5;
        values[6] = 0;
        values[7] = 255;
    }
    uint64_t indices = 0;
    for (uint32_t i = 0; i < 6; ++i)
        indices |= uint64_t(block[2 + i]) << (8 * i);
    for (uint32_t i = 0; i < 16; ++i)
        texels[i * 4 + channel] = uint8_t(values[indices >> (3 * i) & 7]);
}

// Reads the bits of a BC7 block, starting with the lowest bit of the first byte.
struct BitReader {
    const uint8_t* bytes;
    uint32_t at = 0;

    uint32_t read(uint32_t count) {
        uint32_t value = 0;
        for (uint32_t i = 0; i < count; ++i, ++at)
            value |= uint32_t(bytes[at >> 3] >> (at & 7) & 1) << i;
        return value;
    }
};

// A BC7 mode. It gives the number of subsets, the bit counts of the pattern, the rotation and the index selection, and
// the bit counts of the colors and alphas. `endpointBits` and `subsetBits` say whether each endpoint or each subset has
// an extra bit of its own. The last two fields are the bit counts of the two sets of indices.
struct Bc7Mode {
    uint8_t subsets;
    uint8_t patternBits;
    uint8_t rotationBits;
    uint8_t selectionBits;
    uint8_t colorBits;
    uint8_t alphaBits;
    uint8_t endpointBits;
    uint8_t subsetBits;
    uint8_t indexBits;
    uint8_t secondIndexBits;
};

constexpr Bc7Mode kBc7Modes[8] = {
    {3, 4, 0, 0, 4, 0, 1, 0, 3, 0}, {2, 6, 0, 0, 6, 0, 0, 1, 3, 0}, {3, 6, 0, 0, 5, 0, 0, 0, 2, 0},
    {2, 6, 0, 0, 7, 0, 1, 0, 2, 0}, {1, 0, 2, 1, 5, 6, 0, 0, 2, 3}, {1, 0, 2, 0, 7, 8, 0, 0, 2, 2},
    {1, 0, 0, 0, 7, 7, 1, 0, 4, 0}, {2, 6, 0, 0, 5, 5, 1, 0, 2, 0},
};

constexpr uint8_t kBc7Weights2[4] = {0, 21, 43, 64};
constexpr uint8_t kBc7Weights3[8] = {0, 9, 18, 27, 37, 46, 55, 64};
constexpr uint8_t kBc7Weights4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

const uint8_t* bc7Weights(uint32_t bits) {
    return bits == 2 ? kBc7Weights2 : bits == 3 ? kBc7Weights3 : kBc7Weights4;
}

void decodeBc7(const uint8_t* block, uint8_t* texels) {
    uint32_t modeIndex = 0;
    while (modeIndex < 8 && (block[0] >> modeIndex & 1) == 0)
        ++modeIndex;
    // A block with no mode decodes to zeros.
    prMemZero(texels, 64);
    if (modeIndex == 8)
        return;
    const Bc7Mode& mode = kBc7Modes[modeIndex];
    BitReader bits{block, modeIndex + 1};
    uint32_t pattern = bits.read(mode.patternBits);
    uint32_t rotation = bits.read(mode.rotationBits);
    uint32_t selection = bits.read(mode.selectionBits);
    uint32_t endpointCount = uint32_t(mode.subsets) * 2;
    uint32_t endpoints[6][4];
    for (uint32_t c = 0; c < 4; ++c) {
        uint32_t count = c < 3 ? mode.colorBits : mode.alphaBits;
        for (uint32_t e = 0; e < endpointCount; ++e)
            endpoints[e][c] = count != 0 ? bits.read(count) : 255;
    }
    uint32_t shared[6] = {};
    if (mode.endpointBits != 0) {
        for (uint32_t e = 0; e < endpointCount; ++e)
            shared[e] = bits.read(1);
    } else if (mode.subsetBits != 0) {
        for (uint32_t s = 0; s < mode.subsets; ++s)
            shared[s * 2] = shared[s * 2 + 1] = bits.read(1);
    }
    bool extra = mode.endpointBits != 0 || mode.subsetBits != 0;
    for (uint32_t e = 0; e < endpointCount; ++e) {
        for (uint32_t c = 0; c < 4; ++c) {
            uint32_t count = c < 3 ? mode.colorBits : mode.alphaBits;
            if (count == 0)
                continue;
            uint32_t value = endpoints[e][c];
            if (extra) {
                value = value << 1 | shared[e];
                ++count;
            }
            // The top bits of the end and its P-bit are replicated into the rest of the byte (Khronos Data Format
            // Specification 1.4, section 20.1).
            endpoints[e][c] = prPixelChannel8(value, count);
        }
    }
    // The subset of each texel, and the anchor texel of each subset. The top index bit of an anchor texel is not
    // stored.
    uint8_t subsets[16] = {};
    uint32_t anchors[3] = {0, 0, 0};
    if (mode.subsets == 2) {
        for (uint32_t i = 0; i < 16; ++i)
            subsets[i] = uint8_t(kBc7Patterns2[pattern] >> i & 1);
        anchors[1] = kBc7Anchors2[pattern];
    } else if (mode.subsets == 3) {
        for (uint32_t i = 0; i < 16; ++i)
            subsets[i] = uint8_t(kBc7Patterns3[pattern] >> (2 * i) & 3);
        anchors[1] = kBc7Anchors3Second[pattern];
        anchors[2] = kBc7Anchors3Third[pattern];
    }
    uint32_t first[16];
    uint32_t second[16] = {};
    for (uint32_t i = 0; i < 16; ++i) {
        bool anchor = i == anchors[subsets[i]];
        first[i] = bits.read(mode.indexBits - (anchor ? 1 : 0));
    }
    for (uint32_t i = 0; mode.secondIndexBits != 0 && i < 16; ++i)
        second[i] = bits.read(mode.secondIndexBits - (i == 0 ? 1 : 0));
    // If the mode has one set of indices, that set weights both the colors and the alphas. If it has two sets, the
    // first weights the colors and the second weights the alphas. The block's selection bit swaps the two sets.
    uint32_t colorBits = mode.indexBits;
    uint32_t alphaBits = mode.secondIndexBits != 0 ? mode.secondIndexBits : mode.indexBits;
    const uint32_t* colorIndices = first;
    const uint32_t* alphaIndices = mode.secondIndexBits != 0 ? second : first;
    if (selection != 0) {
        colorBits = mode.secondIndexBits;
        alphaBits = mode.indexBits;
        colorIndices = second;
        alphaIndices = first;
    }
    for (uint32_t i = 0; i < 16; ++i) {
        const uint32_t* low = endpoints[subsets[i] * 2];
        const uint32_t* high = endpoints[subsets[i] * 2 + 1];
        uint8_t* texel = texels + i * 4;
        for (uint32_t c = 0; c < 4; ++c) {
            uint32_t weight = c < 3 ? bc7Weights(colorBits)[colorIndices[i]] : bc7Weights(alphaBits)[alphaIndices[i]];
            texel[c] = uint8_t((low[c] * (64 - weight) + high[c] * weight + 32) >> 6);
        }
        if (mode.alphaBits == 0)
            texel[3] = 255;
        // A rotation swaps alpha with one of the color channels.
        if (rotation != 0) {
            uint8_t alpha = texel[3];
            texel[3] = texel[rotation - 1];
            texel[rotation - 1] = alpha;
        }
    }
}

// The fields of a BC6H block's endpoints: the four endpoints w, x, y and z of red, green and blue, and the partition. w
// and x are the ends of the first subset, and y and z those of the second.
enum {
    Bc6Field_Rw, Bc6Field_Gw, Bc6Field_Bw, Bc6Field_Rx, Bc6Field_Gx, Bc6Field_Bx, Bc6Field_Ry, Bc6Field_Gy, Bc6Field_By,
    Bc6Field_Rz, Bc6Field_Gz, Bc6Field_Bz, Bc6Field_D,
};
typedef uint8_t Bc6Field;

// A run of the bits of one field, as a mode stores them in turn: from bit `first` to bit `last` of the field, upward,
// or downward where `first` is above `last`.
struct Bc6Run {
    Bc6Field field;
    uint8_t first;
    uint8_t last;
};

// A BC6H mode: whether it has two subsets, whether the other endpoints are deltas from the first, the bits of an
// endpoint, the bits of each channel of the other endpoints, and the runs of its fields after its mode bits.
struct Bc6Mode {
    bool two;
    bool transformed;
    uint8_t endpointBits;
    uint8_t deltaBits[3];
    uint8_t runCount;
    Bc6Run runs[30];
};

// The fourteen modes, as "BC6H Format" lays them out, in the order of their numbers there.
constexpr Bc6Mode kBc6Modes[14] = {
    {
        true, true, 10, {5, 5, 5}, 20, {
            {Bc6Field_Gy, 4, 4}, {Bc6Field_By, 4, 4}, {Bc6Field_Bz, 4, 4}, {Bc6Field_Rw, 0, 9}, {Bc6Field_Gw, 0, 9},
            {Bc6Field_Bw, 0, 9},
            {Bc6Field_Rx, 0, 4}, {Bc6Field_Gz, 4, 4}, {Bc6Field_Gy, 0, 3}, {Bc6Field_Gx, 0, 4}, {Bc6Field_Bz, 0, 0},
            {Bc6Field_Gz, 0, 3},
            {Bc6Field_Bx, 0, 4}, {Bc6Field_Bz, 1, 1}, {Bc6Field_By, 0, 3}, {Bc6Field_Ry, 0, 4}, {Bc6Field_Bz, 2, 2},
            {Bc6Field_Rz, 0, 4},
            {Bc6Field_Bz, 3, 3}, {Bc6Field_D, 0, 4},
        },
    },
    {
        true, true, 7, {6, 6, 6}, 24, {
            {Bc6Field_Gy, 5, 5}, {Bc6Field_Gz, 4, 4}, {Bc6Field_Gz, 5, 5}, {Bc6Field_Rw, 0, 6}, {Bc6Field_Bz, 0, 0},
            {Bc6Field_Bz, 1, 1},
            {Bc6Field_By, 4, 4}, {Bc6Field_Gw, 0, 6}, {Bc6Field_By, 5, 5}, {Bc6Field_Bz, 2, 2}, {Bc6Field_Gy, 4, 4},
            {Bc6Field_Bw, 0, 6},
            {Bc6Field_Bz, 3, 3}, {Bc6Field_Bz, 5, 5}, {Bc6Field_Bz, 4, 4}, {Bc6Field_Rx, 0, 5}, {Bc6Field_Gy, 0, 3},
            {Bc6Field_Gx, 0, 5},
            {Bc6Field_Gz, 0, 3}, {Bc6Field_Bx, 0, 5}, {Bc6Field_By, 0, 3}, {Bc6Field_Ry, 0, 5}, {Bc6Field_Rz, 0, 5},
            {Bc6Field_D, 0, 4},
        },
    },
    {
        true, true, 11, {5, 4, 4}, 19, {
            {Bc6Field_Rw, 0, 9}, {Bc6Field_Gw, 0, 9}, {Bc6Field_Bw, 0, 9}, {Bc6Field_Rx, 0, 4}, {Bc6Field_Rw, 10, 10},
            {Bc6Field_Gy, 0, 3},
            {Bc6Field_Gx, 0, 3}, {Bc6Field_Gw, 10, 10}, {Bc6Field_Bz, 0, 0}, {Bc6Field_Gz, 0, 3}, {Bc6Field_Bx, 0, 3},
            {Bc6Field_Bw, 10, 10},
            {Bc6Field_Bz, 1, 1}, {Bc6Field_By, 0, 3}, {Bc6Field_Ry, 0, 4}, {Bc6Field_Bz, 2, 2}, {Bc6Field_Rz, 0, 4},
            {Bc6Field_Bz, 3, 3},
            {Bc6Field_D, 0, 4},
        },
    },
    {
        true, true, 11, {4, 5, 4}, 21, {
            {Bc6Field_Rw, 0, 9}, {Bc6Field_Gw, 0, 9}, {Bc6Field_Bw, 0, 9}, {Bc6Field_Rx, 0, 3}, {Bc6Field_Rw, 10, 10},
            {Bc6Field_Gz, 4, 4},
            {Bc6Field_Gy, 0, 3}, {Bc6Field_Gx, 0, 4}, {Bc6Field_Gw, 10, 10}, {Bc6Field_Gz, 0, 3}, {Bc6Field_Bx, 0, 3},
            {Bc6Field_Bw, 10, 10},
            {Bc6Field_Bz, 1, 1}, {Bc6Field_By, 0, 3}, {Bc6Field_Ry, 0, 3}, {Bc6Field_Bz, 0, 0}, {Bc6Field_Bz, 2, 2},
            {Bc6Field_Rz, 0, 3},
            {Bc6Field_Gy, 4, 4}, {Bc6Field_Bz, 3, 3}, {Bc6Field_D, 0, 4},
        },
    },
    {
        true, true, 11, {4, 4, 5}, 21, {
            {Bc6Field_Rw, 0, 9}, {Bc6Field_Gw, 0, 9}, {Bc6Field_Bw, 0, 9}, {Bc6Field_Rx, 0, 3}, {Bc6Field_Rw, 10, 10},
            {Bc6Field_By, 4, 4},
            {Bc6Field_Gy, 0, 3}, {Bc6Field_Gx, 0, 3}, {Bc6Field_Gw, 10, 10}, {Bc6Field_Bz, 0, 0}, {Bc6Field_Gz, 0, 3},
            {Bc6Field_Bx, 0, 4},
            {Bc6Field_Bw, 10, 10}, {Bc6Field_By, 0, 3}, {Bc6Field_Ry, 0, 3}, {Bc6Field_Bz, 1, 1}, {Bc6Field_Bz, 2, 2},
            {Bc6Field_Rz, 0, 3},
            {Bc6Field_Bz, 4, 4}, {Bc6Field_Bz, 3, 3}, {Bc6Field_D, 0, 4},
        },
    },
    {
        true, true, 9, {5, 5, 5}, 20, {
            {Bc6Field_Rw, 0, 8}, {Bc6Field_By, 4, 4}, {Bc6Field_Gw, 0, 8}, {Bc6Field_Gy, 4, 4}, {Bc6Field_Bw, 0, 8},
            {Bc6Field_Bz, 4, 4},
            {Bc6Field_Rx, 0, 4}, {Bc6Field_Gz, 4, 4}, {Bc6Field_Gy, 0, 3}, {Bc6Field_Gx, 0, 4}, {Bc6Field_Bz, 0, 0},
            {Bc6Field_Gz, 0, 3},
            {Bc6Field_Bx, 0, 4}, {Bc6Field_Bz, 1, 1}, {Bc6Field_By, 0, 3}, {Bc6Field_Ry, 0, 4}, {Bc6Field_Bz, 2, 2},
            {Bc6Field_Rz, 0, 4},
            {Bc6Field_Bz, 3, 3}, {Bc6Field_D, 0, 4},
        },
    },
    {
        true, true, 8, {6, 5, 5}, 20, {
            {Bc6Field_Rw, 0, 7}, {Bc6Field_Gz, 4, 4}, {Bc6Field_By, 4, 4}, {Bc6Field_Gw, 0, 7}, {Bc6Field_Bz, 2, 2},
            {Bc6Field_Gy, 4, 4},
            {Bc6Field_Bw, 0, 7}, {Bc6Field_Bz, 3, 3}, {Bc6Field_Bz, 4, 4}, {Bc6Field_Rx, 0, 5}, {Bc6Field_Gy, 0, 3},
            {Bc6Field_Gx, 0, 4},
            {Bc6Field_Bz, 0, 0}, {Bc6Field_Gz, 0, 3}, {Bc6Field_Bx, 0, 4}, {Bc6Field_Bz, 1, 1}, {Bc6Field_By, 0, 3},
            {Bc6Field_Ry, 0, 5},
            {Bc6Field_Rz, 0, 5}, {Bc6Field_D, 0, 4},
        },
    },
    {
        true, true, 8, {5, 6, 5}, 22, {
            {Bc6Field_Rw, 0, 7}, {Bc6Field_Bz, 0, 0}, {Bc6Field_By, 4, 4}, {Bc6Field_Gw, 0, 7}, {Bc6Field_Gy, 5, 5},
            {Bc6Field_Gy, 4, 4},
            {Bc6Field_Bw, 0, 7}, {Bc6Field_Gz, 5, 5}, {Bc6Field_Bz, 4, 4}, {Bc6Field_Rx, 0, 4}, {Bc6Field_Gz, 4, 4},
            {Bc6Field_Gy, 0, 3},
            {Bc6Field_Gx, 0, 5}, {Bc6Field_Gz, 0, 3}, {Bc6Field_Bx, 0, 4}, {Bc6Field_Bz, 1, 1}, {Bc6Field_By, 0, 3},
            {Bc6Field_Ry, 0, 4},
            {Bc6Field_Bz, 2, 2}, {Bc6Field_Rz, 0, 4}, {Bc6Field_Bz, 3, 3}, {Bc6Field_D, 0, 4},
        },
    },
    {
        true, true, 8, {5, 5, 6}, 22, {
            {Bc6Field_Rw, 0, 7}, {Bc6Field_Bz, 1, 1}, {Bc6Field_By, 4, 4}, {Bc6Field_Gw, 0, 7}, {Bc6Field_By, 5, 5},
            {Bc6Field_Gy, 4, 4},
            {Bc6Field_Bw, 0, 7}, {Bc6Field_Bz, 5, 5}, {Bc6Field_Bz, 4, 4}, {Bc6Field_Rx, 0, 4}, {Bc6Field_Gz, 4, 4},
            {Bc6Field_Gy, 0, 3},
            {Bc6Field_Gx, 0, 4}, {Bc6Field_Bz, 0, 0}, {Bc6Field_Gz, 0, 3}, {Bc6Field_Bx, 0, 5}, {Bc6Field_By, 0, 3},
            {Bc6Field_Ry, 0, 4},
            {Bc6Field_Bz, 2, 2}, {Bc6Field_Rz, 0, 4}, {Bc6Field_Bz, 3, 3}, {Bc6Field_D, 0, 4},
        },
    },
    {
        true, false, 6, {6, 6, 6}, 24, {
            {Bc6Field_Rw, 0, 5}, {Bc6Field_Gz, 4, 4}, {Bc6Field_Bz, 0, 0}, {Bc6Field_Bz, 1, 1}, {Bc6Field_By, 4, 4},
            {Bc6Field_Gw, 0, 5},
            {Bc6Field_Gy, 5, 5}, {Bc6Field_By, 5, 5}, {Bc6Field_Bz, 2, 2}, {Bc6Field_Gy, 4, 4}, {Bc6Field_Bw, 0, 5},
            {Bc6Field_Gz, 5, 5},
            {Bc6Field_Bz, 3, 3}, {Bc6Field_Bz, 5, 5}, {Bc6Field_Bz, 4, 4}, {Bc6Field_Rx, 0, 5}, {Bc6Field_Gy, 0, 3},
            {Bc6Field_Gx, 0, 5},
            {Bc6Field_Gz, 0, 3}, {Bc6Field_Bx, 0, 5}, {Bc6Field_By, 0, 3}, {Bc6Field_Ry, 0, 5}, {Bc6Field_Rz, 0, 5},
            {Bc6Field_D, 0, 4},
        },
    },
    {
        false, false, 10, {10, 10, 10}, 6, {
            {Bc6Field_Rw, 0, 9}, {Bc6Field_Gw, 0, 9}, {Bc6Field_Bw, 0, 9}, {Bc6Field_Rx, 0, 9}, {Bc6Field_Gx, 0, 9},
            {Bc6Field_Bx, 0, 9},
        },
    },
    {
        false, true, 11, {9, 9, 9}, 9, {
            {Bc6Field_Rw, 0, 9}, {Bc6Field_Gw, 0, 9}, {Bc6Field_Bw, 0, 9}, {Bc6Field_Rx, 0, 8}, {Bc6Field_Rw, 10, 10},
            {Bc6Field_Gx, 0, 8},
            {Bc6Field_Gw, 10, 10}, {Bc6Field_Bx, 0, 8}, {Bc6Field_Bw, 10, 10},
        },
    },
    {
        false, true, 12, {8, 8, 8}, 9, {
            {Bc6Field_Rw, 0, 9}, {Bc6Field_Gw, 0, 9}, {Bc6Field_Bw, 0, 9}, {Bc6Field_Rx, 0, 7}, {Bc6Field_Rw, 11, 10},
            {Bc6Field_Gx, 0, 7},
            {Bc6Field_Gw, 11, 10}, {Bc6Field_Bx, 0, 7}, {Bc6Field_Bw, 11, 10},
        },
    },
    {
        false, true, 16, {4, 4, 4}, 9, {
            {Bc6Field_Rw, 0, 9}, {Bc6Field_Gw, 0, 9}, {Bc6Field_Bw, 0, 9}, {Bc6Field_Rx, 0, 3}, {Bc6Field_Rw, 15, 10},
            {Bc6Field_Gx, 0, 3},
            {Bc6Field_Gw, 15, 10}, {Bc6Field_Bx, 0, 3}, {Bc6Field_Bw, 15, 10},
        },
    },
};

// The mode of each value of a block's first five bits, or -1 for the four that are reserved. A block whose first two
// bits are 0 or 1 has a mode of two bits.
constexpr int8_t kBc6ModeOfBits[32] = {
    0, 1, 2, 10, 0, 1, 3, 11, 0, 1, 4, 12, 0, 1, 5, 13, 0, 1, 6, -1, 0, 1, 7, -1, 0, 1, 8, -1, 0, 1, 9, -1,
};

// Returns the `bits`-bit value `value` as a signed integer.
int32_t signExtend(uint32_t value, uint32_t bits) {
    const uint32_t sign = 1u << (bits - 1);
    return int32_t((value & (sign * 2 - 1)) ^ sign) - int32_t(sign);
}

// Returns an endpoint of `bits` bits as the 16-bit value that the interpolation works on.
int32_t unquantizeBc6(int32_t value, uint32_t bits, bool isSigned) {
    if (!isSigned) {
        if (bits >= 15 || value == 0)
            return value;
        if (value == int32_t((1u << bits) - 1))
            return 0xffff;
        return int32_t(((uint32_t(value) << 16) + 0x8000) >> bits);
    }
    if (bits >= 16)
        return value;
    const bool negative = value < 0;
    int32_t magnitude = negative ? -value : value;
    if (magnitude == 0)
        return 0;
    if (magnitude >= int32_t(1u << (bits - 1)) - 1)
        magnitude = 0x7fff;
    else
        magnitude = int32_t(((uint32_t(magnitude) << 15) + 0x4000) >> (bits - 1));
    return negative ? -magnitude : magnitude;
}

// Returns the half float that an interpolated value is.
float finishBc6(int32_t value, bool isSigned) {
    uint32_t bits = 0;
    if (!isSigned)
        bits = uint32_t(value * 31) >> 6;
    else if (value < 0)
        bits = 0x8000 | uint32_t((-value * 31) >> 5);
    else
        bits = uint32_t((value * 31) >> 5);
    PrFloat16 half{};
    const uint16_t stored = uint16_t(bits);
    prMemCopy(&half, &stored, sizeof(stored));
    return prFloat16ToFloat(half);
}

// Decodes the BC6H block at `block` to the red, green and blue of the texels, and an alpha of one. A block of a
// reserved mode decodes to zero.
void decodeBc6h(const uint8_t* block, bool isSigned, float* texels) {
    for (uint32_t i = 0; i < 16; ++i) {
        texels[i * 4] = texels[i * 4 + 1] = texels[i * 4 + 2] = 0;
        texels[i * 4 + 3] = 1;
    }
    BitReader bits{block};
    uint32_t first = bits.read(2);
    int32_t modeIndex = int32_t(first);
    if (first >= 2) {
        modeIndex = kBc6ModeOfBits[first | bits.read(3) << 2];
        if (modeIndex < 0)
            return;
    }
    const Bc6Mode& mode = kBc6Modes[modeIndex];
    uint32_t fields[13] = {};
    for (uint32_t r = 0; r < mode.runCount; ++r) {
        const Bc6Run& run = mode.runs[r];
        const int32_t step = run.first <= run.last ? 1 : -1;
        for (int32_t bit = run.first;; bit += step) {
            fields[run.field] |= bits.read(1) << bit;
            if (bit == run.last)
                break;
        }
    }
    // The endpoints of each subset, w and x, then y and z, in each channel.
    const uint32_t ends = mode.two ? 4 : 2;
    int32_t endpoints[4][3];
    const uint32_t epBits = mode.endpointBits;
    for (uint32_t c = 0; c < 3; ++c) {
        for (uint32_t e = 0; e < ends; ++e) {
            const uint32_t value = fields[e * 3 + c];
            const uint32_t width = e == 0 || !mode.transformed ? epBits : mode.deltaBits[c];
            endpoints[e][c] = isSigned || (e != 0 && mode.transformed) ? signExtend(value, width) : int32_t(value);
        }
        if (mode.transformed) {
            const uint32_t mask = (1u << epBits) - 1;
            for (uint32_t e = 1; e < ends; ++e) {
                const uint32_t sum = uint32_t(endpoints[0][c] + endpoints[e][c]) & mask;
                endpoints[e][c] = isSigned ? signExtend(sum, epBits) : int32_t(sum);
            }
        }
        for (uint32_t e = 0; e < ends; ++e)
            endpoints[e][c] = unquantizeBc6(endpoints[e][c], epBits, isSigned);
    }
    // The indices, with one bit fewer for the anchor of each subset.
    const PrBcPartition partition = mode.two ? prBcPartition(2, fields[Bc6Field_D]) : PrBcPartition{};
    const uint32_t indexBits = mode.two ? 3 : 4;
    const uint8_t* weights = bc7Weights(indexBits);
    for (uint32_t i = 0; i < 16; ++i) {
        const uint32_t subset = partition.subsets >> (2 * i) & 3;
        const bool anchor = i == partition.anchors[subset];
        const uint32_t weight = weights[bits.read(indexBits - (anchor ? 1 : 0))];
        const int32_t* a = endpoints[subset * 2];
        const int32_t* b = endpoints[subset * 2 + 1];
        for (uint32_t c = 0; c < 3; ++c) {
            const int32_t value = (a[c] * int32_t(64 - weight) + b[c] * int32_t(weight) + 32) >> 6;
            texels[i * 4 + c] = finishBc6(value, isSigned);
        }
    }
}

// Decodes the signed BC4 values at `block` to channel `channel` of the texels, in [-1, 1]. An end is its value over
// 127, and an end of -128 is -1, as -127 is (Khronos Data Format Specification 1.4, section 19.2).
void decodeBc4Signed(const uint8_t* block, uint32_t channel, float* texels) {
    float values[8];
    const int32_t first = int8_t(block[0]);
    const int32_t second = int8_t(block[1]);
    values[0] = first < -127 ? -1.0f : float(first) / 127.0f;
    values[1] = second < -127 ? -1.0f : float(second) / 127.0f;
    if (first > second) {
        for (uint32_t i = 1; i < 7; ++i)
            values[i + 1] = (values[0] * float(7 - i) + values[1] * float(i)) / 7.0f;
    } else {
        for (uint32_t i = 1; i < 5; ++i)
            values[i + 1] = (values[0] * float(5 - i) + values[1] * float(i)) / 5.0f;
        values[6] = -1;
        values[7] = 1;
    }
    uint64_t indices = 0;
    for (uint32_t i = 0; i < 6; ++i)
        indices |= uint64_t(block[2 + i]) << (8 * i);
    for (uint32_t i = 0; i < 16; ++i)
        texels[i * 4 + channel] = values[indices >> (3 * i) & 7];
}

// Sets every texel to opaque black.
void clearTexels(uint8_t* texels) {
    for (uint32_t i = 0; i < 16; ++i) {
        texels[i * 4] = texels[i * 4 + 1] = texels[i * 4 + 2] = 0;
        texels[i * 4 + 3] = 255;
    }
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrBcTexels prBcFormatTexels(PrBcFormat format) {
    switch (format) {
        case PrBcFormat_Bc4Snorm:
        case PrBcFormat_Bc5Snorm:
        case PrBcFormat_Bc6HUfloat:
        case PrBcFormat_Bc6HSfloat:
            return PrBcTexels_F32;
        default:
            return format < PrBcFormat_Count ? PrBcTexels_U8 : PrBcTexels_None;
    }
}

bool prBcFormatDecodes(PrBcFormat format) {
    return prBcFormatTexels(format) != PrBcTexels_None;
}

void prBcDecode(PrBcFormat format, const uint8_t* block, uint8_t* texels) {
    switch (format) {
        case PrBcFormat_Bc1:
            decodeBc1(block, true, texels);
            break;
        case PrBcFormat_Bc2:
            decodeBc1(block + 8, false, texels);
            // An alpha of four bits (section 18.3) is widened to 8 bits by repeating its bits.
            for (uint32_t i = 0; i < 16; ++i)
                texels[i * 4 + 3] = prPixelChannel8(block[i / 2] >> (i % 2 * 4) & 15, 4);
            break;
        case PrBcFormat_Bc3:
            decodeBc1(block + 8, false, texels);
            decodeBc4(block, 3, texels);
            break;
        case PrBcFormat_Bc4:
            clearTexels(texels);
            decodeBc4(block, 0, texels);
            break;
        case PrBcFormat_Bc5:
            clearTexels(texels);
            decodeBc4(block, 0, texels);
            decodeBc4(block + 8, 1, texels);
            break;
        case PrBcFormat_Bc7:
            decodeBc7(block, texels);
            break;
        default:
            clearTexels(texels);
            break;
    }
}

void prBcDecodeF32(PrBcFormat format, const uint8_t* block, float* texels) {
    switch (format) {
        case PrBcFormat_Bc4Snorm:
        case PrBcFormat_Bc5Snorm:
            for (uint32_t i = 0; i < 16; ++i) {
                texels[i * 4] = texels[i * 4 + 1] = texels[i * 4 + 2] = 0;
                texels[i * 4 + 3] = 1;
            }
            decodeBc4Signed(block, 0, texels);
            if (format == PrBcFormat_Bc5Snorm)
                decodeBc4Signed(block + 8, 1, texels);
            break;
        case PrBcFormat_Bc6HUfloat:
        case PrBcFormat_Bc6HSfloat:
            decodeBc6h(block, format == PrBcFormat_Bc6HSfloat, texels);
            break;
        default: {
            uint8_t bytes[64];
            prBcDecode(format, block, bytes);
            for (uint32_t i = 0; i < 64; ++i)
                texels[i] = float(bytes[i]) / 255.0f;
            break;
        }
    }
}

PrBcPartition prBcPartition(uint32_t subsetCount, uint32_t index) {
    PR_ASSERT((subsetCount == 2 || subsetCount == 3) && index < 64);
    PrBcPartition partition{};
    if (subsetCount == 3) {
        partition.subsets = kBc7Patterns3[index];
        partition.anchors[1] = kBc7Anchors3Second[index];
        partition.anchors[2] = kBc7Anchors3Third[index];
        return partition;
    }
    for (uint32_t texel = 0; texel < 16; ++texel)
        partition.subsets |= uint32_t(kBc7Patterns2[index] >> texel & 1) << (2 * texel);
    partition.anchors[1] = kBc7Anchors2[index];
    return partition;
}
