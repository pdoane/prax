// DDS, DirectDraw Surface: the container of Direct3D textures. A file holds the mip levels of the faces and array
// elements of a texture, or of the slices of a volume. The texels are blocks of a format that a GPU samples (pr/gpu.h)
// or pixels, described by a DXGI format in the DX10 extension header, or by a legacy pixel format of a FourCC or of bit
// masks. A texture decodes to pixels if its pixels have a layout that the library reads, or if lib/bc decodes its
// blocks.
//
// DDS - https://learn.microsoft.com/en-us/windows/win32/direct3ddds/dx-graphics-dds-pguide
// DDS_HEADER_DXT10 - https://learn.microsoft.com/en-us/windows/win32/direct3ddds/dds-header-dxt10
// DXGI_FORMAT - https://learn.microsoft.com/en-us/windows/win32/api/dxgiformat/ne-dxgiformat-dxgi_format

#pragma once

#include "pr/base.h"
#include "pr/byte_view.h"
#include "pr/gpu.h"
#include "pr/pixels.h"
#include "pr/status.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The bytes of a file before its texels, without and with the DX10 extension header: the magic "DDS " and the headers.
// A side of 2^31 texels has the most mip levels.
enum {
    kPrDdsHeaderBytes = 128,
    kPrDdsDx10HeaderBytes = 148,
    kPrDdsMaxLevels = 32,
};

// Returns true if `bytes` begin with the magic of a DDS file.
bool prDdsHasSignature(PrByteView bytes);

// How the stored channels of a format of one or two channels read, for the legacy formats that DXGI names no format of.
enum {
    PrDdsSwizzle_None,              // as stored
    PrDdsSwizzle_Luminance,         // the one channel is gray
    PrDdsSwizzle_LuminanceAlpha,    // the first channel is gray, and the second alpha
};
typedef uint8_t PrDdsSwizzle;

// The storage type of one channel of the pixels that a texture decodes to. Values are in the machine's byte order.
// Unsigned integers map to [0, 1]. Floats hold the values of signed and float formats.
enum {
    PrDdsSample_U8,
    PrDdsSample_U16,
    PrDdsSample_F16,
    PrDdsSample_F32,
};
typedef uint8_t PrDdsSample;

// The relation between alpha and color in the texels, from the DX10 header's alpha mode, or the FourCC of DXT2 and
// DXT4, or the bit masks of a legacy format.
enum {
    PrDdsAlpha_None,
    PrDdsAlpha_Straight,
    PrDdsAlpha_Premultiplied,
};
typedef uint8_t PrDdsAlpha;

// The description of a texture, read from its headers.
//
// The frames of a texture are its array elements and faces. The faces of each element follow in turn, in the order +X,
// -X, +Y, -Y, +Z, -Z. Each frame has `levelCount` mip levels, each half the one before, rounded down, and at least one
// texel, on every axis (prDdsLevelExtent). A volume has one frame, and each of its levels holds its slices in turn.
//
// The stored texels are blocks of `blocks`, or pixels of `dxgiFormat` read with `swizzle`. `dxgiFormat` is the DXGI
// format of the texels, a legacy format's equivalent, or zero for a legacy format that DXGI has none of. The pixels
// that a texture decodes to have `channels` channels of `sample`: one is gray, two are gray and alpha, three are red,
// green and blue, and four add alpha. A format of fewer color channels decodes with the others zero.
typedef struct PrDdsInfo {
    PrUVec2 extent;             // of the first level, in texels
    uint32_t depth;             // the slices of a volume, or one
    uint32_t layerCount;        // the array elements, at least one
    uint32_t faceCount;         // one, or six for a cube
    uint32_t levelCount;        // at least one
    uint32_t dxgiFormat;
    PrDdsSwizzle swizzle;
    PrGpuFormat blocks;         // None for pixels
    bool decodes;               // true if the texels decode to pixels
    bool srgb;                  // true if the format states the sRGB transfer function
    uint8_t channels;
    PrDdsSample sample;
    PrDdsAlpha alpha;
} PrDdsInfo;

// Returns the extent of mip level `level` of a texture of `extent` texels.
PrUVec2 prDdsLevelExtent(PrUVec2 extent, uint32_t level);

// A texture open for reading. Any number of threads may read it at once.
typedef struct PrDdsTexture PrDdsTexture;

// Reads the headers of the texture in `bytes`, which must stay valid until the texture is destroyed. Returns
// InvalidData without the magic or for headers that break the format. Returns NotSupported for a format that the
// library does not read, such as YUV or palette indices, for a cube without all six faces, and for an array of volumes.
// Returns Truncated if the file ends before its texels do, with the texture open: a read of a part that the bytes do
// not hold fails. `*out` is the texture if the status is Ok or Truncated, and null otherwise.
PrStatus prDdsTextureCreate(PrByteView bytes, PrDdsTexture* PR_NULLABLE* PR_NONNULL out);

// Frees the texture.
void prDdsTextureDestroy(PrDdsTexture* PR_NULLABLE texture);

// Returns the description of the texture.
const PrDdsInfo* prDdsTextureInfo(const PrDdsTexture* texture);

// Returns the size in bytes of `level` of one frame as the file stores it: rows of blocks or of pixels with nothing
// between them, and the slices of a volume in turn. The texture must have `level`.
size_t prDdsTextureLevelSize(const PrDdsTexture* texture, uint32_t level);

// Copies `level` of `frame` as stored to `out`, which has room for prDdsTextureLevelSize bytes. Returns Truncated if
// the bytes end before the level does.
PrStatus prDdsTextureReadStored(const PrDdsTexture* texture, uint32_t frame, uint32_t level, uint8_t* out, size_t size);

// Decodes `level` of `frame` into `output`, of the format in the texture's info and of the level's extent, with the
// slices of a volume one below another. Returns NotSupported if the texels do not decode, and Truncated if the bytes
// end before the level does.
PrStatus prDdsTextureRead(const PrDdsTexture* texture, uint32_t frame, uint32_t level, PrPixels output);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
