// Tests lib/bc's decoders over blocks of random bits against the reference unpackers of Basis Universal's encoder: the
// formats of 8-bit texels, and BC6H's halves in both forms. For BC1 to BC5 it also checks each 8-bit texel against the
// real value that the Khronos Data Format Specification gives, computed here in doubles: the specification leaves the
// conversion to 8 bits open, and lib/bc's is to be within one of the nearest 8-bit value.

#include "pr/bc.h"
#include "pr/float16.h"
#include "pr/mem.h"

#include "basisu_gpu_texture.h"
#include "tools/harness/test.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>
#include <vector>

namespace {

// Returns the 8-bit UNORM value nearest to `value` in [0, 1], and the greater of two that are as near. The margin keeps
// a value that is exactly halfway from falling below it in doubles.
uint8_t nearest(double value) {
    return uint8_t(std::floor(value * 255.0 + 0.5 + 1e-9));
}

// Decodes the BC1 colors at `block` as the Khronos Data Format Specification 1.4 gives them (sections 18.1 to 18.4).
// The colors of BC2 and BC3 always have four entries.
void khronosBc1(const uint8_t* block, bool punchthrough, uint8_t* texels) {
    const uint32_t c0 = uint32_t(block[0] | block[1] << 8);
    const uint32_t c1 = uint32_t(block[2] | block[3] << 8);
    double ends[2][3];
    for (uint32_t i = 0; i < 2; ++i) {
        const uint32_t c = i == 0 ? c0 : c1;
        ends[i][0] = double(c >> 11) / 31.0;
        ends[i][1] = double(c >> 5 & 63) / 63.0;
        ends[i][2] = double(c & 31) / 31.0;
    }
    const bool four = c0 > c1 || !punchthrough;
    double colors[4][4];
    for (uint32_t ch = 0; ch < 3; ++ch) {
        colors[0][ch] = ends[0][ch];
        colors[1][ch] = ends[1][ch];
        colors[2][ch] = four ? (2 * ends[0][ch] + ends[1][ch]) / 3 : (ends[0][ch] + ends[1][ch]) / 2;
        colors[3][ch] = four ? (ends[0][ch] + 2 * ends[1][ch]) / 3 : 0;
    }
    colors[0][3] = colors[1][3] = colors[2][3] = 1;
    colors[3][3] = four ? 1 : 0;
    const uint32_t indices = uint32_t(block[4] | block[5] << 8 | block[6] << 16) | uint32_t(block[7]) << 24;
    for (uint32_t t = 0; t < 16; ++t) {
        for (uint32_t ch = 0; ch < 4; ++ch)
            texels[t * 4 + ch] = nearest(colors[indices >> (2 * t) & 3][ch]);
    }
}

// Decodes the BC4 values at `block` to channel `channel` as section 19.1 gives them, as BC3's alpha is too.
void khronosBc4(const uint8_t* block, uint32_t channel, uint8_t* texels) {
    const double a = block[0] / 255.0;
    const double b = block[1] / 255.0;
    double values[8] = {a, b};
    if (block[0] > block[1]) {
        for (uint32_t i = 1; i < 7; ++i)
            values[i + 1] = (a * (7 - i) + b * i) / 7;
    } else {
        for (uint32_t i = 1; i < 5; ++i)
            values[i + 1] = (a * (5 - i) + b * i) / 5;
        values[6] = 0;
        values[7] = 1;
    }
    uint64_t indices = 0;
    for (uint32_t i = 0; i < 6; ++i)
        indices |= uint64_t(block[2 + i]) << (8 * i);
    for (uint32_t t = 0; t < 16; ++t)
        texels[t * 4 + channel] = nearest(values[indices >> (3 * t) & 7]);
}

// Decodes a block of BC1, BC3, BC4 or BC5 as the specification gives it.
void khronosDecode(PrBcFormat format, const uint8_t* block, uint8_t* texels) {
    for (uint32_t t = 0; t < 16; ++t) {
        texels[t * 4] = texels[t * 4 + 1] = texels[t * 4 + 2] = 0;
        texels[t * 4 + 3] = 255;
    }
    if (format == PrBcFormat_Bc1) {
        khronosBc1(block, true, texels);
    } else if (format == PrBcFormat_Bc3) {
        khronosBc1(block + 8, false, texels);
        khronosBc4(block, 3, texels);
    } else {
        khronosBc4(block, 0, texels);
        if (format == PrBcFormat_Bc5)
            khronosBc4(block + 8, 1, texels);
    }
}

} // namespace

