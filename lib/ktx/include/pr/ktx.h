// KTX 2.0, the container format for GPU textures. A file holds the mip levels of the layers and faces of a texture. The
// levels are blocks of a format that a GPU samples (pr/gpu.h), pixels, or one of the two Basis Universal formats, which
// transcode to GPU blocks. A block texture decodes to pixels if lib/bc or lib/etc2 decodes its blocks. A texture in a
// Basis Universal format also decodes to pixels. A level that is supercompressed with Zstandard or zlib inflates
// through the compression codec that the texture is given.
//
// KTX File Format Specification, version 2 - https://registry.khronos.org/KTX/specs/2.0/ktxspec.v2.html

#pragma once

#include "pr/base.h"
#include "pr/byte_view.h"
#include "pr/compress.h"
#include "pr/gpu.h"
#include "pr/pixels.h"
#include "pr/status.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The size in bytes of a file's header, and the maximum number of mip levels in a texture. The header locates
// everything else that comes before the level data. A texture with a side of 2^31 texels has the maximum number of
// levels.
enum {
    kPrKtxHeaderBytes = 80,
    kPrKtxMaxLevels = 32,
    kPrKtxMaxTranscodes = 4,
};

// Returns true if `bytes` begin with the identifier of a KTX 2.0 file.
bool prKtxHasSignature(PrByteView bytes);

// Returns the number of bytes from the start of a file that hold everything before the level data. `start` is the first
// bytes of the file. The count covers the header, the level index, the descriptor, the key and value data and the
// supercompression global data. The header locates the other parts. Returns kPrKtxHeaderBytes if `start` is shorter
// than the header and may begin a KTX 2.0 file. Returns zero if `start` does not begin a KTX 2.0 file. A texture
// created from the returned number of bytes reports all of its info, and its status is Truncated.
uint64_t prKtxInfoBytes(PrByteView start);

// The form of the texel data in a level.
enum {
    PrKtxPayload_Pixels,    // rows of texels with one to four channels of one sample type
    PrKtxPayload_Blocks,    // blocks of a format that a GPU samples
    PrKtxPayload_Etc1s,     // Basis Universal ETC1S slices, decoded with the texture's codebooks (BasisLZ)
    PrKtxPayload_Uastc,     // Basis Universal UASTC blocks
};
typedef uint8_t PrKtxPayload;

// The supercompression scheme applied to the payload of a level.
enum {
    PrKtxScheme_None,
    PrKtxScheme_BasisLz,    // the payload is ETC1S
    PrKtxScheme_Zstd,
    PrKtxScheme_Zlib,
};
typedef uint8_t PrKtxScheme;

// The storage type of one channel of the pixels that a texture decodes to. Values are in the machine's byte order.
// Unsigned integers map to [0, 1].
enum {
    PrKtxSample_U8,
    PrKtxSample_U16,
    PrKtxSample_F16,
    PrKtxSample_F32,
};
typedef uint8_t PrKtxSample;

// The relation between alpha and color in the pixels that a texture decodes to.
enum {
    PrKtxAlpha_None,
    PrKtxAlpha_Straight,
    PrKtxAlpha_Premultiplied,
};
typedef uint8_t PrKtxAlpha;

// The meaning of the values in a texture's color channels. It comes from the transfer function and the primaries in the
// texture's descriptor.
enum {
    PrKtxColor_Data,        // not color: linear values with unspecified primaries are data
    PrKtxColor_Srgb,        // sRGB encoded
    PrKtxColor_SrgbLinear,  // linear light in the primaries of BT.709
};
typedef uint8_t PrKtxColor;

// The description of a texture, read from its header, level index, descriptor and metadata.
//
// The frames of a texture are its layers and faces. The faces of each layer follow in turn, in the order +X, -X, +Y,
// -Y, +Z, -Z. Each frame has `levelCount` mip levels. Each level is half the size of the level before it, rounded down
// (prKtxLevelExtent).
//
// The texture decodes to rows of pixels with `channels` channels of type `sample`. One channel is gray. Three channels
// are red, green and blue. Four channels are red, green, blue and alpha. A texture of two values decodes to red and
// green, with blue zero.
typedef struct PrKtxInfo {
    PrUVec2 extent;             // size of the first level in texels
    uint32_t layerCount;        // at least one
    uint32_t faceCount;         // one, or six for a cube
    uint32_t levelCount;        // at least one
    PrKtxPayload payload;
    PrKtxScheme scheme;
    PrGpuFormat blocks;         // the plain form of the block format of a block texture, None for any other
    bool blocksDecode;          // true if the texture's blocks decode to pixels

    // The plain forms of the block formats that prKtxTextureTranscode produces.
    PrGpuFormat transcodes[kPrKtxMaxTranscodes];
    uint32_t transcodeCount;
    uint8_t channels;
    PrKtxSample sample;
    PrKtxAlpha alpha;
    PrKtxColor color;

    // The texture's KTXorientation. `fromRight` is true if the columns run from the right. `fromBottom` is true if the
    // rows run from the bottom.
    bool fromRight;
    bool fromBottom;

    // The inflated size in bytes of the first level, which is the largest. A read of that level holds this much memory
    // while it runs. Zero if the level is read in place from the file.
    uint64_t inflatedBytes;
} PrKtxInfo;

