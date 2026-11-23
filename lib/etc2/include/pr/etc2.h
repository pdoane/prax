// ETC2 and EAC: the block-compressed formats of OpenGL ES 3 and Vulkan, which a GPU samples, and their decoding to
// pixels in software, for callers that want pixels. A block covers 4 by 4 texels. An image is rows of blocks, starting
// with the first row of texels. ETC1's blocks are blocks of ETC2 RGB8. The library knows the formats apart from any GPU
// interface, so that a container or a transcoder that holds or produces blocks depends on it alone.
//
// The decoding follows chapter 22 of the Khronos Data Format Specification 1.4: base colors of 4 to 7 bits widen by
// replicating their top bits, the planar mode rounds as section 22.1 says, and an EAC value of 11 bits is its value
// over 2047 or 1023, which an 8-bit decode rounds to the nearest.
//
// Khronos Data Format Specification 1.4 - https://registry.khronos.org/DataFormat/specs/1.4/dataformat.1.4.html

#pragma once

#include "pr/base.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The ETC2 and EAC formats. A format does not say whether its texels are sRGB encoded. The texture's color says that.
enum {
    PrEtc2Format_Rgb8,
    PrEtc2Format_Rgb8A1,    // RGB with one bit of alpha
    PrEtc2Format_Rgba8,     // RGB as Rgb8 after alpha as EAC, of 8 bits
    PrEtc2Format_R11,       // one channel of 11 bits, unsigned
    PrEtc2Format_R11Snorm,
    PrEtc2Format_Rg11,      // two channels, each as R11
    PrEtc2Format_Rg11Snorm,
    PrEtc2Format_Count,
};
typedef uint8_t PrEtc2Format;

// The texels that the blocks of a format decode to: 8 bits a channel (prEtc2Decode), or floats (prEtc2DecodeF32), which
// hold the values of the signed formats.
enum {
    PrEtc2Texels_None,
    PrEtc2Texels_U8,
    PrEtc2Texels_F32,
};
typedef uint8_t PrEtc2Texels;

// Returns the texels that the blocks of `format` decode to: floats for the signed formats of EAC, and 8 bits a channel
// for the others.
PrEtc2Texels prEtc2FormatTexels(PrEtc2Format format);

// Returns true if the blocks of `format` decode, to either kind of texel. Every ETC2 and EAC format does.
bool prEtc2FormatDecodes(PrEtc2Format format);

// Decodes the block of `format` at `block` to its 16 texels at `texels`, which has room for 64 bytes. The texels are in
// rows, starting with the first row. Each texel is red, green, blue and alpha, 8 bits each. If the format lacks a
// channel, green and blue are zero and alpha is 255. The 11 bits of EAC are rounded to 8. `format` must be one of 8-bit
// texels (prEtc2FormatTexels).
void prEtc2Decode(PrEtc2Format format, const uint8_t* block, uint8_t* texels);

// Decodes the block of `format` at `block` to its 16 texels at `texels`, which has room for 64 floats, in the order of
// prEtc2Decode. A channel that the format lacks is zero, and alpha is one. A signed EAC value is its 11 bits over 1023,
// in [-1, 1]. A format of 8-bit texels gives the values of prEtc2Decode over 255.
void prEtc2DecodeF32(PrEtc2Format format, const uint8_t* block, float* texels);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
