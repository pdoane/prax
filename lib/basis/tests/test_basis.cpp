// Tests of the Basis Universal library: ETC1S global data and UASTC blocks. tools/suite/basis compares the ETC1S and
// UASTC decoders with the reference transcoder over whole textures. The tests of lib/ktx read both formats through KTX
// 2.0.

#include "pr/basis_etc1s.h"
#include "pr/basis_uastc.h"
#include "pr/mem.h"

#include "tools/harness/test.h"

extern "C" int prTestBasisCApi(void);

TEST_CASE("creating an ETC1S decoder from invalid global data fails") {
    PrBasisEtc1s* etc1s = nullptr;
    const uint8_t nothing[4] = {};
    CHECK(prBasisEtc1sCreate(PrByteView{nothing, sizeof(nothing)}, 1, &etc1s) == PrStatus_InvalidData);
    CHECK(etc1s == nullptr);
    prBasisEtc1sDestroy(etc1s);
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
