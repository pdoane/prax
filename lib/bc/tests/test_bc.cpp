// Tests of lib/bc: the formats that decode, the BC7 partitions, and blocks that decode to known texels. tools/suite/bc
// compares the decoders with the reference unpackers of Basis Universal over blocks of random bits.

#include "pr/bc.h"

#include "tools/harness/test.h"

extern "C" int prTestBcCApi(void);

TEST_CASE("the unsigned formats decode to 8-bit texels, and the signed and float formats to floats") {
    CHECK(prBcFormatTexels(PrBcFormat_Bc1) == PrBcTexels_U8);
    CHECK(prBcFormatTexels(PrBcFormat_Bc3) == PrBcTexels_U8);
    CHECK(prBcFormatTexels(PrBcFormat_Bc7) == PrBcTexels_U8);
    CHECK(prBcFormatTexels(PrBcFormat_Bc4Snorm) == PrBcTexels_F32);
    CHECK(prBcFormatTexels(PrBcFormat_Bc5Snorm) == PrBcTexels_F32);
    CHECK(prBcFormatTexels(PrBcFormat_Bc6HUfloat) == PrBcTexels_F32);
    CHECK(prBcFormatTexels(PrBcFormat_Bc6HSfloat) == PrBcTexels_F32);
    CHECK(prBcFormatDecodes(PrBcFormat_Bc6HSfloat));
}

TEST_CASE("signed BC4 and BC5 blocks decode to values in [-1, 1]") {
    // Ends of 127 and -127, so that the block has eight values. The second texel has index 1, the second end, and the
    // others index 0, the first.
    const uint8_t block[8] = {0x7f, 0x81, 0x08, 0, 0, 0, 0, 0};
    float texels[64];
    prBcDecodeF32(PrBcFormat_Bc4Snorm, block, texels);
    CHECK(texels[0] == 1.0f);
    CHECK(texels[4] == -1.0f);
    CHECK(texels[8] == 1.0f);
    CHECK(texels[1] == 0.0f);
    CHECK(texels[3] == 1.0f);
    // Ends of -128 and 0, so that the block has six values and then -1 and 1. The first texel has index 7.
    const uint8_t two[16] = {0x80, 0x00, 0x07, 0, 0, 0, 0, 0, 0x00, 0x7f, 0, 0, 0, 0, 0, 0};
    prBcDecodeF32(PrBcFormat_Bc5Snorm, two, texels);
    CHECK(texels[0] == 1.0f);
    CHECK(texels[4] == -1.0f);
    CHECK(texels[1] == 0.0f);
    CHECK(texels[2] == 0.0f);
}

TEST_CASE("a BC6H block decodes to its halves, unsigned and signed, and a block of a reserved mode to zero") {
    // Mode 11, of one subset and 10-bit endpoints. Red of the first endpoint is all ones, which unquantizes to 65535
    // and finishes as the half 0x7bff, 65504. Every other channel is zero, and every index is the first endpoint.
    uint8_t block[16] = {};
    block[0] = 0x03 | 0xe0;
    block[1] = 0x7f;
    float texels[64];
    prBcDecodeF32(PrBcFormat_Bc6HUfloat, block, texels);
    for (uint32_t i = 0; i < 16; ++i) {
        CHECK(texels[i * 4] == 65504.0f);
        CHECK(texels[i * 4 + 1] == 0.0f);
        CHECK(texels[i * 4 + 3] == 1.0f);
    }
    // Signed, the 10 bits of ones are -1, a small negative value.
    prBcDecodeF32(PrBcFormat_Bc6HSfloat, block, texels);
    CHECK(texels[0] < 0.0f);
    // Mode bits 10011 are reserved.
    uint8_t reserved[16] = {0x13};
    prBcDecodeF32(PrBcFormat_Bc6HUfloat, reserved, texels);
    CHECK(texels[0] == 0.0f);
    CHECK(texels[3] == 1.0f);
}

TEST_CASE("a format of 8-bit texels decodes to floats of its values over 255") {
    const uint8_t value[8] = {51, 100, 0, 0, 0, 0, 0, 0};
    float texels[64];
    prBcDecodeF32(PrBcFormat_Bc4, value, texels);
    CHECK(texels[0] == 51.0f / 255.0f);
    CHECK(texels[3] == 1.0f);
}

TEST_CASE("a BC1 block decodes to its colors, and a BC4 block decodes to its value in the red channel") {
    // The ends are white and black. Every texel has the first end, so the block is white.
    const uint8_t white[8] = {0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    uint8_t texels[64];
    prBcDecode(PrBcFormat_Bc1, white, texels);
    for (uint32_t i = 0; i < 64; ++i)
        CHECK(texels[i] == 255);
    // Every texel has the second end, so the block is black.
    const uint8_t black[8] = {0xFF, 0xFF, 0x00, 0x00, 0x55, 0x55, 0x55, 0x55};
    prBcDecode(PrBcFormat_Bc1, black, texels);
    CHECK(texels[0] == 0);
    CHECK(texels[3] == 255);
    // A BC4 block whose ends are 200 and 100. Every texel has the first end.
    const uint8_t value[8] = {200, 100, 0, 0, 0, 0, 0, 0};
    prBcDecode(PrBcFormat_Bc4, value, texels);
    for (uint32_t i = 0; i < 16; ++i) {
        CHECK(texels[i * 4] == 200);
        CHECK(texels[i * 4 + 1] == 0);
        CHECK(texels[i * 4 + 3] == 255);
    }
}

TEST_CASE("a BC7 partition gives each texel's subset and each subset's anchor") {
    // The first partition of two subsets puts the two right columns in the second subset, with its anchor at the last
    // texel.
    PrBcPartition two = prBcPartition(2, 0);
    CHECK(two.subsets == 0x50505050u);
    CHECK(two.anchors[0] == 0);
    CHECK(two.anchors[1] == 15);
    CHECK(two.anchors[2] == 0);
    PrBcPartition three = prBcPartition(3, 0);
    CHECK((three.subsets & 3) == 0);
    CHECK(three.anchors[1] == 3);
    CHECK(three.anchors[2] == 15);
}

TEST_CASE("the bc header compiles as C") {
    CHECK(prTestBcCApi() == 0);
}
