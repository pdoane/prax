// KTX 2.0 files as texture sources. A source produces the blocks that a file stores, the pixels of its levels, or the
// blocks that a Basis Universal format transcodes to.

#include "pr/ktx_texture_source.h"

#include "pr/alloc.h"
#include "pr/ktx.h"
#include "pr/memory.h"
#include "pr/texture_backend.h"

namespace pr_impl {

namespace {

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
        desc.format = ktx.blocks;
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
        if (info.payload == PrKtxPayload_Blocks && format == info.blocks)
            return prKtxTextureReadBlocks(texture, layer, level, out, size);
        return prKtxTextureTranscode(texture, layer, level, format, out, size);
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

// Returns true if a source of `ktx` produces `format`: the blocks that the file stores or transcodes to, and the pixels
// that it decodes to, each in the form of the texture's transfer function.
bool produces(const PrKtxInfo& ktx, PrGpuFormat format) {
    if (ktx.payload == PrKtxPayload_Blocks && format == formOf(ktx, ktx.blocks))
        return true;
    for (uint32_t i = 0; i < ktx.transcodeCount; ++i) {
        if (format == formOf(ktx, ktx.transcodes[i]))
            return true;
    }
    if (!decodes(ktx))
        return false;
    return format == formOf(ktx, PrGpuFormat_R8) || format == formOf(ktx, PrGpuFormat_Rgba8) ||
        format == PrGpuFormat_Rgba16F || format == PrGpuFormat_Rgba32F;
}

PrStatus sourceReformat(void* PR_NULLABLE context, PrGpuFormat format, const PrTextureSource* PR_NULLABLE* PR_NONNULL out) {
    KtxSource source = *static_cast<const KtxSource*>(context);
    if (!produces(source.ktx, format))
        return PrStatus_NotSupported;
    source.desc.format = format;
    *out = sourceOf(source);
    return PrStatus_Ok;
}

// The block formats that Basis Universal textures transcode to, in order of preference. A need of one channel takes the
// formats of one channel first.
constexpr PrGpuFormat kTranscodeTargets[] = {
    PrGpuFormat_Astc4x4, PrGpuFormat_Bc7, PrGpuFormat_EacR11, PrGpuFormat_Bc4, PrGpuFormat_EacRg11, PrGpuFormat_Bc5,
    PrGpuFormat_Etc2Rgba8, PrGpuFormat_Bc3, PrGpuFormat_Etc2Rgb8, PrGpuFormat_Bc1,
};

// Returns true if `format` holds one channel.
bool isSingle(PrGpuFormat format) {
    return format == PrGpuFormat_Bc4 || format == PrGpuFormat_EacR11;
}

// Writes to `*out` the source of `held` in `format`, or `own` if that is its own format.
PrStatus answerIn(const KtxSource& held, const PrTextureSource* own, PrGpuFormat format,
        const PrTextureSource* PR_NULLABLE* PR_NONNULL out) {
    if (format == held.desc.format) {
        prTextureSourceRetain(own);
        *out = own;
        return PrStatus_Ok;
    }
    KtxSource source = held;
    source.desc.format = format;
    *out = sourceOf(source);
    return PrStatus_Ok;
}

// Answers a need with the stored blocks if the need takes them, or else with the blocks of the first target of
// kTranscodeTargets that the texture transcodes to and the need takes, the targets of one channel first for a need of
// one channel. A need of pixels takes pixels if the texture decodes, and otherwise its stored blocks: the transcodes
// serve a texture of several levels, whose levels the device need not make. The pixels are those of the source's own
// format if it is pixels, or else Rgba8 in the form of the texture's transfer function, or R8 for a need of the red
// channel of data alone.
PrStatus sourceAnswer(void* PR_NULLABLE context, const PrTextureSource* own, const PrTextureNeed* need,
        const PrTextureSource* PR_NULLABLE* PR_NONNULL out) {
    const KtxSource& held = *static_cast<const KtxSource*>(context);
    const PrKtxInfo& ktx = held.ktx;
    const PrUVec2 extent = held.desc.extent;
    const PrTextureAlpha alpha = held.desc.alpha;
    const bool pixels = need->pixels && decodes(ktx);
    if (!pixels) {
        if (ktx.payload == PrKtxPayload_Blocks && prTextureNeedTakesBlocks(need, formOf(ktx, ktx.blocks), alpha, extent))
            return answerIn(held, own, formOf(ktx, ktx.blocks), out);
        const bool single = prTextureNeedSingleChannel(need) < 4;
        for (uint32_t pass = 0; !need->pixels && pass < 2; ++pass) {
            for (PrGpuFormat target : kTranscodeTargets) {
                // The first pass takes the targets of one channel for a need of one channel, and the second the rest.
                if ((pass == 0) != (single && isSingle(target)))
                    continue;
                const PrGpuFormat format = formOf(ktx, target);
                if (produces(ktx, format) && format != formOf(ktx, ktx.blocks) &&
                        prTextureNeedTakesBlocks(need, format, alpha, extent))
                    return answerIn(held, own, format, out);
            }
        }
    }
    if (!decodes(ktx))
        return PrStatus_NotSupported;
    if (prTextureNeedSingleChannel(need) == 0 && formOf(ktx, PrGpuFormat_R8) == PrGpuFormat_R8)
        return answerIn(held, own, PrGpuFormat_R8, out);
    if (!prGpuFormatIsCompressed(held.desc.format))
        return answerIn(held, own, held.desc.format, out);
    return answerIn(held, own, formOf(ktx, PrGpuFormat_Rgba8), out);
}

// Returns a texture source over a copy of `source`. The copy retains the file's bytes. The format of `source` is one
// that the file produces.
const PrTextureSource* sourceOf(const KtxSource& source) {
    KtxSource* held = allocZeroed<KtxSource>();
    *held = source;
    prBlobSourceRetain(source.bytes);
    PrTextureSourceDesc desc{
        .texture = source.desc,
        .read = sourceRead,
        .reformat = sourceReformat,
        .answer = sourceAnswer,
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
    KtxSource source{.bytes = bytes, .codec = codec, .ktx = ktx, .desc = descOf(ktx)};
    if (!prTextureLimitsAdmit(&limits, &source.desc))
        return PrStatus_Limit;
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

// A KTX file is opened once every byte has arrived.
PrStatus formatCreate(const PrTextureFormat* format, const PrBlobSource* bytes, const PrBlob* blob,
        const PrTextureLimits* limits, bool partial, const PrTextureSource* PR_NULLABLE* PR_NONNULL out) {
    if (partial)
        return PrStatus_Truncated;
    return sourceCreate(bytes, blob, static_cast<const KtxFormat*>(format)->codec, *limits, out);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrTextureFormat* prKtxTextureFormatCreate(const PrCompressCodecSet* PR_NULLABLE codec) {
    KtxFormat* format = allocZeroed<KtxFormat>();
    format->destroyFn = [](PrTextureFormat* made) { prMemoryFree(static_cast<KtxFormat*>(made)); };
    format->supportsTypeFn = formatSupportsType;
    format->createFn = formatCreate;
    format->codec = codec;
    return format;
}
