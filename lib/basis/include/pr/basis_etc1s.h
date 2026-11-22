// Basis Universal ETC1S. It is one of the two formats that a KTX 2.0 texture can hold in place of a format that a GPU
// samples. A texture is blocks of the ETC1 subset. Each block is two indices into codebooks that the whole texture
// shares. The indices are entropy coded (BasisLZ).
//
// KTX File Format Specification, version 2, appendices C and D -
// https://registry.khronos.org/KTX/specs/2.0/ktxspec.v2.html

#pragma once

#include "pr/base.h"
#include "pr/byte_view.h"
#include "pr/status.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The data that the slices of an ETC1S texture share. It holds the endpoint and selector codebooks, the Huffman codes
// of a slice, and the location of each image's slices in its level.
typedef struct PrBasisEtc1s PrBasisEtc1s;

// The location of the slices of one image in the bytes of its mip level. An image without a second slice has an
// `alphaSize` of zero.
typedef struct PrBasisEtc1sImage {
    uint32_t colorOffset;
    uint32_t colorSize;
    uint32_t alphaOffset;
    uint32_t alphaSize;
} PrBasisEtc1sImage;

// Reads the global data of a texture of `imageCount` images. The images are the levels of the faces of each layer,
// ordered by level, then layer, then face. Returns InvalidData if the data does not follow the specification. Returns
// NotSupported if an image depends on the image before it, as a video frame does. On failure `*out` is null.
PrStatus prBasisEtc1sCreate(PrByteView global, uint32_t imageCount, PrBasisEtc1s* PR_NULLABLE* PR_NONNULL out);

// Frees the data that prBasisEtc1sCreate read.
void prBasisEtc1sDestroy(PrBasisEtc1s* PR_NULLABLE etc1s);

// Returns the number of bytes of memory that the texture's shared data takes after it is read.
size_t prBasisEtc1sWorkingBytes(const PrBasisEtc1s* etc1s);

// Returns the location of the slices of image `index`.
PrBasisEtc1sImage prBasisEtc1sImage(const PrBasisEtc1s* etc1s, uint32_t index);

// The output format of a slice decode.
enum {
    PrBasisEtc1sTarget_Pixels,
    PrBasisEtc1sTarget_Bc1,       // BC1 blocks of the slice's color
    PrBasisEtc1sTarget_Bc3Color,  // four-color BC1 blocks of the slice's color, the form that BC3 uses for color
    PrBasisEtc1sTarget_Bc4,       // BC4 blocks of one channel of the slice's color
    PrBasisEtc1sTarget_Etc1,      // ETC1 blocks of the slice's color, which are also ETC2 RGB8 blocks
    PrBasisEtc1sTarget_EacAlpha,  // 8-bit EAC blocks of one channel of the slice's color, as in ETC2 RGBA8 alpha
    PrBasisEtc1sTarget_EacR11,    // EAC R11 blocks of one channel of the slice's color
};
typedef uint8_t PrBasisEtc1sTarget;

// Decodes `slice`, an image of `extent` texels, into pixels at `out`. Each pixel is `pixelBytes` bytes, and rows are
// `stride` bytes apart. The function writes `count` bytes of each pixel, starting at byte `offset`. A count of three
// writes red, green and blue. A count of one writes channel `component` of the slice's color, where zero is red and one
// is green. Returns false if the slice is corrupt, and the pixels are then unspecified.
//
// For a block target, the function writes the 8 bytes of each block instead. Each block has `pixelBytes` bytes, and the
// function writes its 8 bytes starting at byte `offset`. Rows of blocks are `stride` bytes apart. The blocks are BC1 of
// the color, or BC4 or EAC of channel `component`.
//
// An ETC1 block is the slice's own block, so nothing is lost. A BC1 or EAC block is the block whose colors or values
// are nearest to those of the slice's block. The library's tables hold it for the base color, the modifiers and the
// selectors that the slice's block uses. Basis Universal's transcoder writes the same blocks. A BC4 block also comes
// from tables. Its endpoints are the pair whose eight values are nearest to the values of the selectors that the
// slice's block uses.
bool prBasisEtc1sDecodeSlice(const PrBasisEtc1s* etc1s, PrByteView slice, PrUVec2 extent, PrBasisEtc1sTarget target,
    uint8_t* out, size_t stride, uint32_t pixelBytes, uint32_t offset, uint32_t count, uint32_t component);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
