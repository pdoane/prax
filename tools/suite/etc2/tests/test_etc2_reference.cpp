// Tests lib/etc2's decoders against the reference unpackers of Basis Universal's encoder, over blocks of random bits.

#include "pr/etc2.h"

#include "basisu_gpu_texture.h"
#include "tools/harness/test.h"

#include <cstdio>
#include <utility>
#include <vector>

TEST_CASE("the ETC2 decoders decode blocks of random bits to the same texels as the reference unpackers") {
    // Each format, the size of its blocks, the reference's name for it, and the number of channels that it has. The two
    // decoders are compared in those channels.
    struct Format {
        uint32_t vkFormat;
        PrEtc2Format format;
        uint32_t blockBytes;
        basisu::texture_format reference;
        uint32_t channels;
        const char* name;
    };
    const Format kFormats[] = {
        {147, PrEtc2Format_Rgb8, 8, basisu::texture_format::cETC2_RGB, 3, "ETC2 RGB8"},
        {151, PrEtc2Format_Rgba8, 16, basisu::texture_format::cETC2_RGBA, 4, "ETC2 RGBA8"},
        {153, PrEtc2Format_R11, 8, basisu::texture_format::cETC2_R11_EAC, 1, "EAC R11"},
        {155, PrEtc2Format_Rg11, 16, basisu::texture_format::cETC2_RG11_EAC, 2, "EAC RG11"},
    };
    const uint32_t kBlocks = 200000;
    uint64_t state = 0x9E3779B97F4A7C15ull;
    for (const Format& format : kFormats) {
        std::vector<uint8_t> blocks(size_t(kBlocks) * format.blockBytes);
        for (uint8_t& byte : blocks) {
            state = state * 6364136223846793005ull + 1442695040888963407ull;
            byte = uint8_t(state >> 56);
        }
        REQUIRE(prEtc2FormatDecodes(format.format));
        uint32_t compared = 0;
        uint32_t different = 0;
        for (uint32_t i = 0; i < kBlocks; ++i) {
            basisu::color_rgba reference[16];
            for (basisu::color_rgba& texel : reference)
                texel = basisu::color_rgba(0, 0, 0, 255);
            // The reference refuses some blocks: it has no decoder of a mode that it lacks, nor of the modes that ETC2
            // adds to ETC1.
            if (!basisu::unpack_block(format.reference, &blocks[size_t(i) * format.blockBytes], reference))
                continue;
            ++compared;
            uint8_t texels[64];
            prEtc2Decode(format.format, &blocks[size_t(i) * format.blockBytes], texels);
            bool same = true;
            for (uint32_t t = 0; t < 16; ++t) {
                const uint8_t* pixel = &texels[t * 4];
                for (uint32_t c = 0; c < format.channels; ++c)
                    same = same && pixel[c] == reference[t][c];
            }
            different += same ? 0 : 1;
        }
        printf("%-10s  %6u blocks compared, %u differ\n", format.name, compared, different);
        CHECK(compared > 1000);
        CHECK(different == 0);
    }
}
