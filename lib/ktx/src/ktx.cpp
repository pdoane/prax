// KTX 2.0 textures.
//
// A file is a header, an index of its parts, an index of its mip levels, a Data Format Descriptor, key/value metadata,
// global data for a supercompression scheme, and the levels, smallest first. The Data Format Descriptor says what the
// texels mean. All integers in the file are little-endian.

#include "pr/ktx.h"

#include "pr/align.h"
#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/basis_etc1s.h"
#include "pr/basis_uastc.h"
#include "pr/mem.h"

namespace pr_impl {

namespace {

constexpr uint8_t kIdentifier[12] = {0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, '\r', '\n', 0x1A, '\n'};

constexpr size_t kLevelIndexBytes = 24;

// Values in a descriptor: the color models that name the Basis Universal formats, the transfer functions, the
// primaries, and the flag for premultiplied alpha.
constexpr uint8_t kModelEtc1s = 163;
constexpr uint8_t kModelUastc = 166;
constexpr uint8_t kTransferLinear = 1;
constexpr uint8_t kTransferSrgb = 2;
constexpr uint8_t kPrimariesBt709 = 1;
constexpr uint8_t kFlagPremultiplied = 1;

// The contents of the channels of a Basis Universal texture, from the channel ids in its descriptor.
enum {
    Channels_Rgb,
    Channels_Rgba,
    Channels_R,             // one value, which ETC1S repeats in red, green and blue
    Channels_Rg,            // two values, in red and green
    Channels_RrrG,          // two values, the first in red, green and blue, the second in alpha
};
typedef uint8_t Channels;

// One mip level: the location of its bytes in the file, and their size after inflation.
struct Level {
    uint64_t offset;
    uint64_t size;
    uint64_t inflatedSize;
};

// A Vulkan format that the reader accepts. It is pixels with `channels` channels of type `sample`, or blocks.
struct VkFormat {
    uint32_t value;
    PrBasisBlocks blocks;
    uint8_t channels;
    PrKtxSample sample;
    bool bgr;
    bool alpha;         // true if a block format has alpha
};

constexpr VkFormat kFormats[] = {
    {9, PrBasisBlocks_None, 1, PrKtxSample_U8},                      // R8_UNORM
    {15, PrBasisBlocks_None, 1, PrKtxSample_U8},                     // R8_SRGB
    {16, PrBasisBlocks_None, 2, PrKtxSample_U8},                     // R8G8_UNORM
    {22, PrBasisBlocks_None, 2, PrKtxSample_U8},                     // R8G8_SRGB
    {23, PrBasisBlocks_None, 3, PrKtxSample_U8},                     // R8G8B8_UNORM
    {29, PrBasisBlocks_None, 3, PrKtxSample_U8},                     // R8G8B8_SRGB
    {30, PrBasisBlocks_None, 3, PrKtxSample_U8, true},               // B8G8R8_UNORM
    {36, PrBasisBlocks_None, 3, PrKtxSample_U8, true},               // B8G8R8_SRGB
    {37, PrBasisBlocks_None, 4, PrKtxSample_U8},                     // R8G8B8A8_UNORM
    {43, PrBasisBlocks_None, 4, PrKtxSample_U8},                     // R8G8B8A8_SRGB
    {44, PrBasisBlocks_None, 4, PrKtxSample_U8, true},               // B8G8R8A8_UNORM
    {50, PrBasisBlocks_None, 4, PrKtxSample_U8, true},               // B8G8R8A8_SRGB
    {70, PrBasisBlocks_None, 1, PrKtxSample_U16},                    // R16_UNORM
    {76, PrBasisBlocks_None, 1, PrKtxSample_F16},                    // R16_SFLOAT
    {77, PrBasisBlocks_None, 2, PrKtxSample_U16},                    // R16G16_UNORM
    {83, PrBasisBlocks_None, 2, PrKtxSample_F16},                    // R16G16_SFLOAT
    {84, PrBasisBlocks_None, 3, PrKtxSample_U16},                    // R16G16B16_UNORM
    {90, PrBasisBlocks_None, 3, PrKtxSample_F16},                    // R16G16B16_SFLOAT
    {91, PrBasisBlocks_None, 4, PrKtxSample_U16},                    // R16G16B16A16_UNORM
    {97, PrBasisBlocks_None, 4, PrKtxSample_F16},                    // R16G16B16A16_SFLOAT
    {100, PrBasisBlocks_None, 1, PrKtxSample_F32},                   // R32_SFLOAT
    {103, PrBasisBlocks_None, 2, PrKtxSample_F32},                   // R32G32_SFLOAT
    {106, PrBasisBlocks_None, 3, PrKtxSample_F32},                   // R32G32B32_SFLOAT
    {109, PrBasisBlocks_None, 4, PrKtxSample_F32},                   // R32G32B32A32_SFLOAT
    {131, PrBasisBlocks_Bc1, 3, PrKtxSample_U8},                     // BC1_RGB_UNORM_BLOCK
    {132, PrBasisBlocks_Bc1, 3, PrKtxSample_U8},                     // BC1_RGB_SRGB_BLOCK
    {133, PrBasisBlocks_Bc1, 3, PrKtxSample_U8, false, true},        // BC1_RGBA_UNORM_BLOCK
    {134, PrBasisBlocks_Bc1, 3, PrKtxSample_U8, false, true},        // BC1_RGBA_SRGB_BLOCK
    {135, PrBasisBlocks_Bc2, 3, PrKtxSample_U8, false, true},        // BC2_UNORM_BLOCK
    {136, PrBasisBlocks_Bc2, 3, PrKtxSample_U8, false, true},        // BC2_SRGB_BLOCK
    {137, PrBasisBlocks_Bc3, 3, PrKtxSample_U8, false, true},        // BC3_UNORM_BLOCK
    {138, PrBasisBlocks_Bc3, 3, PrKtxSample_U8, false, true},        // BC3_SRGB_BLOCK
    {139, PrBasisBlocks_Bc4, 1, PrKtxSample_U8},                     // BC4_UNORM_BLOCK
    {140, PrBasisBlocks_Bc4Snorm, 1, PrKtxSample_U8},                // BC4_SNORM_BLOCK
    {141, PrBasisBlocks_Bc5, 3, PrKtxSample_U8},                     // BC5_UNORM_BLOCK
    {142, PrBasisBlocks_Bc5Snorm, 3, PrKtxSample_U8},                // BC5_SNORM_BLOCK
    {143, PrBasisBlocks_Bc6HUfloat, 3, PrKtxSample_F16},             // BC6H_UFLOAT_BLOCK
    {144, PrBasisBlocks_Bc6HSfloat, 3, PrKtxSample_F16},             // BC6H_SFLOAT_BLOCK
    {145, PrBasisBlocks_Bc7, 3, PrKtxSample_U8, false, true},        // BC7_UNORM_BLOCK
    {146, PrBasisBlocks_Bc7, 3, PrKtxSample_U8, false, true},        // BC7_SRGB_BLOCK
    {147, PrBasisBlocks_Etc2Rgb8, 3, PrKtxSample_U8},                // ETC2_R8G8B8_UNORM_BLOCK
    {148, PrBasisBlocks_Etc2Rgb8, 3, PrKtxSample_U8},                // ETC2_R8G8B8_SRGB_BLOCK
    {149, PrBasisBlocks_Etc2Rgb8A1, 3, PrKtxSample_U8, false, true}, // ETC2_R8G8B8A1_UNORM_BLOCK
    {150, PrBasisBlocks_Etc2Rgb8A1, 3, PrKtxSample_U8, false, true}, // ETC2_R8G8B8A1_SRGB_BLOCK
    {151, PrBasisBlocks_Etc2Rgba8, 3, PrKtxSample_U8, false, true},  // ETC2_R8G8B8A8_UNORM_BLOCK
    {152, PrBasisBlocks_Etc2Rgba8, 3, PrKtxSample_U8, false, true},  // ETC2_R8G8B8A8_SRGB_BLOCK
    {153, PrBasisBlocks_EacR11, 1, PrKtxSample_U8},                  // EAC_R11_UNORM_BLOCK
    {154, PrBasisBlocks_EacR11Snorm, 1, PrKtxSample_U8},             // EAC_R11_SNORM_BLOCK
    {155, PrBasisBlocks_EacRg11, 3, PrKtxSample_U8},                 // EAC_R11G11_UNORM_BLOCK
    {156, PrBasisBlocks_EacRg11Snorm, 3, PrKtxSample_U8},            // EAC_R11G11_SNORM_BLOCK
};

// The ASTC formats are consecutive, ordered by block size. The LDR formats start at kAstcFirst, with a UNORM and then
// an SRGB format for each block size. The HDR formats start at kAstcHdrFirst, with one SFLOAT format for each block
// size.
constexpr uint32_t kAstcFirst = 157;
constexpr uint32_t kAstcHdrFirst = 1000066000;
constexpr uint32_t kAstcBlockSizes = 14;

// Finds the Vulkan format with the value `vkFormat`. Returns false if the reader does not accept that format.
bool findFormat(uint32_t vkFormat, VkFormat* out) {
    for (const VkFormat& candidate : kFormats) {
        if (candidate.value == vkFormat) {
            *out = candidate;
            return true;
        }
    }
    bool ldr = vkFormat >= kAstcFirst && vkFormat < kAstcFirst + 2 * kAstcBlockSizes;
    bool hdr = vkFormat >= kAstcHdrFirst && vkFormat < kAstcHdrFirst + kAstcBlockSizes;
    if (!ldr && !hdr)
        return false;
    uint32_t size = ldr ? (vkFormat - kAstcFirst) / 2 : vkFormat - kAstcHdrFirst;
    PrBasisBlocks blocks = PrBasisBlocks(PrBasisBlocks_Astc4x4 + 2 * size + (hdr ? 1 : 0));
    PrKtxSample sample = hdr ? PrKtxSample_F16 : PrKtxSample_U8;
    *out = VkFormat{vkFormat, blocks, 3, sample, false, true};
    return true;
}

// Returns true if the `size` bytes at `offset` lie in a file of `fileSize` bytes.
bool within(uint64_t offset, uint64_t size, uint64_t fileSize) {
    return offset <= fileSize && size <= fileSize - offset;
}

uint32_t sampleBytes(PrKtxSample sample) {
    return sample == PrKtxSample_U8 ? 1 : sample == PrKtxSample_F32 ? 4 : 2;
}

// Reads the orientation from the key/value data, if the data has the key. `l` reverses the columns and `u` reverses the
// rows. Returns false if the pairs do not fill the data exactly.
bool readOrientation(PrByteView data, PrKtxInfo* out) {
    constexpr char kKey[] = "KTXorientation";
    size_t at = 0;
    while (at < data.size) {
        if (data.size - at < 4)
            return false;
        uint32_t length = prMemReadLeU32(data.data + at);
        at += 4;
        if (length < 2 || length > data.size - at)
            return false;
        const uint8_t* pair = data.data + at;
        if (length >= sizeof(kKey) + 2 && prMemEquals(pair, kKey, sizeof(kKey))) {
            out->fromRight = pair[sizeof(kKey)] == 'l';
            out->fromBottom = pair[sizeof(kKey) + 1] == 'u';
        }
        // A pair is padded to four bytes.
        size_t padded = alignUp(size_t(length), size_t(4));
        at += padded < data.size - at ? padded : data.size - at;
    }
    return true;
}

// A texture open for reading.
struct KtxTexture {
    PrByteView bytes;
    const PrCompressCodecSet* PR_NULLABLE codec;
    PrKtxInfo info;
    Channels channels;              // channel contents of a Basis Universal payload
    uint8_t pixelChannels;          // Pixels payload: channel count of a stored texel, and whether blue is first
    bool pixelsBgr;
    PrByteView global;              // the supercompression global data, which is the BasisLZ codebooks
    Level levels[kPrKtxMaxLevels];
    PrBasisEtc1s* PR_NULLABLE etc1s;    // shared data of the slices of an ETC1S texture, set by prKtxTexturePrepare
};

// Returns the size in bytes of one pixel that the texture decodes to.
uint32_t pixelBytes(const KtxTexture& ktx) {
    return uint32_t(ktx.info.channels) * sampleBytes(ktx.info.sample);
}

// Returns the inflated size in bytes of one image of `level`. An image is one face of one layer. Returns zero for
// ETC1S, whose images are slices of the level.
uint64_t imageSize(const KtxTexture& ktx, uint32_t level) {
    PrUVec2 extent = prKtxLevelExtent(ktx.info.extent, level);
    if (ktx.info.payload == PrKtxPayload_Etc1s)
        return 0;
    if (ktx.info.payload == PrKtxPayload_Pixels)
        return uint64_t(extent.x) * extent.y * ktx.pixelChannels * sampleBytes(ktx.info.sample);
    // UASTC's blocks are of ASTC's size.
    return prBasisBlocksSize(extent, ktx.info.payload == PrKtxPayload_Uastc ? PrBasisBlocks_Astc4x4 : ktx.info.blocks);
}

// Returns a bit for each PrBasisBlocks that the texture transcodes to. Returns zero if the texture does not hold a
// Basis Universal format.
uint64_t transcodesOf(const KtxTexture& ktx) {
    // UASTC transcodes to BC7 and ASTC 4x4. The exception is a texture of two values that are not in red and green,
    // where the decoded pixels have them.
    if (ktx.info.payload == PrKtxPayload_Uastc && ktx.channels != Channels_RrrG)
        return uint64_t(1) << PrBasisBlocks_Bc7 | uint64_t(1) << PrBasisBlocks_Astc4x4;
    // ETC1S transcodes to the ETC2 and BC formats that have its channels. The entries are for color, color with alpha,
    // one value, and two values. One value is also gray as color.
    constexpr uint64_t kEtc1sTranscodes[] = {
        1u << PrBasisBlocks_Etc2Rgb8 | 1u << PrBasisBlocks_Bc1,
        1u << PrBasisBlocks_Etc2Rgba8 | 1u << PrBasisBlocks_Bc3,
        1u << PrBasisBlocks_EacR11 | 1u << PrBasisBlocks_Etc2Rgb8 | 1u << PrBasisBlocks_Bc4 | 1u << PrBasisBlocks_Bc1,
        1u << PrBasisBlocks_EacRg11 | 1u << PrBasisBlocks_Bc5,
    };
    return ktx.info.payload == PrKtxPayload_Etc1s ? kEtc1sTranscodes[ktx.channels] : 0;
}

// Returns the inflated bytes of `level` in `*out`. If the level is stored without supercompression, `*out` is a view of
// the file and `*storage` is null. Otherwise the function inflates the level through the texture's codec into
// `*storage`, which it allocates. The caller frees `*storage` with prMemoryFree.
PrStatus levelData(const KtxTexture& ktx, uint32_t level, uint8_t* PR_NULLABLE* PR_NONNULL storage, PrByteView* out) {
    *storage = nullptr;
    const Level& stored = ktx.levels[level];
    if (!within(stored.offset, stored.size, ktx.bytes.size))
        return PrStatus_Truncated;
    PrByteView data{ktx.bytes.data + stored.offset, size_t(stored.size)};
    if (ktx.info.scheme == PrKtxScheme_None || ktx.info.scheme == PrKtxScheme_BasisLz) {
        *out = data;
        return PrStatus_Ok;
    }
    PrCompressFormat format = ktx.info.scheme == PrKtxScheme_Zstd ? PrCompressFormat_Zstd : PrCompressFormat_Zlib;
    if (ktx.codec == nullptr || !prCompressCodecSetDecodes(ktx.codec, format))
        return PrStatus_NotSupported;
    size_t size = size_t(stored.inflatedSize);
    uint8_t* inflated = static_cast<uint8_t*>(prMemoryAlloc(size != 0 ? size : 1));
    PrCompressRead read = {};
    PrStatus status = prCompressCodecSetDecode(ktx.codec, format, data, inflated, size, &read);
    if (status != PrStatus_Ok || read.progress != PrCompressProgress_Done || read.written != size) {
        prMemoryFree(inflated);
        return status == PrStatus_NotSupported ? status : PrStatus_InvalidData;
    }
    *storage = inflated;
    *out = PrByteView{inflated, size};
    return PrStatus_Ok;
}

// Copies the pixels of a Pixels texture from `source` into the layout in the texture's info. One channel is gray. Two
// channels become red and green with blue zero. Pixels stored blue first become red first.
void copyPixels(const KtxTexture& ktx, const uint8_t* source, PrPixels output) {
    uint32_t channels = ktx.pixelChannels;
    uint32_t sample = sampleBytes(ktx.info.sample);
    for (uint32_t y = 0; y < output.extent.y; ++y) {
        uint8_t* to = output.data + size_t(y) * output.stride;
        const uint8_t* from = source + size_t(y) * output.extent.x * channels * sample;
        if (channels != 2 && !ktx.pixelsBgr) {
            prMemCopy(to, from, size_t(output.extent.x) * channels * sample);
            continue;
        }
        for (uint32_t x = 0; x < output.extent.x; ++x, from += channels * sample) {
            if (channels == 2) {
                prMemCopy(to, from, 2 * sample);
                prMemZero(to + 2 * sample, sample);
                to += 3 * sample;
                continue;
            }
            prMemCopy(to, from + 2 * sample, sample);
            prMemCopy(to + sample, from + sample, sample);
            prMemCopy(to + 2 * sample, from, sample);
            if (channels == 4)
                prMemCopy(to + 3 * sample, from + 3 * sample, sample);
            to += channels * sample;
        }
    }
}

// Decodes the UASTC blocks at `blocks` into pixels with the channels of the texture.
bool decodeUastc(const KtxTexture& ktx, const uint8_t* blocks, PrPixels output) {
    uint32_t blocksX = (output.extent.x + 3) / 4;
    uint32_t blocksY = (output.extent.y + 3) / 4;
    uint32_t bytes = pixelBytes(ktx);
    // Two values become red and green. The second value comes from alpha if the texture stores it there.
    uint32_t second = ktx.channels == Channels_RrrG ? 3 : 1;
    bool pair = ktx.channels == Channels_Rg || ktx.channels == Channels_RrrG;
    for (uint32_t blockY = 0; blockY < blocksY; ++blockY) {
        for (uint32_t blockX = 0; blockX < blocksX; ++blockX, blocks += 16) {
            uint8_t texels[64];
            if (!prBasisUastcDecodeBlock(blocks, texels))
                return false;
            uint32_t width = output.extent.x - blockX * 4 < 4 ? output.extent.x - blockX * 4 : 4;
            uint32_t height = output.extent.y - blockY * 4 < 4 ? output.extent.y - blockY * 4 : 4;
            for (uint32_t y = 0; y < height; ++y) {
                uint8_t* to = output.data + (size_t(blockY) * 4 + y) * output.stride + size_t(blockX) * 4 * bytes;
                const uint8_t* from = texels + y * 16;
                for (uint32_t x = 0; x < width; ++x, to += bytes, from += 4) {
                    if (pair) {
                        to[0] = from[0];
                        to[1] = from[second];
                        to[2] = 0;
                    } else {
                        prMemCopy(to, from, bytes);
                    }
                }
            }
        }
    }
    return true;
}

// Decodes the ETC1S slices of image `index` into pixels with the channels of the texture. `level` is the bytes of the
// level that holds the slices.
bool decodeEtc1s(const KtxTexture& ktx, PrByteView level, uint32_t index, PrPixels output) {
    PrBasisEtc1sImage image = prBasisEtc1sImage(ktx.etc1s, index);
    bool two = ktx.channels == Channels_Rgba || ktx.channels == Channels_Rg;
    if (image.colorSize == 0 || image.colorOffset > level.size || image.colorSize > level.size - image.colorOffset)
        return false;
    if (two && (image.alphaSize == 0 || image.alphaOffset > level.size ||
            image.alphaSize > level.size - image.alphaOffset))
        return false;
    PrByteView color{level.data + image.colorOffset, image.colorSize};
    PrByteView alpha{level.data + (two ? image.alphaOffset : 0), two ? image.alphaSize : 0};
    uint32_t bytes = pixelBytes(ktx);
    const PrBasisEtc1s* etc1s = ktx.etc1s;
    uint8_t* out = output.data;
    switch (ktx.channels) {
        case Channels_Rgb:
            return prBasisEtc1sDecodeSlice(etc1s, color, output.extent, PrBasisEtc1sTarget_Pixels, out, output.stride,
                bytes, 0, 3, 0);
        case Channels_Rgba:
            return prBasisEtc1sDecodeSlice(etc1s, color, output.extent, PrBasisEtc1sTarget_Pixels, out, output.stride,
                bytes, 0, 3, 0) &&
                prBasisEtc1sDecodeSlice(etc1s, alpha, output.extent, PrBasisEtc1sTarget_Pixels, out, output.stride,
                    bytes, 3, 1, 1);
        case Channels_R:
            return prBasisEtc1sDecodeSlice(etc1s, color, output.extent, PrBasisEtc1sTarget_Pixels, out, output.stride,
                bytes, 0, 1, 0);
        default:
            break;
    }
    // Two values, each stored as a gray slice. They become red and green, with blue zero.
    for (uint32_t y = 0; y < output.extent.y; ++y)
        prMemZero(out + size_t(y) * output.stride, size_t(output.extent.x) * bytes);
    return prBasisEtc1sDecodeSlice(etc1s, color, output.extent, PrBasisEtc1sTarget_Pixels, out, output.stride, bytes, 0, 1, 0) &&
        prBasisEtc1sDecodeSlice(etc1s, alpha, output.extent, PrBasisEtc1sTarget_Pixels, out, output.stride, bytes, 1, 1, 1);
}

// Transcodes the UASTC blocks of an image of `extent` at `source` to blocks of `target`, BC7 or ASTC 4x4, at `out`.
bool transcodeUastc(const uint8_t* source, PrUVec2 extent, PrBasisBlocks target, uint8_t* out) {
    size_t count = size_t((extent.x + 3) / 4) * ((extent.y + 3) / 4);
    bool (*transcode)(const uint8_t*, uint8_t*) =
        target == PrBasisBlocks_Astc4x4 ? prBasisUastcTranscodeAstc : prBasisUastcTranscodeBc7;
    for (size_t i = 0; i < count; ++i) {
        if (!transcode(source + i * 16, out + i * 16))
            return false;
    }
    return true;
}

// Transcodes the ETC1S slices of image `index` to blocks of `target` at `out`. `level` is the bytes of the level that
// holds the slices, and `extent` is the extent of the image. Color becomes BC1 or ETC2 RGB8. Color with alpha becomes
// BC3 or ETC2 RGBA8. One value becomes BC4 or EAC R11. Two values become BC5 or EAC RG11.
bool transcodeEtc1s(const KtxTexture& ktx, PrByteView level, uint32_t index, PrUVec2 extent, PrBasisBlocks target,
        uint8_t* out) {
    PrBasisEtc1sImage image = prBasisEtc1sImage(ktx.etc1s, index);
    bool two = prBasisBlocksBytes(target) == 16;
    if (image.colorSize == 0 || image.colorOffset > level.size || image.colorSize > level.size - image.colorOffset)
        return false;
    if (two && (image.alphaSize == 0 || image.alphaOffset > level.size ||
            image.alphaSize > level.size - image.alphaOffset))
        return false;
    PrByteView color{level.data + image.colorOffset, image.colorSize};
    PrByteView alpha{level.data + (two ? image.alphaOffset : 0), two ? image.alphaSize : 0};
    const PrBasisEtc1s* etc1s = ktx.etc1s;
    uint32_t blockBytes = two ? 16 : 8;
    size_t stride = size_t((extent.x + 3) / 4) * blockBytes;
    // The source of each half of a block. `slice` is the slice, and `blocks` is the block format that the slice
    // becomes. `count` is the number of channels of the slice's color that the blocks hold. If `count` is one,
    // `component` is that channel. A slice of one value is gray. A block of 8 bytes has only the first half.
    struct Half {
        PrByteView slice;
        PrBasisEtc1sTarget blocks;
        uint32_t count;
        uint32_t component;
    };
    Half halves[2] = {};
    switch (target) {
        case PrBasisBlocks_Bc1:
            halves[0] = {color, PrBasisEtc1sTarget_Bc1, 3};
            break;
        case PrBasisBlocks_Bc4:
            halves[0] = {color, PrBasisEtc1sTarget_Bc4, 1};
            break;
        case PrBasisBlocks_Bc3:
            halves[0] = {alpha, PrBasisEtc1sTarget_Bc4, 1, 1};
            halves[1] = {color, PrBasisEtc1sTarget_Bc3Color, 3};
            break;
        case PrBasisBlocks_Bc5:
            halves[0] = {color, PrBasisEtc1sTarget_Bc4, 1};
            halves[1] = {alpha, PrBasisEtc1sTarget_Bc4, 1, 1};
            break;
        case PrBasisBlocks_Etc2Rgb8:
            halves[0] = {color, PrBasisEtc1sTarget_Etc1, 3};
            break;
        case PrBasisBlocks_Etc2Rgba8:
            halves[0] = {alpha, PrBasisEtc1sTarget_EacAlpha, 1};
            halves[1] = {color, PrBasisEtc1sTarget_Etc1, 3};
            break;
        case PrBasisBlocks_EacR11:
            halves[0] = {color, PrBasisEtc1sTarget_EacR11, 1};
            break;
        default:
            halves[0] = {color, PrBasisEtc1sTarget_EacR11, 1};
            halves[1] = {alpha, PrBasisEtc1sTarget_EacR11, 1};
            break;
    }
    for (uint32_t half = 0; half < (two ? 2u : 1u); ++half) {
        if (!prBasisEtc1sDecodeSlice(etc1s, halves[half].slice, extent, halves[half].blocks, out, stride, blockBytes,
                half * 8, halves[half].count, halves[half].component))
            return false;
    }
    return true;
}

// Decodes the blocks at `blocks` of a block texture into pixels in the format of the texture's info. The pixels are red
// alone, red, green and blue, or red, green, blue and alpha.
void decodeBlocks(const KtxTexture& ktx, const uint8_t* blocks, PrPixels output) {
    uint32_t blocksX = (output.extent.x + 3) / 4;
    uint32_t blocksY = (output.extent.y + 3) / 4;
    uint32_t bytes = pixelBytes(ktx);
    uint32_t blockBytes = prBasisBlocksBytes(ktx.info.blocks);
    for (uint32_t blockY = 0; blockY < blocksY; ++blockY) {
        for (uint32_t blockX = 0; blockX < blocksX; ++blockX, blocks += blockBytes) {
            uint8_t texels[64];
            prBasisBlockDecode(ktx.info.blocks, blocks, texels);
            uint32_t width = output.extent.x - blockX * 4 < 4 ? output.extent.x - blockX * 4 : 4;
            uint32_t height = output.extent.y - blockY * 4 < 4 ? output.extent.y - blockY * 4 : 4;
            for (uint32_t y = 0; y < height; ++y) {
                uint8_t* to = output.data + (size_t(blockY) * 4 + y) * output.stride + size_t(blockX) * 4 * bytes;
                for (uint32_t x = 0; x < width; ++x, to += bytes)
                    prMemCopy(to, texels + (y * 4 + x) * 4, bytes);
            }
        }
    }
}

// The output of a read of a level.
enum {
    ReadAs_Pixels,
    ReadAs_Blocks,
    ReadAs_Transcode,
};
typedef uint8_t ReadAs;

// Reads `level` of `frame` of the texture in the form that `as` selects. ReadAs_Pixels decodes into `output`.
// ReadAs_Blocks copies the blocks to `blocks`. ReadAs_Transcode writes blocks of `target` at `blocks`.
PrStatus readLevel(const KtxTexture& ktx, uint32_t frame, uint32_t level, ReadAs as, PrPixels output,
        uint8_t* PR_NULLABLE blocks, PrBasisBlocks target) {
    PR_ASSERT(frame < ktx.info.layerCount * ktx.info.faceCount && level < ktx.info.levelCount);
    PR_ASSERT(ktx.info.payload != PrKtxPayload_Etc1s || ktx.etc1s != nullptr);
    uint8_t* storage = nullptr;
    PrByteView data = {};
    PrStatus status = levelData(ktx, level, &storage, &data);
    if (status != PrStatus_Ok)
        return status;
    // The images of a level follow one another and all have the same size. ETC1S slices are the exception.
    size_t size = size_t(imageSize(ktx, level));
    const uint8_t* from = data.data + size * frame;
    uint32_t index = level * ktx.info.layerCount * ktx.info.faceCount + frame;
    PrUVec2 extent = prKtxLevelExtent(ktx.info.extent, level);
    bool ok = true;
    if (as == ReadAs_Transcode && ktx.info.payload == PrKtxPayload_Etc1s)
        ok = transcodeEtc1s(ktx, data, index, extent, target, blocks);
    else if (as == ReadAs_Transcode)
        ok = transcodeUastc(from, extent, target, blocks);
    else if (as == ReadAs_Blocks)
        prMemCopy(blocks, from, size);
    else if (ktx.info.payload == PrKtxPayload_Blocks)
        decodeBlocks(ktx, from, output);
    else if (ktx.info.payload == PrKtxPayload_Pixels)
        copyPixels(ktx, from, output);
    else if (ktx.info.payload == PrKtxPayload_Uastc)
        ok = decodeUastc(ktx, from, output);
    else
        ok = decodeEtc1s(ktx, data, index, output);
    prMemoryFree(storage);
    return ok ? PrStatus_Ok : PrStatus_InvalidData;
}

// Reads the texture in `bytes` into `*out`, as prKtxTextureCreate describes.
PrStatus readTexture(PrByteView bytes, KtxTexture* out) {
    KtxTexture& texture = *out;
    PrKtxInfo& info = texture.info;
    texture.bytes = bytes;
    if (!prKtxHasSignature(bytes) || bytes.size < kPrKtxHeaderBytes)
        return PrStatus_InvalidData;
    const uint8_t* at = bytes.data;
    uint32_t vkFormat = prMemReadLeU32(at + 12);
    uint32_t depth = prMemReadLeU32(at + 28);
    uint32_t layers = prMemReadLeU32(at + 32);
    uint32_t scheme = prMemReadLeU32(at + 44);
    info.extent = PrUVec2{prMemReadLeU32(at + 20), prMemReadLeU32(at + 24)};
    info.faceCount = prMemReadLeU32(at + 36);
    info.levelCount = prMemReadLeU32(at + 40);
    info.layerCount = layers != 0 ? layers : 1;
    bool cube = info.faceCount == 6;
    if (info.extent.x == 0 || (!cube && info.faceCount != 1) || (cube && info.extent.x != info.extent.y))
        return PrStatus_InvalidData;
    if (depth != 0 || scheme > PrKtxScheme_Zlib)
        return PrStatus_NotSupported;
    info.scheme = PrKtxScheme(scheme);
    // A texture with a height of zero is one row of texels. A texture with a level count of zero has only its first
    // level.
    bool hasHeight = info.extent.y != 0;
    if (!hasHeight)
        info.extent.y = 1;
    if (info.levelCount == 0)
        info.levelCount = 1;
    uint32_t side = info.extent.x > info.extent.y ? info.extent.x : info.extent.y;
    if (info.levelCount > kPrKtxMaxLevels || side >> (info.levelCount - 1) == 0 ||
            bytes.size < kPrKtxHeaderBytes + kLevelIndexBytes * info.levelCount)
        return PrStatus_InvalidData;

    // The descriptor is its size, then one block. The samples of the block follow 24 bytes of data that they share.
    uint64_t dfdOffset = prMemReadLeU32(at + 48);
    uint64_t dfdSize = prMemReadLeU32(at + 52);
    if (!within(dfdOffset, dfdSize, bytes.size) || dfdSize < 28 || prMemReadLeU32(at + dfdOffset) != dfdSize)
        return PrStatus_InvalidData;
    const uint8_t* dfd = at + dfdOffset;
    uint32_t blockSize = prMemReadLeU16(dfd + 10);
    if (blockSize < 24 || blockSize > dfdSize - 4 || (blockSize - 24) % 16 != 0)
        return PrStatus_InvalidData;
    uint32_t samples = (blockSize - 24) / 16;
    uint8_t model = dfd[12];
    uint8_t primaries = dfd[13];
    uint8_t transfer = dfd[14];
    bool premultiplied = (dfd[15] & kFlagPremultiplied) != 0;
    // The channel that each of the first two samples holds.
    uint8_t channel0 = samples >= 1 ? dfd[28 + 3] & 15 : 0;
    uint8_t channel1 = samples >= 2 ? dfd[28 + 16 + 3] & 15 : 0;

    info.sample = PrKtxSample_U8;
    info.channels = 3;
    PrKtxAlpha alpha = premultiplied ? PrKtxAlpha_Premultiplied : PrKtxAlpha_Straight;
    if (vkFormat == 0) {
        if (!hasHeight || samples == 0)
            return PrStatus_InvalidData;
        if (model == kModelEtc1s) {
            if (info.scheme != PrKtxScheme_BasisLz || samples > 2)
                return PrStatus_InvalidData;
            info.payload = PrKtxPayload_Etc1s;
            if (samples == 1 && (channel0 == 0 || channel0 == 3))
                texture.channels = channel0 == 0 ? Channels_Rgb : Channels_R;
            else if (samples == 2 && channel0 == 0 && channel1 == 15)
                texture.channels = Channels_Rgba;
            else if (samples == 2 && channel0 == 3 && channel1 == 4)
                texture.channels = Channels_Rg;
            else
                return PrStatus_InvalidData;
        } else if (model == kModelUastc) {
            if (info.scheme == PrKtxScheme_BasisLz)
                return PrStatus_InvalidData;
            info.payload = PrKtxPayload_Uastc;
            switch (channel0) {
                case 0: texture.channels = Channels_Rgb; break;
                case 3: texture.channels = Channels_Rgba; break;
                case 4: texture.channels = Channels_R; break;
                case 5: texture.channels = Channels_RrrG; break;
                case 6: texture.channels = Channels_Rg; break;
                default: return PrStatus_InvalidData;
            }
        } else {
            return PrStatus_NotSupported;
        }
        if (texture.channels == Channels_R)
            info.channels = 1;
        if (texture.channels == Channels_Rgba) {
            info.channels = 4;
            info.alpha = alpha;
        }
    } else {
        VkFormat found = {};
        if (!findFormat(vkFormat, &found))
            return PrStatus_NotSupported;
        if (info.scheme == PrKtxScheme_BasisLz || (found.blocks != PrBasisBlocks_None && !hasHeight))
            return PrStatus_InvalidData;
        info.payload = found.blocks != PrBasisBlocks_None ? PrKtxPayload_Blocks : PrKtxPayload_Pixels;
        info.blocks = found.blocks;
        texture.pixelChannels = found.channels;
        texture.pixelsBgr = found.bgr;
        info.sample = found.sample;
        if (found.channels == 1)
            info.channels = 1;
        if (found.channels == 4 || found.alpha) {
            info.channels = 4;
            info.alpha = alpha;
        }
    }
    // Linear values of unspecified primaries are data, not color.
    if (transfer == kTransferSrgb)
        info.color = PrKtxColor_Srgb;
    else if (transfer == kTransferLinear && primaries == kPrimariesBt709)
        info.color = PrKtxColor_SrgbLinear;

    uint64_t kvdOffset = prMemReadLeU32(at + 56);
    uint64_t kvdSize = prMemReadLeU32(at + 60);
    uint64_t sgdOffset = prMemReadLeU64(at + 64);
    uint64_t sgdSize = prMemReadLeU64(at + 72);
    if (!within(kvdOffset, kvdSize, bytes.size) || !within(sgdOffset, sgdSize, bytes.size) ||
            !readOrientation(PrByteView{at + kvdOffset, size_t(kvdSize)}, &info))
        return PrStatus_InvalidData;
    texture.global = PrByteView{at + sgdOffset, size_t(sgdSize)};
    if ((info.scheme == PrKtxScheme_BasisLz) != (sgdSize != 0))
        return PrStatus_InvalidData;

    bool truncated = false;
    for (uint32_t level = 0; level < info.levelCount; ++level) {
        const uint8_t* entry = at + kPrKtxHeaderBytes + kLevelIndexBytes * level;
        Level& stored = texture.levels[level];
        stored = Level{prMemReadLeU64(entry), prMemReadLeU64(entry + 8), prMemReadLeU64(entry + 16)};
        // The level data follows everything else in the file. If the bytes end before a level does, they still contain
        // the header and the other parts.
        if (stored.offset > UINT64_MAX - stored.size)
            return PrStatus_InvalidData;
        truncated = truncated || !within(stored.offset, stored.size, bytes.size);
        if (info.payload == PrKtxPayload_Etc1s)
            continue;
        // A level is the images of the faces of each layer, and all the images have the same size.
        uint64_t expected = imageSize(texture, level) * info.layerCount * info.faceCount;
        if ((info.scheme == PrKtxScheme_None ? stored.size : stored.inflatedSize) != expected)
            return PrStatus_InvalidData;
    }
    if (info.scheme == PrKtxScheme_Zstd || info.scheme == PrKtxScheme_Zlib)
        info.inflatedBytes = texture.levels[0].inflatedSize;
    info.blocksDecode = info.payload == PrKtxPayload_Blocks && prBasisBlocksDecodes(info.blocks);
    info.transcodes = transcodesOf(texture);
    return truncated ? PrStatus_Truncated : PrStatus_Ok;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrKtxTexture : KtxTexture {};

bool prKtxHasSignature(PrByteView bytes) {
    return bytes.size >= sizeof(kIdentifier) && prMemEquals(bytes.data, kIdentifier, sizeof(kIdentifier));
}

uint64_t prKtxInfoBytes(PrByteView start) {
    // Bytes that match the start of the identifier may begin a file.
    size_t begun = start.size < sizeof(kIdentifier) ? start.size : sizeof(kIdentifier);
    if (start.size < kPrKtxHeaderBytes)
        return begun == 0 || prMemEquals(start.data, kIdentifier, begun) ? kPrKtxHeaderBytes : 0;
    if (!prKtxHasSignature(start))
        return 0;
    const uint8_t* at = start.data;
    uint64_t levels = prMemReadLeU32(at + 40);
    levels = levels == 0 ? 1 : levels < kPrKtxMaxLevels ? levels : kPrKtxMaxLevels;
    uint64_t end = kPrKtxHeaderBytes + kLevelIndexBytes * levels;
    // The ends of the descriptor, the key and value data, and the global data. Each is stored as an offset and a size.
    const uint64_t ends[3] = {
        uint64_t(prMemReadLeU32(at + 48)) + prMemReadLeU32(at + 52),
        uint64_t(prMemReadLeU32(at + 56)) + prMemReadLeU32(at + 60),
        prMemReadLeU64(at + 64) + prMemReadLeU64(at + 72),
    };
    for (uint64_t part : ends)
        end = part > end ? part : end;
    return end;
}

PrUVec2 prKtxLevelExtent(PrUVec2 extent, uint32_t level) {
    PrUVec2 size{level < 32 ? extent.x >> level : 0, level < 32 ? extent.y >> level : 0};
    return PrUVec2{size.x != 0 ? size.x : 1, size.y != 0 ? size.y : 1};
}

PrStatus prKtxTextureCreate(PrByteView bytes, const PrCompressCodecSet* PR_NULLABLE codec,
        PrKtxTexture* PR_NULLABLE* PR_NONNULL out) {
    PrKtxTexture* texture = allocZeroed<PrKtxTexture>();
    texture->codec = codec;
    PrStatus status = readTexture(bytes, texture);
    if (status != PrStatus_Ok && status != PrStatus_Truncated) {
        prMemoryFree(texture);
        texture = nullptr;
    }
    *out = texture;
    return status;
}

void prKtxTextureDestroy(PrKtxTexture* PR_NULLABLE texture) {
    if (texture == nullptr)
        return;
    prBasisEtc1sDestroy(texture->etc1s);
    prMemoryFree(texture);
}

const PrKtxInfo* prKtxTextureInfo(const PrKtxTexture* texture) {
    return &texture->info;
}

PrStatus prKtxTexturePrepare(PrKtxTexture* texture) {
    if (texture->info.payload != PrKtxPayload_Etc1s || texture->etc1s != nullptr)
        return PrStatus_Ok;
    const PrKtxInfo& info = texture->info;
    uint64_t images = uint64_t(info.layerCount) * info.faceCount * info.levelCount;
    return prBasisEtc1sCreate(texture->global, uint32_t(images), &texture->etc1s);
}

uint64_t prKtxTextureSharedBytes(const PrKtxTexture* texture) {
    return texture->etc1s != nullptr ? prBasisEtc1sWorkingBytes(texture->etc1s) : 0;
}

size_t prKtxTextureBlocksSize(const PrKtxTexture* texture, uint32_t level) {
    PR_ASSERT(level < texture->info.levelCount);
    return prBasisBlocksSize(prKtxLevelExtent(texture->info.extent, level), texture->info.blocks);
}

PrStatus prKtxTextureRead(const PrKtxTexture* texture, uint32_t frame, uint32_t level, PrPixels output) {
    PR_ASSERT(output.extent == prKtxLevelExtent(texture->info.extent, level));
    if (texture->info.payload == PrKtxPayload_Blocks && !texture->info.blocksDecode)
        return PrStatus_NotSupported;
    return readLevel(*texture, frame, level, ReadAs_Pixels, output, nullptr, PrBasisBlocks_None);
}

PrStatus prKtxTextureReadBlocks(const PrKtxTexture* texture, uint32_t frame, uint32_t level, uint8_t* out, size_t size) {
    if (texture->info.payload != PrKtxPayload_Blocks)
        return PrStatus_NotSupported;
    PR_ASSERT(size == prKtxTextureBlocksSize(texture, level));
    return readLevel(*texture, frame, level, ReadAs_Blocks, PrPixels{}, out, PrBasisBlocks_None);
}

PrStatus prKtxTextureTranscode(const PrKtxTexture* texture, uint32_t frame, uint32_t level, PrBasisBlocks target,
        uint8_t* out, size_t size) {
    if (target >= PrBasisBlocks_Count || (texture->info.transcodes >> target & 1) == 0)
        return PrStatus_NotSupported;
    PR_ASSERT(size == prBasisBlocksSize(prKtxLevelExtent(texture->info.extent, level), target));
    return readLevel(*texture, frame, level, ReadAs_Transcode, PrPixels{}, out, target);
}