TEST_CASE("the BC decoders decode blocks of random bits to the same texels as the reference unpackers") {
    // Each format, the size of its blocks, the reference's name for it, and the number of channels that it has. The two
    // decoders are compared in those channels.
    struct Format {
        uint32_t vkFormat;
        PrBcFormat format;
        uint32_t blockBytes;
        basisu::texture_format reference;
        uint32_t channels;
        const char* name;
    };
    const Format kFormats[] = {
        {133, PrBcFormat_Bc1, 8, basisu::texture_format::cBC1, 4, "BC1"},
        {137, PrBcFormat_Bc3, 16, basisu::texture_format::cBC3, 4, "BC3"},
        {139, PrBcFormat_Bc4, 8, basisu::texture_format::cBC4, 1, "BC4"},
        {141, PrBcFormat_Bc5, 16, basisu::texture_format::cBC5, 2, "BC5"},
        {145, PrBcFormat_Bc7, 16, basisu::texture_format::cBC7, 4, "BC7"},
    };
    const uint32_t kBlocks = 200000;
    uint64_t state = 0x9E3779B97F4A7C15ull;
    for (const Format& format : kFormats) {
        std::vector<uint8_t> blocks(size_t(kBlocks) * format.blockBytes);
        for (uint8_t& byte : blocks) {
            state = state * 6364136223846793005ull + 1442695040888963407ull;
            byte = uint8_t(state >> 56);
        }
        // The reference reads the colors of BC3 as it reads the colors of BC1. A BC1 block has four colors only if its
        // first endpoint is the greater. A BC3 block always has four.
        for (uint32_t i = 0; format.format == PrBcFormat_Bc3 && i < kBlocks; ++i) {
            uint8_t* block = &blocks[size_t(i) * 16];
            if ((block[8] | block[9] << 8) <= (block[10] | block[11] << 8)) {
                std::swap(block[8], block[10]);
                std::swap(block[9], block[11]);
            }
        }
        REQUIRE(prBcFormatDecodes(format.format));
        const bool khronos = format.format != PrBcFormat_Bc7;
        uint32_t compared = 0;
        uint32_t different = 0;
        uint32_t farthest = 0;
        for (uint32_t i = 0; i < kBlocks; ++i) {
            const uint8_t* block = &blocks[size_t(i) * format.blockBytes];
            basisu::color_rgba reference[16];
            for (basisu::color_rgba& texel : reference)
                texel = basisu::color_rgba(0, 0, 0, 255);
            // Basis refuses some blocks: it has no decoder of a mode that it lacks, nor of a BC7 block of no mode.
            if (!basisu::unpack_block(format.reference, block, reference))
                continue;
            ++compared;
            uint8_t texels[64];
            prBcDecode(format.format, block, texels);
            uint8_t exact[64];
            if (khronos)
                khronosDecode(format.format, block, exact);
            bool same = true;
            for (uint32_t t = 0; t < 16; ++t) {
                const uint8_t* pixel = &texels[t * 4];
                for (uint32_t c = 0; c < format.channels; ++c) {
                    same = same && pixel[c] == reference[t][c];
                    if (khronos) {
                        const int32_t distance = int32_t(pixel[c]) - int32_t(exact[t * 4 + c]);
                        farthest = std::max(farthest, uint32_t(distance < 0 ? -distance : distance));
                    }
                }
            }
            different += same ? 0 : 1;
        }
        printf("%-10s  %6u blocks compared, %u differ, farthest from the specification's nearest %u\n", format.name,
            compared, different, farthest);
        CHECK(compared > 1000);
        CHECK(different == 0);
        CHECK(farthest <= 1);
    }
}

TEST_CASE("the BC6H decoder decodes blocks of random bits to the same halves as the reference unpacker") {
    const uint32_t kBlocks = 200000;
    uint64_t state = 0x2545F4914F6CDD1Dull;
    const PrBcFormat kForms[2] = {PrBcFormat_Bc6HUfloat, PrBcFormat_Bc6HSfloat};
    for (PrBcFormat form : kForms) {
        const bool isSigned = form == PrBcFormat_Bc6HSfloat;
        uint32_t compared = 0;
        uint32_t different = 0;
        for (uint32_t i = 0; i < kBlocks; ++i) {
            uint8_t block[16];
            for (uint8_t& byte : block) {
                state = state * 6364136223846793005ull + 1442695040888963407ull;
                byte = uint8_t(state >> 56);
            }
            uint16_t reference[16 * 3];
            // The reference refuses a block of a reserved mode.
            if (!basisu::unpack_bc6h(block, reference, isSigned))
                continue;
            ++compared;
            float texels[64];
            prBcDecodeF32(form, block, texels);
            // The values must be equal. A signed value that rounds to zero is negative zero here, as Direct3D's
            // finishing of a negative value gives it, and positive zero in the reference.
            bool same = true;
            for (uint32_t t = 0; t < 16; ++t) {
                for (uint32_t c = 0; c < 3; ++c) {
                    PrFloat16 half{};
                    prMemCopy(&half, &reference[t * 3 + c], sizeof(half));
                    same = same && texels[t * 4 + c] == prFloat16ToFloat(half);
                }
            }
            different += same ? 0 : 1;
        }
        printf("%-10s  %6u blocks compared, %u differ\n", isSigned ? "BC6H SF16" : "BC6H UF16", compared, different);
        CHECK(compared > 1000);
        CHECK(different == 0);
    }
}