// Returns the extent of mip level `level` of a texture of `extent` texels. Each level is half the level before it,
// rounded down, and at least one texel on each axis.
PrUVec2 prKtxLevelExtent(PrUVec2 extent, uint32_t level);

// A texture open for reading. After prKtxTexturePrepare, any number of threads may read the texture at once.
typedef struct PrKtxTexture PrKtxTexture;

// Reads the texture in `bytes` without its level data. `bytes` must stay valid until the texture is destroyed.
// Supercompressed levels inflate through `codec`, which must outlive the texture. Without a codec, those levels are
// NotSupported. Returns InvalidData if the structure of the file does not follow the specification. Returns
// NotSupported for a texture that the library does not read: a volume, a format that is not a Basis Universal format,
// pixels, BC, ETC2, EAC or ASTC blocks, a supercompression scheme that the specification does not define, or an
// animation whose frames depend on other frames. Returns Truncated if the file ends before its levels do. The rest of
// the file is still read. `*out` is the texture if the status is Ok or Truncated, and null otherwise.
PrStatus prKtxTextureCreate(PrByteView bytes, const PrCompressCodecSet* PR_NULLABLE codec,
    PrKtxTexture* PR_NULLABLE* PR_NONNULL out);

// Frees the texture.
void prKtxTextureDestroy(PrKtxTexture* PR_NULLABLE texture);

// Returns the description of the texture.
const PrKtxInfo* prKtxTextureInfo(const PrKtxTexture* texture);

// Reads the data that the levels of the texture share, which is the ETC1S codebooks. Every read needs it. Returns
// InvalidData if the data does not follow the specification. Call it once, before the first read.
PrStatus prKtxTexturePrepare(PrKtxTexture* texture);

// Returns the number of bytes that prKtxTexturePrepare allocated.
uint64_t prKtxTextureSharedBytes(const PrKtxTexture* texture);

// Returns the size in bytes of the blocks of `level` of a block texture, or zero for any other texture. The texture
// must have `level`.
size_t prKtxTextureBlocksSize(const PrKtxTexture* texture, uint32_t level);

// Decodes `level` of `frame` into `output`. The texture must have that level and frame. `output` has the extent of the
// level and the format in the texture's info. The result is the pixels that the level holds, or the pixels that its
// blocks or its Basis Universal data decode to. Returns InvalidData if the level is corrupt or does not inflate to the
// size that the texture gives for it. Returns NotSupported if the blocks do not decode or the codec does not decode the
// scheme. Returns Truncated if the texture's bytes end before the level does.
PrStatus prKtxTextureRead(const PrKtxTexture* texture, uint32_t frame, uint32_t level, PrPixels output);

// Copies the blocks of `level` of `frame` of a block texture to `out`, which has room for prKtxTextureBlocksSize bytes.
// The blocks are in rows with nothing between them. Returns NotSupported if the texture is not a block texture.
// Otherwise it fails as prKtxTextureRead does.
PrStatus prKtxTextureReadBlocks(const PrKtxTexture* texture, uint32_t frame, uint32_t level, uint8_t* out, size_t size);

// Transcodes `level` of `frame` of a texture in a Basis Universal format to blocks of `target` at `out`, without
// decoding the level to pixels. `target` is one of the texture's `transcodes`. `out` has room for the bytes of the
// level in `target` (prGpuFormatLevelBytes). The blocks are in rows, as prKtxTextureReadBlocks writes them. Color
// becomes BC1 or ETC2 RGB8. Color with alpha becomes BC3 or ETC2 RGBA8. One value becomes BC4 or EAC R11. Two values
// become BC5 or EAC RG11. UASTC becomes BC7 or ASTC 4x4. Returns NotSupported if `target` is not among the texture's
// `transcodes`. Otherwise it fails as prKtxTextureRead does.
PrStatus prKtxTextureTranscode(const PrKtxTexture* texture, uint32_t frame, uint32_t level, PrGpuFormat target,
    uint8_t* out, size_t size);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
