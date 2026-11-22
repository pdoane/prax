// KTX 2.0 files as texture sources. A source produces the blocks that a file stores, the pixels of its levels, or the
// blocks that a Basis Universal format transcodes to.

#include "pr/ktx_texture_source.h"

#include "pr/alloc.h"
#include "pr/ktx.h"
#include "pr/memory.h"
#include "pr/texture_backend.h"

namespace pr_impl {

namespace {

// The PrGpuFormat of each block format, indexed by PrBasisBlocks. The table ends before the ASTC formats.
constexpr PrGpuFormat kBlockFormats[] = {
    PrGpuFormat_None, PrGpuFormat_Bc1, PrGpuFormat_Bc2, PrGpuFormat_Bc3, PrGpuFormat_Bc4, PrGpuFormat_Bc4Snorm,
    PrGpuFormat_Bc5, PrGpuFormat_Bc5Snorm, PrGpuFormat_Bc6HUfloat, PrGpuFormat_Bc6HSfloat, PrGpuFormat_Bc7,
    PrGpuFormat_Etc2Rgb8, PrGpuFormat_Etc2Rgb8A1, PrGpuFormat_Etc2Rgba8, PrGpuFormat_EacR11, PrGpuFormat_EacR11Snorm,
    PrGpuFormat_EacRg11, PrGpuFormat_EacRg11Snorm,
};

// Returns the PrGpuFormat that holds blocks of `blocks` as they are stored.
PrGpuFormat blockFormat(PrBasisBlocks blocks) {
    if (blocks < PrBasisBlocks_Astc4x4)
        return kBlockFormats[blocks];
    // PrBasisBlocks has two profiles for each ASTC block size. PrGpuFormat has three formats for each block size, and
    // the format of the HDR profile is the last of the three.
    uint32_t index = uint32_t(blocks) - PrBasisBlocks_Astc4x4;
    return PrGpuFormat(PrGpuFormat_Astc4x4 + index / 2 * kPrGpuAstcFormatsPerBlockSize + (index % 2 != 0 ? 2 : 0));
}

// Returns the block format that the PrGpuFormat `format` holds, or None.
PrBasisBlocks blocksOf(PrGpuFormat format) {
    for (uint32_t blocks = PrBasisBlocks_Bc1; blocks < PrBasisBlocks_Count; ++blocks) {
        if (blockFormat(PrBasisBlocks(blocks)) == format)
            return PrBasisBlocks(blocks);
    }
    return PrBasisBlocks_None;
}

// The state of a source that reads a texture from a file. It holds the file's bytes, the codec, the texture's
// description from the file, and the description of the texture that the source produces.
struct KtxSource {
    const PrBlobSource* bytes;
    const PrCompressCodecSet* PR_NULLABLE codec;
    PrKtxInfo ktx;
    PrTextureDesc desc;
};

// A read in progress: the source, and the PrTextureRead that it completes.
struct KtxRead {
    const KtxSource* source;
    PrTextureRead* read;
};

// Returns true if a texture of `ktx` decodes to pixels.
bool decodes(const PrKtxInfo& ktx) {
    return ktx.payload != PrKtxPayload_Blocks || ktx.blocksDecode;
}

// Returns the form of `format` that holds the values of a texture of `ktx`. If the texture's descriptor states the sRGB
// transfer function, that is the Srgb form of `format`, if it has one.
PrGpuFormat formOf(const PrKtxInfo& ktx, PrGpuFormat format) {
    return ktx.color == PrKtxColor_Srgb ? prGpuFormatSrgb(format) : format;
}

// Returns the description of a texture of `ktx` in its native form. A block texture keeps its blocks. Any other texture
// is the pixels that it decodes to.
PrTextureDesc descOf(const PrKtxInfo& ktx) {
    PrTextureDesc desc{
        .kind = PrGpuTextureKind_2d,
        .extent = ktx.extent,
        .depth = 1,
        .layerCount = ktx.layerCount * ktx.faceCount,
        .levelCount = ktx.levelCount < uint32_t(kPrTextureMaxLevels) ? ktx.levelCount : uint32_t(kPrTextureMaxLevels),
    };
    if (ktx.faceCount == 6)
        desc.kind = ktx.layerCount > 1 ? PrGpuTextureKind_CubeArray : PrGpuTextureKind_Cube;
    else if (ktx.layerCount > 1)
        desc.kind = PrGpuTextureKind_2dArray;
    if (ktx.payload == PrKtxPayload_Blocks)
        desc.format = blockFormat(ktx.blocks);
    else if (ktx.payload == PrKtxPayload_Pixels && (ktx.sample == PrKtxSample_F16 || ktx.sample == PrKtxSample_F32))
        desc.format = PrGpuFormat_Rgba32F;
    else
        desc.format = PrGpuFormat_Rgba8;
    constexpr PrTextureAlpha kAlphas[] = {PrTextureAlpha_None, PrTextureAlpha_Straight, PrTextureAlpha_Premultiplied};
    desc.alpha = kAlphas[ktx.alpha];
    desc.format = formOf(ktx, desc.format);
    if (ktx.fromRight)
        desc.orientation |= PrTextureOrientationFlag_FlipX;
    if (ktx.fromBottom)
        desc.orientation |= PrTextureOrientationFlag_FlipY;
    return desc;
}

// Reads `bytes` into a PrKtxTexture at `*out`. A texture that ends before its levels do is still Ok. Its missing levels
// fail when they are read.
PrStatus ktxOpen(PrByteView bytes, const PrCompressCodecSet* PR_NULLABLE codec, PrKtxTexture* PR_NULLABLE* PR_NONNULL out) {
    PrStatus status = prKtxTextureCreate(bytes, codec, out);
    return status == PrStatus_Truncated ? PrStatus_Ok : status;
}

// Writes level `level` of frame `layer` of `texture` at `out`, in the source's format. `out` holds `size` bytes.
PrStatus readLevel(const KtxSource& source, const PrKtxTexture* texture, uint32_t level, uint32_t layer, uint8_t* out,
        size_t size) {
    const PrKtxInfo& info = source.ktx;
    PrGpuFormat format = prGpuFormatPlain(source.desc.format);
    if (prGpuFormatIsCompressed(format)) {
        if (info.payload == PrKtxPayload_Blocks && format == blockFormat(info.blocks))
            return prKtxTextureReadBlocks(texture, layer, level, out, size);
        return prKtxTextureTranscode(texture, layer, level, blocksOf(format), out, size);
    }
    constexpr PrTextureSample kSamples[] = {
        PrTextureSample_U8, PrTextureSample_U16, PrTextureSample_F16, PrTextureSample_F32,
    };
    constexpr PrTextureChannels kChannels[] = {
        PrTextureChannels_Gray, PrTextureChannels_Rg, PrTextureChannels_Rgb, PrTextureChannels_Rgba,
    };
    PrTextureSample sample = kSamples[info.sample];
    PrTextureChannels channels = kChannels[info.channels - 1];
    PrUVec2 extent = prKtxLevelExtent(info.extent, level);
    size_t stride = size_t(extent.x) * prTexturePixelSize(channels, sample);
    uint8_t* stored = static_cast<uint8_t*>(prMemoryAlloc(stride * extent.y));
    PrStatus status = prKtxTextureRead(texture, layer, level, PrPixels{stored, stride, extent});
    if (status == PrStatus_Ok) {
        size_t texelBytes = prGpuFormatBlock(format).bytes;
        prTexturePixelsUnpack(PrPixelView{stored, stride, extent}, channels, sample, format,
            PrPixels{out, size_t(extent.x) * texelBytes, extent});
    }
    prMemoryFree(stored);
    return status;
}

// Reads the levels and layers that `read` asks for from `bytes`. `bytes` is the file's bytes from a second read of the
// blob.
PrStatus readTexture(const KtxSource& source, PrByteView bytes, PrTextureRead* read) {
    PrKtxTexture* texture = nullptr;
    PrStatus status = ktxOpen(bytes, source.codec, &texture);
    // The second read must return the texture that the source describes.
    if (status == PrStatus_Ok) {
        const PrKtxInfo& again = *prKtxTextureInfo(texture);
        bool same = again.extent == source.ktx.extent && again.levelCount == source.ktx.levelCount &&
            again.layerCount == source.ktx.layerCount && again.faceCount == source.ktx.faceCount &&
            again.payload == source.ktx.payload && again.blocks == source.ktx.blocks;
        status = same ? prKtxTexturePrepare(texture) : PrStatus_InvalidData;
    }
    const PrTextureReadDesc& asked = *prTextureReadInfo(read);
    for (uint32_t level = asked.firstLevel; status == PrStatus_Ok && level < asked.firstLevel + asked.levelCount; ++level) {
        size_t size = prTextureLevelSize(&source.desc, level);
        for (uint32_t layer = asked.firstLayer; status == PrStatus_Ok && layer < asked.firstLayer + asked.layerCount; ++layer) {
            uint8_t* out = prTextureReadStorage(read) + prTextureReadOffset(&source.desc, &asked, level, layer);
            status = readLevel(source, texture, level, layer, out, size);
        }
    }
    prKtxTextureDestroy(texture);
    return status == PrStatus_Truncated ? PrStatus_InvalidData : status;
}

void sourceRead(void* PR_NULLABLE context, PrTextureRead* read) {
    const KtxSource* source = static_cast<const KtxSource*>(context);
    KtxRead* held = static_cast<KtxRead*>(prMemoryAlloc(sizeof(KtxRead)));
    *held = KtxRead{source, read};
    PrBlobReadDesc bytes = prBlobReadDescDefault();
    bytes.done = [](void* PR_NULLABLE context, PrBlobRead* blob) {
        KtxRead held = *static_cast<KtxRead*>(context);
        prMemoryFree(context);
        PrStatus status = prBlobReadStatus(blob);
        if (status == PrStatus_Ok)
            status = readTexture(*held.source, prBlobBytes(prBlobReadBlob(blob)), held.read);
        prBlobReadDestroy(blob);
        prTextureReadComplete(held.read, status);
    };
    bytes.context = held;
    prBlobReadCreate(source->bytes, &bytes);
}

void sourceRelease(void* PR_NULLABLE context) {
    KtxSource* source = static_cast<KtxSource*>(context);
    prBlobSourceRelease(source->bytes);
    prMemoryFree(source);
}

const PrTextureSource* sourceOf(const KtxSource& source);

PrStatus sourceReformat(void* PR_NULLABLE context, PrGpuFormat format, const PrTextureSource* PR_NULLABLE* PR_NONNULL out) {
    KtxSource source = *static_cast<const KtxSource*>(context);
    source.desc.format = format;
    *out = sourceOf(source);
    return PrStatus_Ok;
}

// Returns a texture source over a copy of `source`. The copy retains the file's bytes. The source's formats are the
// blocks that the file stores or transcodes to, and the pixels that it decodes to. The format of `source` is one of
// them.
const PrTextureSource* sourceOf(const KtxSource& source) {
    KtxSource* held = allocZeroed<KtxSource>();
    *held = source;
    prBlobSourceRetain(source.bytes);
    const PrKtxInfo& ktx = source.ktx;
    PrGpuFormat formats[kPrTextureSourceMaxFormats];
    uint32_t count = 0;
    auto add = [&](PrGpuFormat plain) {
        PrGpuFormat format = formOf(ktx, plain);
        if (format != source.desc.format && count < kPrTextureSourceMaxFormats)
            formats[count++] = format;
    };
    if (ktx.payload == PrKtxPayload_Blocks)
        add(blockFormat(ktx.blocks));
    for (uint32_t blocks = PrBasisBlocks_Bc1; blocks < PrBasisBlocks_Count; ++blocks) {
        if ((ktx.transcodes >> blocks & 1) != 0)
            add(blockFormat(PrBasisBlocks(blocks)));
    }
    if (decodes(ktx)) {
        add(PrGpuFormat_R8);
        add(PrGpuFormat_Rgba8);
        add(PrGpuFormat_Rgba16F);
        add(PrGpuFormat_Rgba32F);
    }
    PrTextureSourceDesc desc{
        .texture = source.desc,
        .read = sourceRead,
        .reformat = sourceReformat,
        .formats = formats,
        .formatCount = count,
        .release = sourceRelease,
        .context = held,
    };
    return prTextureSourceCreate(&desc);
}

// Creates a source over `bytes`, described by `blob`.
PrStatus sourceCreate(const PrBlobSource* bytes, const PrBlob* blob, const PrCompressCodecSet* PR_NULLABLE codec,
        const PrTextureLimits& limits, const PrTextureSource* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    PrKtxTexture* texture = nullptr;
    PrStatus status = ktxOpen(prBlobBytes(blob), codec, &texture);
    if (status != PrStatus_Ok)
        return status;
    PrKtxInfo ktx = *prKtxTextureInfo(texture);
    prKtxTextureDestroy(texture);
    uint64_t pixels = uint64_t(ktx.extent.x) * ktx.extent.y;
    if (pixels > limits.pixels || ktx.inflatedBytes > limits.workingBytes)
        return PrStatus_Limit;
    KtxSource source{.bytes = bytes, .codec = codec, .ktx = ktx, .desc = descOf(ktx)};
    *out = sourceOf(source);
    return PrStatus_Ok;
}

// The KTX texture format, with the codec that its sources inflate through.
struct KtxFormat : PrTextureFormat {
    const PrCompressCodecSet* PR_NULLABLE codec;
};

bool formatSupportsType(const PrTextureFormat*, PrStringView type) {
    return type == "image/ktx2";
}

PrStatus formatCreate(const PrTextureFormat* format, const PrBlobSource* bytes, const PrBlob* blob,
        const PrTextureLimits* limits, const PrTextureSource* PR_NULLABLE* PR_NONNULL out) {
    return sourceCreate(bytes, blob, static_cast<const KtxFormat*>(format)->codec, *limits, out);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrStatus prKtxTextureSourceOpen(const PrBlobSource* bytes, const PrCompressCodecSet* PR_NULLABLE codec,
        const PrTextureLimits* PR_NULLABLE limits, const PrTextureSource* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    PrBlob* blob = nullptr;
    PrStatus status = prBlobSourceRead(bytes, prBlobRangeWhole(), &blob);
    if (status != PrStatus_Ok)
        return status;
    PrTextureLimits bounds = limits != nullptr ? *limits : prTextureLimitsDefault();
    status = sourceCreate(bytes, blob, codec, bounds, out);
    prBlobRelease(blob);
    return status;
}

PrTextureFormat* prKtxTextureFormatCreate(const PrCompressCodecSet* PR_NULLABLE codec) {
    KtxFormat* format = allocZeroed<KtxFormat>();
    format->destroyFn = [](PrTextureFormat* made) { prMemoryFree(static_cast<KtxFormat*>(made)); };
    format->supportsTypeFn = formatSupportsType;
    format->createFn = formatCreate;
    format->codec = codec;
    return format;
}
