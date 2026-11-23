// Tests of lib/etc2: the formats that decode, and the modes that ETC2 adds to ETC1. tools/suite/etc2 compares the
// decoders with the reference unpackers of Basis Universal over blocks of random bits.

#include "pr/etc2.h"

#include "tools/harness/test.h"

extern "C" int prTestEtc2CApi(void);

TEST_CASE("ETC2 and EAC decode to 8-bit texels, and the signed formats of EAC to floats") {
    CHECK(prEtc2FormatTexels(PrEtc2Format_Rgb8) == PrEtc2Texels_U8);
    CHECK(prEtc2FormatTexels(PrEtc2Format_Rgba8) == PrEtc2Texels_U8);
    CHECK(prEtc2FormatTexels(PrEtc2Format_Rg11) == PrEtc2Texels_U8);
    CHECK(prEtc2FormatTexels(PrEtc2Format_R11Snorm) == PrEtc2Texels_F32);
    CHECK(prEtc2FormatTexels(PrEtc2Format_Rg11Snorm) == PrEtc2Texels_F32);
    CHECK(prEtc2FormatDecodes(PrEtc2Format_Rg11Snorm));
}

TEST_CASE("signed EAC blocks decode to their 11-bit values over 1023, clamped") {
    // A base of 100 with a multiplier of zero: every texel is 800 plus its modifier, of table 0 and index 0, -3.
    const uint8_t block[8] = {100, 0x00, 0, 0, 0, 0, 0, 0};
    float texels[64];
    prEtc2DecodeF32(PrEtc2Format_R11Snorm, block, texels);
    for (uint32_t i = 0; i < 16; ++i) {
        CHECK(texels[i * 4] == float(800 - 3) / 1023.0f);
        CHECK(texels[i * 4 + 1] == 0.0f);
        CHECK(texels[i * 4 + 3] == 1.0f);
    }
    // A base of -128 reads as -127. With a multiplier of 15 and modifier -3, the value clamps to -1023. The second
    // channel's base of 127 and modifier -3 at a multiplier of one is 1016 - 24.
    const uint8_t two[16] = {0x80, 0xf0, 0, 0, 0, 0, 0, 0, 0x7f, 0x10, 0, 0, 0, 0, 0, 0};
    prEtc2DecodeF32(PrEtc2Format_Rg11Snorm, two, texels);
    CHECK(texels[0] == -1.0f);
    CHECK(texels[1] == float(1016 - 24) / 1023.0f);
    CHECK(texels[2] == 0.0f);
}

TEST_CASE("the modes that ETC2 adds to ETC1 decode to the colors that the ETC2 specification gives") {
    uint8_t texels[64];
    // T mode. The first color is (15, 0, 0). The second is (0, 15, 0), with the two colors at a distance of 3 to either
    // side of it. Every texel has the second of the four colors.
    const uint8_t kT[8] = {0xFB, 0x00, 0x0F, 0x02, 0x00, 0x00, 0xFF, 0xFF};
    prEtc2Decode(PrEtc2Format_Rgb8, kT, texels);
    for (uint32_t i = 0; i < 16; ++i) {
        CHECK(texels[i * 4] == 3);
        CHECK(texels[i * 4 + 1] == 255);
        CHECK(texels[i * 4 + 2] == 3);
        CHECK(texels[i * 4 + 3] == 255);
    }
    // H mode. The colors are those at a distance of 6 to either side of (8, 1, 10) and of (0, 0, 15). Every texel has
    // the last of the four colors.
    const uint8_t kH[8] = {0x40, 0xF9, 0x00, 0x7A, 0xFF, 0xFF, 0xFF, 0xFF};
    prEtc2Decode(PrEtc2Format_Rgb8, kH, texels);
    for (uint32_t i = 0; i < 16; ++i) {
        CHECK(texels[i * 4] == 0);
        CHECK(texels[i * 4 + 1] == 0);
        CHECK(texels[i * 4 + 2] == 249);
    }
    const uint8_t kHFirst[8] = {0x40, 0xF9, 0x00, 0x7A, 0x00, 0x00, 0x00, 0x00};
    prEtc2Decode(PrEtc2Format_Rgb8, kHFirst, texels);
    CHECK(texels[0] == 142);
    CHECK(texels[1] == 23);
    CHECK(texels[2] == 176);
    // Planar mode. Blue is 105 at the origin and along the rows, and 255 four texels down.
    const uint8_t kPlanar[8] = {0x00, 0x00, 0xF9, 0x02, 0x00, 0xD0, 0x00, 0x3F};
    prEtc2Decode(PrEtc2Format_Rgb8, kPlanar, texels);
    const uint8_t kBlues[4] = {105, 143, 180, 218};
    for (uint32_t i = 0; i < 16; ++i) {
        CHECK(texels[i * 4] == 0);
        CHECK(texels[i * 4 + 1] == 0);
        CHECK(texels[i * 4 + 2] == kBlues[i / 4]);
    }
    // In the format with one bit of alpha, texels with the third index are transparent, in this mode and in the ETC1
    // modes.
    const uint8_t kTransparent[8] = {0xFB, 0x00, 0x0F, 0x00, 0xFF, 0xFF, 0x00, 0x00};
    prEtc2Decode(PrEtc2Format_Rgb8A1, kTransparent, texels);
    for (uint32_t i = 0; i < 64; ++i)
        CHECK(texels[i] == 0);
    const uint8_t kPunched[8] = {0xF8, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00};
    prEtc2Decode(PrEtc2Format_Rgb8A1, kPunched, texels);
    for (uint32_t i = 0; i < 64; ++i)
        CHECK(texels[i] == 0);
    const uint8_t kUnmodified[8] = {0xF8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    prEtc2Decode(PrEtc2Format_Rgb8A1, kUnmodified, texels);
    CHECK(texels[0] == 255);
    CHECK(texels[1] == 0);
    CHECK(texels[3] == 255);
}

TEST_CASE("the etc2 header compiles as C") {
    CHECK(prTestEtc2CApi() == 0);
}
