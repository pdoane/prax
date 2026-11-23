// ETC2 and EAC decoding.
//
// A block is 8 or 16 bytes, and it decodes without any other block. ETC2 and EAC store their integers big-endian, with
// the first texels of a block in the highest bits. They order the texels of a block by column.

#include "pr/etc2.h"

#include "pr/mem.h"
#include "pr/pixels.h"

namespace pr_impl {

namespace {

uint8_t clamp255(int32_t value) {
    return uint8_t(value < 0 ? 0 : value > 255 ? 255 : value);
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
                paints[0][c] = prPixelChannel8(uint32_t(first[c]), 4);
                paints[1][c] = clamp255(prPixelChannel8(uint32_t(second[c]), 4) + distance);
                paints[2][c] = prPixelChannel8(uint32_t(second[c]), 4);
                paints[3][c] = clamp255(prPixelChannel8(uint32_t(second[c]), 4) - distance);
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
                paints[0][c] = clamp255(prPixelChannel8(uint32_t(first[c]), 4) + distance);
                paints[1][c] = clamp255(prPixelChannel8(uint32_t(first[c]), 4) - distance);
                paints[2][c] = clamp255(prPixelChannel8(uint32_t(second[c]), 4) + distance);
                paints[3][c] = clamp255(prPixelChannel8(uint32_t(second[c]), 4) - distance);
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
                const uint32_t bits = c == 1 ? 7 : 6;
                origin[c] = prPixelChannel8(uint32_t(origin[c]), bits);
                across[c] = prPixelChannel8(uint32_t(across[c]), bits);
                down[c] = prPixelChannel8(uint32_t(down[c]), bits);
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
            bases[0][c] = prPixelChannel8(uint32_t(first), 5);
            bases[1][c] = prPixelChannel8(uint32_t(second), 5);
        } else {
            bases[0][c] = prPixelChannel8(block[c] >> 4, 4);
            bases[1][c] = prPixelChannel8(block[c] & 15u, 4);
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

// Decodes the signed EAC values at `block` to channel `channel` of the texels, in [-1, 1]. The base is a signed byte,
// of which -128 reads as -127, and the value of 11 bits is clamped to [-1023, 1023] (Khronos Data Format Specification
// 1.4, section 22.7).
void decodeEacSigned(const uint8_t* block, uint32_t channel, float* texels) {
    int32_t base = int8_t(block[0]);
    base = base < -127 ? -127 : base;
    const int32_t multiplier = block[1] >> 4;
    const int8_t* modifiers = kEacModifiers[block[1] & 15];
    uint64_t indices = 0;
    for (uint32_t i = 0; i < 6; ++i)
        indices = indices << 8 | block[2 + i];
    for (uint32_t x = 0; x < 4; ++x) {
        for (uint32_t y = 0; y < 4; ++y) {
            const int32_t modifier = modifiers[indices >> (45 - 3 * (x * 4 + y)) & 7];
            int32_t value = base * 8 + modifier * (multiplier != 0 ? multiplier * 8 : 1);
            value = value < -1023 ? -1023 : value > 1023 ? 1023 : value;
            texels[(y * 4 + x) * 4 + channel] = float(value) / 1023.0f;
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

PrEtc2Texels prEtc2FormatTexels(PrEtc2Format format) {
    if (format == PrEtc2Format_R11Snorm || format == PrEtc2Format_Rg11Snorm)
        return PrEtc2Texels_F32;
    return format < PrEtc2Format_Count ? PrEtc2Texels_U8 : PrEtc2Texels_None;
}

bool prEtc2FormatDecodes(PrEtc2Format format) {
    return prEtc2FormatTexels(format) != PrEtc2Texels_None;
}

void prEtc2Decode(PrEtc2Format format, const uint8_t* block, uint8_t* texels) {
    switch (format) {
        case PrEtc2Format_Rgb8:
            decodeEtc2(block, false, texels);
            break;
        case PrEtc2Format_Rgb8A1:
            decodeEtc2(block, true, texels);
            break;
        case PrEtc2Format_Rgba8:
            decodeEtc2(block + 8, false, texels);
            decodeEac(block, false, 3, texels);
            break;
        case PrEtc2Format_R11:
            clearTexels(texels);
            decodeEac(block, true, 0, texels);
            break;
        case PrEtc2Format_Rg11:
            clearTexels(texels);
            decodeEac(block, true, 0, texels);
            decodeEac(block + 8, true, 1, texels);
            break;
        default:
            clearTexels(texels);
            break;
    }
}

void prEtc2DecodeF32(PrEtc2Format format, const uint8_t* block, float* texels) {
    if (format == PrEtc2Format_R11Snorm || format == PrEtc2Format_Rg11Snorm) {
        for (uint32_t i = 0; i < 16; ++i) {
            texels[i * 4] = texels[i * 4 + 1] = texels[i * 4 + 2] = 0;
            texels[i * 4 + 3] = 1;
        }
        decodeEacSigned(block, 0, texels);
        if (format == PrEtc2Format_Rg11Snorm)
            decodeEacSigned(block + 8, 1, texels);
        return;
    }
    uint8_t bytes[64];
    prEtc2Decode(format, block, bytes);
    for (uint32_t i = 0; i < 64; ++i)
        texels[i] = float(bytes[i]) / 255.0f;
}
