// Tests of the Basis Universal library of GPU blocks: the sizes of the formats, the decoding of blocks to pixels, and
// UASTC blocks. tools/suite/basis compares the ETC1S and UASTC decoders with the reference transcoder over whole
// textures. The tests of lib/ktx read both formats through KTX 2.0.

#include "pr/basis.h"
#include "pr/basis_etc1s.h"
#include "pr/basis_uastc.h"
#include "pr/mem.h"

#include "tools/harness/test.h"

extern "C" int prTestBasisCApi(void);

TEST_CASE("a block format reports the extent of a block in texels and its size in bytes") {
    CHECK(prBasisBlocksExtent(PrBasisBlocks_None) == PrUVec2{0, 0});
    CHECK(prBasisBlocksBytes(PrBasisBlocks_None) == 0);
    CHECK(prBasisBlocksExtent(PrBasisBlocks_Bc1) == PrUVec2{4, 4});
    CHECK(prBasisBlocksBytes(PrBasisBlocks_Bc1) == 8);
    CHECK(prBasisBlocksBytes(PrBasisBlocks_Bc7) == 16);
    CHECK(prBasisBlocksBytes(PrBasisBlocks_EacR11) == 8);
    CHECK(prBasisBlocksBytes(PrBasisBlocks_EacRg11) == 16);
    CHECK(prBasisBlocksExtent(PrBasisBlocks_Astc6x5Hdr) == PrUVec2{6, 5});
    CHECK(prBasisBlocksExtent(PrBasisBlocks_Astc12x12) == PrUVec2{12, 12});
    CHECK(prBasisBlocksBytes(PrBasisBlocks_Astc12x12Hdr) == 16);
    // The last blocks of a level extend past its texels.
    CHECK(prBasisBlocksSize(PrUVec2{5, 4}, PrBasisBlocks_Bc1) == 16);
    CHECK(prBasisBlocksSize(PrUVec2{13, 1}, PrBasisBlocks_Astc12x12) == 32);
    CHECK(prBasisBlocksSize(PrUVec2{4, 4}, PrBasisBlocks_None) == 0);
}

TEST_CASE("the BC and ETC2 formats with 8-bit texels decode, and the other formats do not") {
    CHECK(prBasisBlocksDecodes(PrBasisBlocks_Bc1));
    CHECK(prBasisBlocksDecodes(PrBasisBlocks_Bc7));
    CHECK(prBasisBlocksDecodes(PrBasisBlocks_Etc2Rgba8));
    CHECK(prBasisBlocksDecodes(PrBasisBlocks_EacRg11));
    CHECK_FALSE(prBasisBlocksDecodes(PrBasisBlocks_None));
    CHECK_FALSE(prBasisBlocksDecodes(PrBasisBlocks_Bc4Snorm));
    CHECK_FALSE(prBasisBlocksDecodes(PrBasisBlocks_Bc6HUfloat));
    CHECK_FALSE(prBasisBlocksDecodes(PrBasisBlocks_Astc4x4));
}

