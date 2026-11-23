// BC: the block-compressed formats of Direct3D, BC1 to BC7, which a GPU samples, and their decoding to pixels in
// software, for callers that want pixels. A block covers 4 by 4 texels. An image is rows of blocks, starting with the
// first row of texels. The library knows the formats apart from any GPU interface, so that a container or a transcoder
// that holds or produces blocks depends on it alone.
//
// The decoding follows the Khronos Data Format Specification 1.4 for the block layouts, the modes and the weights of
// the interpolation: BC1 to BC3 in chapter 18 (S3TC), BC4 and BC5 in chapter 19 (RGTC), and BC6H and BC7 in chapter 20
// (BPTC). For BC6H and BC7 the specification gives integer formulas, which the decode follows bit for bit.
//
// For BC1 to BC5 the specification gives real values: an end of n bits is its value over 2^n - 1, and the values
// between the ends are weighted means of them. It leaves open how a decode to 8 bits converts those. lib/bc widens an
// end to 8 bits by repeating its bits, as the rest of the repository widens a channel (prPixelChannel8), and
// interpolates the 8-bit ends in integers, dropping the fraction. That is what common decoders do, among them the
// reference unpacker of Basis Universal and Pillow, whose texels lib/bc's equal; each differs from the nearest 8-bit
// value to the real one by at most one. The decodes to floats, of signed BC4 and BC5, take the real values as given.
//
// Khronos Data Format Specification 1.4 - https://registry.khronos.org/DataFormat/specs/1.4/dataformat.1.4.html

#pragma once

#include "pr/base.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The BC formats. A format does not say whether its texels are sRGB encoded. The texture's color says that.
enum {
    PrBcFormat_Bc1,         // RGB with one bit of alpha
    PrBcFormat_Bc2,         // RGB as Bc1, and four bits of alpha a texel
    PrBcFormat_Bc3,         // RGB as Bc1, and alpha as Bc4
    PrBcFormat_Bc4,         // one channel, unsigned
    PrBcFormat_Bc4Snorm,
    PrBcFormat_Bc5,         // two channels, each as Bc4
    PrBcFormat_Bc5Snorm,
    PrBcFormat_Bc6HUfloat,  // RGB of half floats, without a sign
    PrBcFormat_Bc6HSfloat,  // and with one
    PrBcFormat_Bc7,         // RGBA
    PrBcFormat_Count,
};
typedef uint8_t PrBcFormat;

// The texels that the blocks of a format decode to: 8 bits a channel (prBcDecode), or floats (prBcDecodeF32), which
// hold the values of the signed and the float formats.
enum {
    PrBcTexels_None,
    PrBcTexels_U8,
    PrBcTexels_F32,
};
typedef uint8_t PrBcTexels;

// Returns the texels that the blocks of `format` decode to. BC1, BC2, BC3, BC4, BC5 and BC7 decode to 8 bits a channel,
// and the signed BC4 and BC5 and both forms of BC6H to floats.
PrBcTexels prBcFormatTexels(PrBcFormat format);

// Returns true if the blocks of `format` decode, to either kind of texel. Every BC format does.
bool prBcFormatDecodes(PrBcFormat format);

// Decodes the block of `format` at `block` to its 16 texels at `texels`, which has room for 64 bytes. The texels are in
// rows, starting with the first row. Each texel is red, green, blue and alpha, 8 bits each. If the format lacks a
// channel, green and blue are zero and alpha is 255. `format` must be one of 8-bit texels (prBcFormatTexels).
void prBcDecode(PrBcFormat format, const uint8_t* block, uint8_t* texels);

// Decodes the block of `format` at `block` to its 16 texels at `texels`, which has room for 64 floats, in the order of
// prBcDecode. A channel that the format lacks is zero, and alpha is one. A signed BC4 or BC5 value is in [-1, 1], with
// -128 read as -127. A BC6H texel is the half float that the block encodes, of red, green and blue, and a block of a
// reserved mode is zero. A format of 8-bit texels gives the values of prBcDecode over 255.
void prBcDecodeF32(PrBcFormat format, const uint8_t* block, float* texels);

// A partition of the texels of a BC7 block into two or three subsets. `subsets` holds the subset of each texel, two
// bits each, from the first texel in the lowest bits. `anchors` holds the texel of each subset whose index is stored
// with one bit fewer: the first texel for the first subset, and zero for a subset that the partition does not have.
typedef struct PrBcPartition {
    uint32_t subsets;
    uint8_t anchors[3];
} PrBcPartition;

// Returns partition `index`, below 64, of the BC7 partitions of `subsetCount` subsets, two or three. BC6H's partitions
// of two subsets are the first 32 of them.
PrBcPartition prBcPartition(uint32_t subsetCount, uint32_t index);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
