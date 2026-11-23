// DDS files as texture sources. A source produces the texels that a file stores, or the pixels of its levels.

#include "pr/dds_texture_source.h"

#include "pr/alloc.h"
#include "pr/dds.h"
#include "pr/memory.h"
#include "pr/texture_backend.h"

namespace pr_impl {

namespace {

// A DXGI format of pixels and the PrGpuFormat that holds them as stored. The DXGI formats of the sRGB transfer function
// take the Srgb form of their format.
struct GpuPixels {
    uint32_t dxgi;
    PrGpuFormat format;
};

constexpr GpuPixels kPixelFormats[] = {
    {2, PrGpuFormat_Rgba32F}, {10, PrGpuFormat_Rgba16F}, {11, PrGpuFormat_Rgba16}, {13, PrGpuFormat_Rgba16Snorm},
    {16, PrGpuFormat_Rg32F}, {24, PrGpuFormat_Rgb10A2}, {26, PrGpuFormat_Rg11B10F}, {27, PrGpuFormat_Rgba8},
    {28, PrGpuFormat_Rgba8}, {29, PrGpuFormat_Rgba8}, {31, PrGpuFormat_Rgba8Snorm}, {34, PrGpuFormat_Rg16F},
    {35, PrGpuFormat_Rg16}, {37, PrGpuFormat_Rg16Snorm}, {41, PrGpuFormat_R32F}, {49, PrGpuFormat_Rg8},
    {51, PrGpuFormat_Rg8Snorm}, {54, PrGpuFormat_R16F}, {56, PrGpuFormat_R16}, {58, PrGpuFormat_R16Snorm},
    {61, PrGpuFormat_R8}, {63, PrGpuFormat_R8Snorm}, {65, PrGpuFormat_R8Alpha}, {67, PrGpuFormat_Rgb9E5F},
    {85, PrGpuFormat_B5G6R5}, {86, PrGpuFormat_Bgr5A1}, {87, PrGpuFormat_Bgra8}, {90, PrGpuFormat_Bgra8},
    {91, PrGpuFormat_Bgra8}, {115, PrGpuFormat_Bgra4},
};

// Returns the format that `dds` stores its texels in, or None if lib/gpu has no such format.
PrGpuFormat storedFormat(const PrDdsInfo& dds) {
    PrGpuFormat format = PrGpuFormat_None;
    if (dds.blocks != PrGpuFormat_None) {
        format = dds.blocks;
    } else {
        for (const GpuPixels& pixels : kPixelFormats) {
            if (pixels.dxgi == dds.dxgiFormat)
                format = pixels.format;
        }
    }
    if (format == PrGpuFormat_None)
        return format;
    if (dds.swizzle == PrDdsSwizzle_Luminance)
        format = prGpuFormatSwizzled(format, PrGpuSwizzle_Luminance);
    else if (dds.swizzle == PrDdsSwizzle_LuminanceAlpha)
        format = prGpuFormatSwizzled(format, PrGpuSwizzle_LuminanceAlpha);
    return dds.srgb ? prGpuFormatSrgb(format) : format;
}

// Returns the form of `format` that holds the values of a texture of `dds`: its Srgb form for a format of the sRGB
// transfer function, if it has one.
PrGpuFormat formOf(const PrDdsInfo& dds, PrGpuFormat format) {
    return dds.srgb ? prGpuFormatSrgb(format) : format;
}

// The state of a source that reads a texture from a file: the file's bytes, the texture's description from the file,
// and the description of the texture that the source produces.
struct DdsSource {
    const PrBlobSource* bytes;
    PrDdsInfo dds;
    PrTextureDesc desc;
};

struct DdsRead {
    const DdsSource* source;
    PrTextureRead* read;
};

// Returns the description of a texture of `dds` in its native form: its texels as stored if lib/gpu has their format,
// and otherwise the pixels that they decode to.
PrTextureDesc descOf(const PrDdsInfo& dds) {
    PrTextureDesc desc{
        .kind = PrGpuTextureKind_2d,
        .extent = dds.extent,
        .depth = dds.depth,
        .layerCount = dds.layerCount * dds.faceCount,
        .levelCount = dds.levelCount < uint32_t(kPrTextureMaxLevels) ? dds.levelCount : uint32_t(kPrTextureMaxLevels),
    };
    if (dds.depth > 1)
        desc.kind = PrGpuTextureKind_3d;
    else if (dds.faceCount == 6)
        desc.kind = dds.layerCount > 1 ? PrGpuTextureKind_CubeArray : PrGpuTextureKind_Cube;
    else if (dds.layerCount > 1)
        desc.kind = PrGpuTextureKind_2dArray;
    desc.format = storedFormat(dds);
    if (desc.format == PrGpuFormat_None)
        desc.format = formOf(dds, dds.sample == PrDdsSample_U8 ? PrGpuFormat_Rgba8 : PrGpuFormat_Rgba32F);
    constexpr PrTextureAlpha kAlphas[] = {PrTextureAlpha_None, PrTextureAlpha_Straight, PrTextureAlpha_Premultiplied};
    desc.alpha = kAlphas[dds.alpha];
    return desc;
}

// Writes level `level` of frame `layer` of `texture` at `out`, in the source's format. `out` holds `size` bytes.
PrStatus readLevel(const DdsSource& source, const PrDdsTexture* texture, uint32_t level, uint32_t layer, uint8_t* out,
        size_t size) {
    const PrDdsInfo& dds = source.dds;
    const PrGpuFormat format = prGpuFormatPlain(source.desc.format);
    const PrGpuFormat stored = storedFormat(dds);
    if (stored != PrGpuFormat_None && format == prGpuFormatPlain(stored))
        return prDdsTextureReadStored(texture, layer, level, out, size);
    constexpr PrTextureSample kSamples[] = {
        PrTextureSample_U8, PrTextureSample_U16, PrTextureSample_F16, PrTextureSample_F32,
    };
    constexpr PrTextureChannels kChannels[] = {
        PrTextureChannels_Gray, PrTextureChannels_GrayAlpha, PrTextureChannels_Rgb, PrTextureChannels_Rgba,
    };
    const PrTextureSample sample = kSamples[dds.sample];
    const PrTextureChannels channels = kChannels[dds.channels - 1];
    const PrUVec2 level2d = prDdsLevelExtent(dds.extent, level);
    const uint32_t depth = level < 32 && dds.depth >> level != 0 ? dds.depth >> level : 1;
    const PrUVec2 extent{level2d.x, level2d.y * depth};
    const size_t stride = size_t(extent.x) * prTexturePixelSize(channels, sample);
    uint8_t* decoded = static_cast<uint8_t*>(prMemoryAlloc(stride * extent.y));
    PrStatus status = prDdsTextureRead(texture, layer, level, PrPixels{decoded, stride, extent});
    if (status == PrStatus_Ok) {
        const size_t texelBytes = prGpuFormatBlock(format).bytes;
        prTexturePixelsUnpack(PrPixelView{decoded, stride, extent}, channels, sample, format,
            PrPixels{out, size_t(extent.x) * texelBytes, extent});
    }
    prMemoryFree(decoded);
    return status;
}

// Reads the levels and layers that `read` asks for from `bytes`, the file's bytes from a second read of the blob.
PrStatus readTexture(const DdsSource& source, PrByteView bytes, PrTextureRead* read) {
    PrDdsTexture* texture = nullptr;
    PrStatus status = prDdsTextureCreate(bytes, &texture);
    if (status == PrStatus_Truncated)
        status = PrStatus_Ok;
    // The second read must return the texture that the source describes.
    if (status == PrStatus_Ok) {
        const PrDdsInfo& again = *prDdsTextureInfo(texture);
        const bool same = again.extent == source.dds.extent && again.depth == source.dds.depth &&
            again.levelCount == source.dds.levelCount && again.layerCount == source.dds.layerCount &&
            again.faceCount == source.dds.faceCount && again.dxgiFormat == source.dds.dxgiFormat &&
            again.blocks == source.dds.blocks;
        status = same ? PrStatus_Ok : PrStatus_InvalidData;
    }
    const PrTextureReadDesc& asked = *prTextureReadInfo(read);
    for (uint32_t level = asked.firstLevel; status == PrStatus_Ok && level < asked.firstLevel + asked.levelCount; ++level) {
        const size_t size = prTextureLevelSize(&source.desc, level);
        for (uint32_t layer = asked.firstLayer; status == PrStatus_Ok && layer < asked.firstLayer + asked.layerCount; ++layer) {
            uint8_t* out = prTextureReadStorage(read) + prTextureReadOffset(&source.desc, &asked, level, layer);
            status = readLevel(source, texture, level, layer, out, size);
        }
    }
    prDdsTextureDestroy(texture);
    return status == PrStatus_Truncated ? PrStatus_InvalidData : status;
}

void sourceRead(void* PR_NULLABLE context, PrTextureRead* read) {
    const DdsSource* source = static_cast<const DdsSource*>(context);
    DdsRead* held = static_cast<DdsRead*>(prMemoryAlloc(sizeof(DdsRead)));
    *held = DdsRead{source, read};
    PrBlobReadDesc bytes = prBlobReadDescDefault();
    bytes.done = [](void* PR_NULLABLE context, PrBlobRead* blob) {
        DdsRead held = *static_cast<DdsRead*>(context);
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
    DdsSource* source = static_cast<DdsSource*>(context);
    prBlobSourceRelease(source->bytes);
    prMemoryFree(source);
}

const PrTextureSource* sourceOf(const DdsSource& source);

// Returns true if a source of `dds` produces `format`: the texels as stored, if lib/gpu has their format, and the
// pixels that they decode to.
bool produces(const PrDdsInfo& dds, PrGpuFormat format) {
    if (format != PrGpuFormat_None && format == storedFormat(dds))
        return true;
    if (!dds.decodes)
        return false;
    return format == formOf(dds, PrGpuFormat_R8) || format == formOf(dds, PrGpuFormat_Rgba8) ||
        format == PrGpuFormat_Rgba16F || format == PrGpuFormat_Rgba32F;
}

PrStatus sourceReformat(void* PR_NULLABLE context, PrGpuFormat format, const PrTextureSource* PR_NULLABLE* PR_NONNULL out) {
    DdsSource source = *static_cast<const DdsSource*>(context);
    if (!produces(source.dds, format))
        return PrStatus_NotSupported;
    source.desc.format = format;
    *out = sourceOf(source);
    return PrStatus_Ok;
}

// Returns true if this library converts pixels of `format`: 8-bit RGBA and its forms, and floats, as stored or
// swizzled.
bool converts(PrGpuFormat format) {
    switch (prGpuFormatPlain(prGpuFormatStored(format))) {
        case PrGpuFormat_R8:
        case PrGpuFormat_Rg8:
        case PrGpuFormat_Rgba8:
        case PrGpuFormat_Bgra8:
        case PrGpuFormat_R16F:
        case PrGpuFormat_Rg16F:
        case PrGpuFormat_Rgba16F:
        case PrGpuFormat_R32F:
        case PrGpuFormat_Rg32F:
        case PrGpuFormat_Rgba32F:
            return true;
        default:
            return false;
    }
}

// Writes to `*out` the source of `held` in `format`, or `own` if that is its own format.
PrStatus answerIn(const DdsSource& held, const PrTextureSource* own, PrGpuFormat format,
        const PrTextureSource* PR_NULLABLE* PR_NONNULL out) {
    if (format == held.desc.format) {
        prTextureSourceRetain(own);
        *out = own;
        return PrStatus_Ok;
    }
    DdsSource source = held;
    source.desc.format = format;
    *out = sourceOf(source);
    return PrStatus_Ok;
}

// Answers a need with the stored blocks if the need takes them, unless it needs pixels and the blocks decode, and
// otherwise with pixels: the stored pixels if this library converts their format, R8 for a need of the red channel of
// data alone, or else Rgba8, in its form of the format's transfer function, for 8-bit samples and Rgba32F for wider
// ones.
PrStatus sourceAnswer(void* PR_NULLABLE context, const PrTextureSource* own, const PrTextureNeed* need,
        const PrTextureSource* PR_NULLABLE* PR_NONNULL out) {
    const DdsSource& held = *static_cast<const DdsSource*>(context);
    const PrDdsInfo& dds = held.dds;
    const PrGpuFormat stored = storedFormat(dds);
    const bool blocks = stored != PrGpuFormat_None && prGpuFormatIsCompressed(stored);
    if (blocks && !(need->pixels && dds.decodes) &&
            prTextureNeedTakesBlocks(need, stored, held.desc.alpha, held.desc.extent))
        return answerIn(held, own, stored, out);
    if (!dds.decodes)
        return PrStatus_NotSupported;
    if (prTextureNeedSingleChannel(need) == 0 && !dds.srgb)
        return answerIn(held, own, PrGpuFormat_R8, out);
    if (stored != PrGpuFormat_None && !blocks && converts(stored))
        return answerIn(held, own, stored, out);
    return answerIn(held, own, formOf(dds, dds.sample == PrDdsSample_U8 ? PrGpuFormat_Rgba8 : PrGpuFormat_Rgba32F), out);
}

// Returns a texture source over a copy of `source`, which retains the file's bytes. The format of `source` is one that
// the file produces.
const PrTextureSource* sourceOf(const DdsSource& source) {
    DdsSource* held = allocZeroed<DdsSource>();
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
PrStatus sourceCreate(const PrBlobSource* bytes, const PrBlob* blob, const PrTextureLimits& limits,
        const PrTextureSource* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    PrDdsTexture* texture = nullptr;
    PrStatus status = prDdsTextureCreate(prBlobBytes(blob), &texture);
    if (status != PrStatus_Ok && status != PrStatus_Truncated)
        return status;
    const PrDdsInfo dds = *prDdsTextureInfo(texture);
    prDdsTextureDestroy(texture);
    // A texture whose texels neither decode nor have a format of lib/gpu has nothing to produce.
    if (!dds.decodes && storedFormat(dds) == PrGpuFormat_None)
        return PrStatus_NotSupported;
    // A read decodes one level of one frame at a time, which the extent bounds, so the extent alone limits a texture.
    const DdsSource source{.bytes = bytes, .dds = dds, .desc = descOf(dds)};
    if (!prTextureLimitsAdmit(&limits, &source.desc))
        return PrStatus_Limit;
    *out = sourceOf(source);
    return PrStatus_Ok;
}

bool formatSupportsType(const PrTextureFormat*, PrStringView type) {
    return type == "image/vnd.ms-dds";
}

// A DDS file is opened once every byte has arrived.
PrStatus formatCreate(const PrTextureFormat*, const PrBlobSource* bytes, const PrBlob* blob,
        const PrTextureLimits* limits, bool partial, const PrTextureSource* PR_NULLABLE* PR_NONNULL out) {
    if (partial)
        return PrStatus_Truncated;
    return sourceCreate(bytes, blob, *limits, out);
}

// The format has no state, so one serves every set.
PrTextureFormat gFormat = {.supportsTypeFn = formatSupportsType, .createFn = formatCreate};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrTextureFormat* prDdsTextureFormatCreate(void) {
    return &gFormat;
}