TEST_CASE("a BC1 block decodes to its colors, and a BC4 block decodes to its value in the red channel") {
    // The ends are white and black. Every texel has the first end, so the block is white.
    const uint8_t white[8] = {0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    uint8_t texels[64];
    prBasisBlockDecode(PrBasisBlocks_Bc1, white, texels);
    for (uint32_t i = 0; i < 64; ++i)
        CHECK(texels[i] == 255);
    // Every texel has the second end, so the block is black.
    const uint8_t black[8] = {0xFF, 0xFF, 0x00, 0x00, 0x55, 0x55, 0x55, 0x55};
    prBasisBlockDecode(PrBasisBlocks_Bc1, black, texels);
    CHECK(texels[0] == 0);
    CHECK(texels[3] == 255);
    // A BC4 block whose ends are 200 and 100. Every texel has the first end.
    const uint8_t value[8] = {200, 100, 0, 0, 0, 0, 0, 0};
    prBasisBlockDecode(PrBasisBlocks_Bc4, value, texels);
    for (uint32_t i = 0; i < 16; ++i) {
        CHECK(texels[i * 4] == 200);
        CHECK(texels[i * 4 + 1] == 0);
        CHECK(texels[i * 4 + 3] == 255);
    }
}

TEST_CASE("creating an ETC1S decoder from invalid global data fails") {
    PrBasisEtc1s* etc1s = nullptr;
    const uint8_t nothing[4] = {};
    CHECK(prBasisEtc1sCreate(PrByteView{nothing, sizeof(nothing)}, 1, &etc1s) == PrStatus_InvalidData);
    CHECK(etc1s == nullptr);
    prBasisEtc1sDestroy(etc1s);
}

TEST_CASE("the modes that ETC2 adds to ETC1 decode to the colors that the ETC2 specification gives") {
    uint8_t texels[64];
    // T mode. The first color is (15, 0, 0). The second is (0, 15, 0), with the two colors at a distance of 3 to either
    // side of it. Every texel has the second of the four colors.
    const uint8_t kT[8] = {0xFB, 0x00, 0x0F, 0x02, 0x00, 0x00, 0xFF, 0xFF};
    prBasisBlockDecode(PrBasisBlocks_Etc2Rgb8, kT, texels);
    for (uint32_t i = 0; i < 16; ++i) {
        CHECK(texels[i * 4] == 3);
        CHECK(texels[i * 4 + 1] == 255);
        CHECK(texels[i * 4 + 2] == 3);
        CHECK(texels[i * 4 + 3] == 255);
    }
    // H mode. The colors are those at a distance of 6 to either side of (8, 1, 10) and of (0, 0, 15). Every texel has
    // the last of the four colors.
    const uint8_t kH[8] = {0x40, 0xF9, 0x00, 0x7A, 0xFF, 0xFF, 0xFF, 0xFF};
    prBasisBlockDecode(PrBasisBlocks_Etc2Rgb8, kH, texels);
    for (uint32_t i = 0; i < 16; ++i) {
        CHECK(texels[i * 4] == 0);
        CHECK(texels[i * 4 + 1] == 0);
        CHECK(texels[i * 4 + 2] == 249);
    }
    const uint8_t kHFirst[8] = {0x40, 0xF9, 0x00, 0x7A, 0x00, 0x00, 0x00, 0x00};
    prBasisBlockDecode(PrBasisBlocks_Etc2Rgb8, kHFirst, texels);
    CHECK(texels[0] == 142);
    CHECK(texels[1] == 23);
    CHECK(texels[2] == 176);
    // Planar mode. Blue is 105 at the origin and along the rows, and 255 four texels down.
    const uint8_t kPlanar[8] = {0x00, 0x00, 0xF9, 0x02, 0x00, 0xD0, 0x00, 0x3F};
    prBasisBlockDecode(PrBasisBlocks_Etc2Rgb8, kPlanar, texels);
    const uint8_t kBlues[4] = {105, 143, 180, 218};
    for (uint32_t i = 0; i < 16; ++i) {
        CHECK(texels[i * 4] == 0);
        CHECK(texels[i * 4 + 1] == 0);
        CHECK(texels[i * 4 + 2] == kBlues[i / 4]);
    }
    // In the format with one bit of alpha, texels with the third index are transparent, in this mode and in the ETC1
    // modes.
    const uint8_t kTransparent[8] = {0xFB, 0x00, 0x0F, 0x00, 0xFF, 0xFF, 0x00, 0x00};
    prBasisBlockDecode(PrBasisBlocks_Etc2Rgb8A1, kTransparent, texels);
    for (uint32_t i = 0; i < 64; ++i)
        CHECK(texels[i] == 0);
    const uint8_t kPunched[8] = {0xF8, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00};
    prBasisBlockDecode(PrBasisBlocks_Etc2Rgb8A1, kPunched, texels);
    for (uint32_t i = 0; i < 64; ++i)
        CHECK(texels[i] == 0);
    const uint8_t kUnmodified[8] = {0xF8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    prBasisBlockDecode(PrBasisBlocks_Etc2Rgb8A1, kUnmodified, texels);
    CHECK(texels[0] == 255);
    CHECK(texels[1] == 0);
    CHECK(texels[3] == 255);
}

TEST_CASE("basis transcodes a single-color UASTC block to a BC7 block of mode 5 or 6, and refuses the reserved mode") {
    // A mode 8 block of red 10, green 20, blue 30 and alpha 40. BC7 mode 5 makes that color with every color index 1.
    uint8_t block[16] = {};
    uint64_t bits = 0x17 | uint64_t(10) << 5 | uint64_t(20) << 13 | uint64_t(30) << 21 | uint64_t(40) << 29;
    for (int k = 0; k < 8; ++k)
        block[k] = uint8_t(bits >> (8 * k));
    uint8_t bc7[16] = {};
    REQUIRE(prBasisUastcTranscodeBc7(block, bc7));
    // The block is mode 5 or mode 6, so the mode bit is the sixth or the seventh bit.
    CHECK((bc7[0] == 0x20 || (bc7[0] & 0x7F) == 0x40));
    block[0] = 0x45;
    CHECK(!prBasisUastcTranscodeBc7(block, bc7));
}

TEST_CASE("basis transcodes a single-color UASTC block to a constant-color ASTC block with 16 bits per channel") {
    uint8_t block[16] = {};
    uint64_t bits = 0x17 | uint64_t(10) << 5 | uint64_t(20) << 13 | uint64_t(30) << 21 | uint64_t(40) << 29;
    for (int k = 0; k < 8; ++k)
        block[k] = uint8_t(bits >> (8 * k));
    uint8_t astc[16] = {};
    REQUIRE(prBasisUastcTranscodeAstc(block, astc));
    // The block mode of a constant color with 16-bit values. The block has no extent, so those bits are all ones.
    CHECK(prMemReadLeU64(astc) == 0xFFFFFFFFFFFFFDFCull);
    CHECK(prMemReadLeU16(astc + 8) == 10 * 0x101);
    CHECK(prMemReadLeU16(astc + 10) == 20 * 0x101);
    CHECK(prMemReadLeU16(astc + 12) == 30 * 0x101);
    CHECK(prMemReadLeU16(astc + 14) == 40 * 0x101);
    block[0] = 0x45;
    CHECK(!prBasisUastcTranscodeAstc(block, astc));
}

TEST_CASE("basis decodes a single-color UASTC block, and refuses the mode that UASTC reserves") {
    // Mode 8 has the code 0x17 in five bits. Red, green, blue and alpha follow.
    uint8_t block[16] = {};
    uint64_t bits = 0x17 | uint64_t(10) << 5 | uint64_t(20) << 13 | uint64_t(30) << 21 | uint64_t(40) << 29;
    for (int k = 0; k < 8; ++k)
        block[k] = uint8_t(bits >> (8 * k));
    uint8_t texels[64] = {};
    REQUIRE(prBasisUastcDecodeBlock(block, texels));
    for (int i = 0; i < 16; ++i) {
        CHECK(texels[i * 4] == 10);
        CHECK(texels[i * 4 + 1] == 20);
        CHECK(texels[i * 4 + 2] == 30);
        CHECK(texels[i * 4 + 3] == 40);
    }
    // Mode 19 has the code 0x45 in seven bits.
    block[0] = 0x45;
    CHECK(!prBasisUastcDecodeBlock(block, texels));
}

TEST_CASE("Basis Universal's public headers compile as C") {
    CHECK(prTestBasisCApi() == 0);
}
