// The block-compressed formats that a GPU samples, and their decoding in software.
//
// A block of any of these formats is 8 or 16 bytes, and it decodes without any other block. BC stores its integers
// little-endian and reads the bits of a block from the lowest bit of the first byte. ETC2 and EAC store their integers
// big-endian, with the first texels of a block in the highest bits. They order the texels of a block by column.

#include "pr/basis.h"

#include "pr/mem.h"

#include "basis/src/bc7_patterns.h"

namespace pr_impl {

namespace {

uint8_t clamp255(int32_t value) {
    return uint8_t(value < 0 ? 0 : value > 255 ? 255 : value);
}

// Decodes the BC1 colors at `block` to the texels. If `punchthrough` is set, a block whose first color is not the
// greater has three colors and a fourth that is transparent black. Otherwise the block has four colors, as the color
// blocks of BC2 and BC3 always do.
void decodeBc1(const uint8_t* block, bool punchthrough, uint8_t* texels) {
    uint32_t c0 = prMemReadLeU16(block);
    uint32_t c1 = prMemReadLeU16(block + 2);
    uint32_t palette[4][4];
    for (uint32_t i = 0; i < 2; ++i) {
        uint32_t c = i == 0 ? c0 : c1;
        uint32_t r = c >> 11;
        uint32_t g = c >> 5 & 63;
        uint32_t b = c & 31;
        palette[i][0] = r << 3 | r >> 2;
        palette[i][1] = g << 2 | g >> 4;
        palette[i][2] = b << 3 | b >> 2;
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

// Decodes the BC4 values at `block` to channel `channel` of the texels.
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
            endpoints[e][c] = (value << (8 - count) | value >> (2 * count - 8)) & 255;
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

// The ETC1 modifier tables, the distances of the ETC2 T and H modes, and the EAC modifier tables.
constexpr int32_t kEtcModifiers[8][2] = {{2, 8}, {5, 17}, {9, 29}, {13, 42}, {18, 60}, {24, 80}, {33, 106}, {47, 183}};
constexpr int32_t kEtcDistances[8] = {3, 6, 11, 16, 23, 32, 41, 64};
constexpr int8_t kEacModifiers[16][8] = {
    {-3, -6, -9, -15, 2, 5, 8, 14}, {-3, -7, -10, -13, 2, 6, 9, 12}, {-2, -5, -8, -13, 1, 4, 7, 12},
    {-2, -4, -6, -13, 1, 3, 5, 12}, {-3, -6, -8, -12, 2, 5, 7, 11}, {-3, -7, -9, -11, 2, 6, 8, 10},
    {-4, -7, -8, -11, 3, 6, 7, 10}, {-3, -5, -8, -11, 2, 4, 7, 10}, {-2, -6, -8, -10, 1, 5, 7, 9},
    {-2, -5, -8, -10, 1, 4, 7, 9}, {-2, -4, -8, -10, 1, 3, 7, 9}, {-2, -5, -7, -10, 1, 4, 6, 9},
    {-3, -4, -7, -10, 2, 3, 6, 9}, {-1, -2, -3, -10, 0, 1, 2, 9}, {-4, -6, -8, -9, 3, 5, 7, 8},
    {-3, -5, -7, -9, 2, 4, 6, 8},
};

// Returns the value of the low three bits of `bits` as a signed integer.
int32_t signed3(uint32_t bits) {
    return int32_t(bits & 3) - int32_t(bits & 4);
}

// Decodes the ETC2 colors at `block` to the red, green, blue and alpha of the texels. `punchthrough` selects the format
// with one bit of alpha. Its blocks have no mode with two colors of four bits per channel.
void decodeEtc2(const uint8_t* block, bool punchthrough, uint8_t* texels) {
    uint32_t word = prMemReadBeU32(block + 4);
    bool flag = (block[3] & 2) != 0;
    bool opaque = !punchthrough || flag;
    int32_t paints[4][3];
    bool painted = false;
    if (punchthrough || flag) {
        int32_t r = int32_t(block[0] >> 3) + signed3(block[0]);
        int32_t g = int32_t(block[1] >> 3) + signed3(block[1]);
        int32_t b = int32_t(block[2] >> 3) + signed3(block[2]);
        if (r < 0 || r > 31) {
            // T mode. The palette is one color alone, a second color, and the two colors at a distance to either side
            // of the second.
            int32_t first[3] = {int32_t((block[0] >> 3 & 3) << 2 | (block[0] & 3)), block[1] >> 4, block[1] & 15};
            int32_t second[3] = {block[2] >> 4, block[2] & 15, block[3] >> 4};
            int32_t distance = kEtcDistances[(block[3] >> 2 & 3) << 1 | (block[3] & 1)];
            for (uint32_t c = 0; c < 3; ++c) {
                paints[0][c] = first[c] * 17;
                paints[1][c] = clamp255(second[c] * 17 + distance);
                paints[2][c] = second[c] * 17;
                paints[3][c] = clamp255(second[c] * 17 - distance);
            }
            painted = true;
        } else if (g < 0 || g > 31) {
            // H mode. The palette is the colors at a distance to either side of each of two colors.
            int32_t first[3] = {
                block[0] >> 3 & 15, int32_t((block[0] & 7) << 1 | (block[1] >> 4 & 1)),
                int32_t((block[1] & 8) | (block[1] & 3) << 1 | block[2] >> 7),
            };
            int32_t second[3] = {block[2] >> 3 & 15, int32_t((block[2] & 7) << 1 | block[3] >> 7), block[3] >> 3 & 15};
            int32_t firstValue = first[0] << 16 | first[1] << 8 | first[2];
            int32_t secondValue = second[0] << 16 | second[1] << 8 | second[2];
            int32_t distance = kEtcDistances[(block[3] & 4) | (block[3] & 1) << 1 | (firstValue >= secondValue ? 1 : 0)];
            for (uint32_t c = 0; c < 3; ++c) {
                paints[0][c] = clamp255(first[c] * 17 + distance);
                paints[1][c] = clamp255(first[c] * 17 - distance);
                paints[2][c] = clamp255(second[c] * 17 + distance);
                paints[3][c] = clamp255(second[c] * 17 - distance);
            }
            painted = true;
        } else if (b < 0 || b > 31) {
            // Planar mode. The block stores the color at its origin and the colors four texels away along each axis.
            int32_t origin[3] = {
                block[0] >> 1 & 63, int32_t((block[0] & 1) << 6 | (block[1] >> 1 & 63)),
                int32_t((block[1] & 1) << 5 | (block[2] & 0x18) | (block[2] & 3) << 1 | block[3] >> 7),
            };
            int32_t across[3] = {
                int32_t((block[3] >> 2 & 31) << 1 | (block[3] & 1)), block[4] >> 1,
                int32_t((block[4] & 1) << 5 | block[5] >> 3),
            };
            int32_t down[3] = {
                int32_t((block[5] & 7) << 3 | block[6] >> 5), int32_t((block[6] & 31) << 2 | block[7] >> 6),
                block[7] & 63,
            };
            for (uint32_t c = 0; c < 3; ++c) {
                // Green has seven bits, and red and blue six.
                uint32_t low = c == 1 ? 1 : 2;
                uint32_t high = c == 1 ? 6 : 4;
                origin[c] = origin[c] << low | origin[c] >> high;
                across[c] = across[c] << low | across[c] >> high;
                down[c] = down[c] << low | down[c] >> high;
            }
            for (uint32_t y = 0; y < 4; ++y) {
                for (uint32_t x = 0; x < 4; ++x) {
                    uint8_t* texel = texels + (y * 4 + x) * 4;
                    for (uint32_t c = 0; c < 3; ++c) {
                        int32_t value = int32_t(x) * (across[c] - origin[c]) + int32_t(y) * (down[c] - origin[c]) +
                            4 * origin[c] + 2;
                        texel[c] = clamp255(value >> 2);
                    }
                    texel[3] = 255;
                }
            }
            return;
        }
    }
    if (painted) {
        for (uint32_t x = 0; x < 4; ++x) {
            for (uint32_t y = 0; y < 4; ++y) {
                uint32_t pixel = x * 4 + y;
                uint32_t index = (word >> (16 + pixel) & 1) << 1 | (word >> pixel & 1);
                uint8_t* texel = texels + (y * 4 + x) * 4;
                bool transparent = !opaque && index == 2;
                for (uint32_t c = 0; c < 3; ++c)
                    texel[c] = transparent ? 0 : uint8_t(paints[index][c]);
                texel[3] = transparent ? 0 : 255;
            }
        }
        return;
    }
    // The ETC1 modes. Each half of the block has a base color and a modifier table. The second base color is stored
    // whole or as a difference from the first.
    int32_t bases[2][3];
    for (uint32_t c = 0; c < 3; ++c) {
        if (punchthrough || flag) {
            int32_t first = block[c] >> 3;
            int32_t second = first + signed3(block[c]);
            bases[0][c] = first << 3 | first >> 2;
            bases[1][c] = second << 3 | second >> 2;
        } else {
            bases[0][c] = (block[c] >> 4) * 17;
            bases[1][c] = (block[c] & 15) * 17;
        }
    }
    uint32_t tables[2] = {uint32_t(block[3] >> 5), uint32_t(block[3] >> 2 & 7)};
    bool flip = (block[3] & 1) != 0;
    for (uint32_t x = 0; x < 4; ++x) {
        for (uint32_t y = 0; y < 4; ++y) {
            uint32_t pixel = x * 4 + y;
            uint32_t index = (word >> (16 + pixel) & 1) << 1 | (word >> pixel & 1);
            uint32_t half = (flip ? y : x) < 2 ? 0 : 1;
            int32_t modifier = kEtcModifiers[tables[half]][index & 1];
            if (!opaque && (index & 1) == 0)
                modifier = 0;
            if ((index & 2) != 0)
                modifier = -modifier;
            uint8_t* texel = texels + (y * 4 + x) * 4;
            bool transparent = !opaque && index == 2;
            for (uint32_t c = 0; c < 3; ++c)
                texel[c] = transparent ? 0 : clamp255(bases[half][c] + modifier);
            texel[3] = transparent ? 0 : 255;
        }
    }
}

// Decodes the EAC values at `block` to channel `channel` of the texels. The values have 8 bits, or 11 bits rounded to 8
// if `eleven` is set.
void decodeEac(const uint8_t* block, bool eleven, uint32_t channel, uint8_t* texels) {
    int32_t base = block[0];
    int32_t multiplier = block[1] >> 4;
    const int8_t* modifiers = kEacModifiers[block[1] & 15];
    uint64_t indices = 0;
    for (uint32_t i = 0; i < 6; ++i)
        indices = indices << 8 | block[2 + i];
    for (uint32_t x = 0; x < 4; ++x) {
        for (uint32_t y = 0; y < 4; ++y) {
            int32_t modifier = modifiers[indices >> (45 - 3 * (x * 4 + y)) & 7];
            uint8_t* texel = texels + (y * 4 + x) * 4;
            if (!eleven) {
                texel[channel] = clamp255(base + modifier * multiplier);
                continue;
            }
            int32_t value = base * 8 + 4 + modifier * (multiplier != 0 ? multiplier * 8 : 1);
            value = value < 0 ? 0 : value > 2047 ? 2047 : value;
            texel[channel] = uint8_t((value * 255 + 1023) / 2047);
        }
    }
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

PrUVec2 prBasisBlocksExtent(PrBasisBlocks blocks) {
    // The ASTC block extents, in the order of PrBasisBlocks.
    constexpr PrUVec2 kAstc[] = {
        {4, 4}, {5, 4}, {5, 5}, {6, 5}, {6, 6}, {8, 5}, {8, 6}, {8, 8}, {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10},
        {12, 12},
    };
    if (blocks == PrBasisBlocks_None || blocks >= PrBasisBlocks_Count)
        return PrUVec2{0, 0};
    return blocks < PrBasisBlocks_Astc4x4 ? PrUVec2{4, 4} : kAstc[(blocks - PrBasisBlocks_Astc4x4) / 2];
}

uint32_t prBasisBlocksBytes(PrBasisBlocks blocks) {
    switch (blocks) {
        case PrBasisBlocks_None:
            return 0;
        case PrBasisBlocks_Bc1:
        case PrBasisBlocks_Bc4:
        case PrBasisBlocks_Bc4Snorm:
        case PrBasisBlocks_Etc2Rgb8:
        case PrBasisBlocks_Etc2Rgb8A1:
        case PrBasisBlocks_EacR11:
        case PrBasisBlocks_EacR11Snorm:
            return 8;
        default:
            return blocks < PrBasisBlocks_Count ? 16 : 0;
    }
}

size_t prBasisBlocksSize(PrUVec2 extent, PrBasisBlocks blocks) {
    PrUVec2 block = prBasisBlocksExtent(blocks);
    if (block.x == 0)
        return 0;
    return size_t((extent.x + block.x - 1) / block.x) * ((extent.y + block.y - 1) / block.y) *
        prBasisBlocksBytes(blocks);
}

bool prBasisBlocksDecodes(PrBasisBlocks blocks) {
    switch (blocks) {
        case PrBasisBlocks_Bc1:
        case PrBasisBlocks_Bc2:
        case PrBasisBlocks_Bc3:
        case PrBasisBlocks_Bc4:
        case PrBasisBlocks_Bc5:
        case PrBasisBlocks_Bc7:
        case PrBasisBlocks_Etc2Rgb8:
        case PrBasisBlocks_Etc2Rgb8A1:
        case PrBasisBlocks_Etc2Rgba8:
        case PrBasisBlocks_EacR11:
        case PrBasisBlocks_EacRg11:
            return true;
        default:
            return false;
    }
}

void prBasisBlockDecode(PrBasisBlocks blocks, const uint8_t* block, uint8_t* texels) {
    switch (blocks) {
        case PrBasisBlocks_Bc1:
            decodeBc1(block, true, texels);
            break;
        case PrBasisBlocks_Bc2:
            decodeBc1(block + 8, false, texels);
            for (uint32_t i = 0; i < 16; ++i)
                texels[i * 4 + 3] = uint8_t((block[i / 2] >> (i % 2 * 4) & 15) * 17);
            break;
        case PrBasisBlocks_Bc3:
            decodeBc1(block + 8, false, texels);
            decodeBc4(block, 3, texels);
            break;
        case PrBasisBlocks_Bc4:
            clearTexels(texels);
            decodeBc4(block, 0, texels);
            break;
        case PrBasisBlocks_Bc5:
            clearTexels(texels);
            decodeBc4(block, 0, texels);
            decodeBc4(block + 8, 1, texels);
            break;
        case PrBasisBlocks_Bc7:
            decodeBc7(block, texels);
            break;
        case PrBasisBlocks_Etc2Rgb8:
            decodeEtc2(block, false, texels);
            break;
        case PrBasisBlocks_Etc2Rgb8A1:
            decodeEtc2(block, true, texels);
            break;
        case PrBasisBlocks_Etc2Rgba8:
            decodeEtc2(block + 8, false, texels);
            decodeEac(block, false, 3, texels);
            break;
        case PrBasisBlocks_EacR11:
            clearTexels(texels);
            decodeEac(block, true, 0, texels);
            break;
        case PrBasisBlocks_EacRg11:
            clearTexels(texels);
            decodeEac(block, true, 0, texels);
            decodeEac(block + 8, true, 1, texels);
            break;
        default:
            clearTexels(texels);
            break;
    }
}
