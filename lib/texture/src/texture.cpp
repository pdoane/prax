// Textures: descriptions, sources, reads, the sources that wrap a source, format sets and uploads.

#include "pr/texture.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/atomic.h"
#include "pr/color_space.h"
#include "pr/float16.h"
#include "pr/mem.h"
#include "pr/memory.h"
#include "pr/mime_sniff.h"
#include "pr/raw_array.h"
#include "pr/texture_backend.h"
#include "pr/thread.h"

#include "texture/src/texture_source.h"

namespace pr_impl {

namespace {

// The id of the next source.
Atomic<uint64_t> gNextSourceId;

// A read: its request, its storage, and its completion state.
struct TextureRead {
    const PrTextureSource* source;
    PrTextureReadDesc desc;         // as the provider sees it. A read that the library uploads is a read into memory.
    bool ownsStorage;

    // The state of a read into a texture that the library serves. The provider fills `desc.storage`. The completion
    // uploads the storage to `texture` on `device` at `origin`, and then frees the storage.
    PrGpuDevice* PR_NULLABLE device;
    PrGpuTexture texture;
    PrIVec2 origin;

    Atomic<bool> done;
    PrStatus status;
    PrMutex lock;
    PrCondition completed;
};

// The formats of a set, in the order that the set tries them.
struct TextureFormatSet {
    RawArray<PrTextureFormat*> formats;
};

// A texture being opened: the set, the bytes, the name of the bytes, the limits, and the caller's callback. The open
// retains the bytes.
struct TextureOpen {
    const TextureFormatSet* set;
    const PrBlobSource* bytes;
    char* name;
    size_t nameSize;
    PrTextureLimits limits;
    PrTextureOpenFn done;
    void* PR_NULLABLE context;
};

// Returns a source of `blob`, which holds the bytes of `open`. The source comes from the first format of the set that
// reads the media type of the blob and does not refuse the blob.
PrStatus textureOpen(const TextureOpen& open, PrBlob* blob, const PrTextureSource* PR_NULLABLE* PR_NONNULL out) {
    PrByteView all = prBlobBytes(blob);
    PrByteView prefix{all.data, all.size < kPrMimeSniffBytes ? all.size : size_t(kPrMimeSniffBytes)};
    PrStringView type = prMimeFormatInfo(prMimeSniff(prefix, PrStringView{open.name, open.nameSize})).type;
    PrStatus status = PrStatus_NotSupported;
    for (uint32_t i = 0; i < open.set->formats.count; ++i) {
        const PrTextureFormat* format = open.set->formats.data[i];
        if (!format->supportsTypeFn(format, type))
            continue;
        status = format->createFn(format, open.bytes, blob, &open.limits, out);
        if (status != PrStatus_NotSupported)
            break;
    }
    return status;
}

// Lets the calling thread wait for a call that completes on any thread.
struct TextureWait {
    PrMutex lock;
    PrCondition completed;
    bool done;
    PrStatus status;
    const void* PR_NULLABLE result;

    void init() {
        prMutexInit(&lock);
        prConditionInit(&completed);
    }

    void complete(PrStatus value, const void* PR_NULLABLE made) {
        prMutexLock(&lock);
        status = value;
        result = made;
        done = true;
        prConditionBroadcast(&completed);
        prMutexUnlock(&lock);
    }

    // Waits for the completion, then frees the state of the wait.
    void wait() {
        prMutexLock(&lock);
        while (!done)
            prConditionWait(&completed, &lock);
        prMutexUnlock(&lock);
        prMutexReset(&lock);
        prConditionReset(&completed);
    }
};

// Returns true if the device can generate the mip levels of a texture of `format`. The device must filter the format
// and render to it.
bool makesLevels(const PrGpuDevice* device, PrGpuFormat format) {
    if (prGpuFormatIsCompressed(format) || prGpuFormatSwizzle(format) != PrGpuSwizzle_None)
        return false;
    PrGpuTextureUsageFlags usage = PrGpuTextureUsageFlag_Sampled | PrGpuTextureUsageFlag_RenderTarget;
    return prGpuDeviceSupportsFormat(device, format, usage) && prGpuFormatFilters(format);
}

// Returns the description of the GPU texture for an upload of `desc` with `flags` on `device`.
PrGpuTextureDesc uploadDesc(const PrGpuDevice* device, const PrTextureDesc& desc, PrTextureUploadFlags flags) {
    PrGpuTextureDesc gpu = prGpuTextureDesc2d(desc.format, desc.extent, PrGpuTextureUsageFlag_Sampled);
    gpu.kind = desc.kind;
    gpu.levels = uint8_t(desc.levelCount);
    gpu.extent.depth = desc.kind == PrGpuTextureKind_3d ? desc.depth : desc.layerCount;
    bool chain = (flags & PrTextureUploadFlag_MakesLevels) != 0 && desc.levelCount == 1 &&
        desc.kind == PrGpuTextureKind_2d && prGpuMipLevels(desc.extent) > 1 && makesLevels(device, desc.format);
    if (chain) {
        gpu.levels = uint8_t(prGpuMipLevels(desc.extent));
        gpu.usage |= PrGpuTextureUsageFlag_RenderTarget;
    }
    PrGpuTextureUsageFlags viewed = gpu.usage | PrGpuTextureUsageFlag_SrgbViews;
    if ((flags & PrTextureUploadFlag_SrgbViews) != 0 && prGpuDeviceSupportsFormat(device, desc.format, viewed))
        gpu.usage = viewed;
    return gpu;
}

// Uploads the levels and layers that `read` holds in its storage to its texture.
void uploadRead(const TextureRead& read) {
    const PrTextureDesc& desc = *prTextureSourceDesc(read.source);
    const uint8_t* texels = read.desc.storage;
    for (uint32_t i = 0; i < read.desc.levelCount; ++i) {
        uint32_t level = read.desc.firstLevel + i;
        PrUVec2 extent = prTextureLevelExtent(desc.extent, level);
        uint32_t depth = read.desc.layerCount;
        if (desc.kind == PrGpuTextureKind_3d)
            depth = desc.depth >> level > 1 ? desc.depth >> level : 1;
        PrGpuTextureRegion region{
            .level = uint8_t(i),
            .x = uint32_t(read.origin.x) >> i,
            .y = uint32_t(read.origin.y) >> i,
            .extent = {extent.x, extent.y, depth},
        };
        size_t offset = prTextureReadOffset(&desc, &read.desc, level, read.desc.firstLayer);
        prGpuTextureUpload(read.device, read.texture, &region, texels + offset,
            prGpuFormatRowBytes(desc.format, extent.x), 0);
    }
}

// An upload: the device, the texture that the upload fills, the description of that texture, and the caller's callback.
struct TextureUpload {
    PrGpuDevice* device;
    PrGpuTexture texture;
    PrGpuTextureDesc gpu;
    PrTextureDesc desc;
    PrTextureUploadFn done;
    void* PR_NULLABLE context;
};

// ---------------------------------------------------------------------------------------------------------------------
// Pixels of sources

// Returns the size in bytes of one sample of type `sample`.
uint32_t sampleSize(PrTextureSample sample) {
    switch (sample) {
        case PrTextureSample_U8:
            return 1;
        case PrTextureSample_U16:
        case PrTextureSample_I16:
        case PrTextureSample_F16:
            return 2;
        default:
            return 4;
    }
}

// Returns the value of the sample of type `sample` at `bytes`.
float sampleValue(PrTextureSample sample, const uint8_t* bytes) {
    switch (sample) {
        case PrTextureSample_U8:
            return float(bytes[0]) / 255;
        case PrTextureSample_U16: {
            uint16_t value = 0;
            prMemCopy(&value, bytes, 2);
            return float(value) / 65535;
        }
        case PrTextureSample_I16: {
            int16_t value = 0;
            prMemCopy(&value, bytes, 2);
            return value <= -32767 ? -1.0f : float(value) / 32767;
        }
        case PrTextureSample_F16: {
            PrFloat16 value{};
            prMemCopy(&value, bytes, 2);
            return prFloat16ToFloat(value);
        }
        case PrTextureSample_F32: {
            float value = 0;
            prMemCopy(&value, bytes, 4);
            return value;
        }
        default: {
            uint32_t value = 0;
            prMemCopy(&value, bytes, 4);
            return float(double(value) / 4294967295.0);
        }
    }
}

// Returns the sample of type `sample` at `bytes` as an 8-bit value, rounded to the nearest of 255 steps. A sample of 8
// or 16 bits is rounded exactly, in integer arithmetic.
uint8_t sampleU8(PrTextureSample sample, const uint8_t* bytes) {
    if (sample == PrTextureSample_U8)
        return bytes[0];
    if (sample == PrTextureSample_U16) {
        uint16_t wide = 0;
        prMemCopy(&wide, bytes, 2);
        return uint8_t((uint32_t(wide) * 255 + 32767) / 65535);
    }
    float value = sampleValue(sample, bytes);
    float clamped = value > 0 ? (value < 1 ? value : 1) : 0;
    return uint8_t(clamped * 255 + 0.5f);
}

// Expands a row of `width` RGB pixels at `from` to opaque RGBA at `to`. The loop is a separate function because the
// compiler vectorizes it there. The compiler does not vectorize the same loop inlined among the other forms of an
// unpack.
void expandRgbRow(const uint8_t* from, uint8_t* to, uint32_t width) {
    for (uint32_t x = 0; x < width; ++x) {
        to[x * 4] = from[x * 3];
        to[x * 4 + 1] = from[x * 3 + 1];
        to[x * 4 + 2] = from[x * 3 + 2];
        to[x * 4 + 3] = 255;
    }
}

// The channels of a stored pixel: the number of channels, and whether the last channel is alpha.
struct ChannelLayout {
    uint32_t count;
    bool alpha;
};

ChannelLayout layoutOf(PrTextureChannels channels) {
    switch (channels) {
        case PrTextureChannels_Gray:
            return ChannelLayout{1, false};
        case PrTextureChannels_GrayAlpha:
            return ChannelLayout{2, true};
        case PrTextureChannels_Rg:
            return ChannelLayout{2, false};
        case PrTextureChannels_Rgb:
            return ChannelLayout{3, false};
        default:
            return ChannelLayout{4, true};
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Plain texels

// The texel layout of a stored plain format: the number of channels, the size of each channel in bytes, and whether red
// and blue are swapped.
struct PlainLayout {
    uint8_t channels;           // 0 for a format that is not plain
    uint8_t bytes;              // 1 for unsigned 8 bits, 2 for half floats, 4 for floats
    bool bgra;
};

// Returns the texel layout of `format`, which is the stored form of a format.
PlainLayout plainLayout(PrGpuFormat format) {
    switch (prGpuFormatPlain(prGpuFormatStored(format))) {
        case PrGpuFormat_R8:
            return PlainLayout{1, 1, false};
        case PrGpuFormat_Rg8:
            return PlainLayout{2, 1, false};
        case PrGpuFormat_Rgba8:
            return PlainLayout{4, 1, false};
        case PrGpuFormat_Bgra8:
            return PlainLayout{4, 1, true};
        case PrGpuFormat_R16F:
            return PlainLayout{1, 2, false};
        case PrGpuFormat_Rg16F:
            return PlainLayout{2, 2, false};
        case PrGpuFormat_Rgba16F:
            return PlainLayout{4, 2, false};
        case PrGpuFormat_R32F:
            return PlainLayout{1, 4, false};
        case PrGpuFormat_Rg32F:
            return PlainLayout{2, 4, false};
        case PrGpuFormat_Rgba32F:
            return PlainLayout{4, 4, false};
        default:
            return PlainLayout{};
    }
}

// Returns true if the library reads and writes the texels of `format` as numbers. The format is then plain or the
// swizzled form of a plain format.
bool isPlain(PrGpuFormat format) {
    return plainLayout(format).channels != 0;
}

// Reads the stored channels of the texel at `bytes` into `values` as numbers. It leaves the other entries of `values`
// unchanged.
void loadStored(PlainLayout layout, const uint8_t* bytes, float* values) {
    for (uint32_t c = 0; c < layout.channels; ++c) {
        uint32_t at = layout.bgra && c < 3 ? 2 - c : c;
        if (layout.bytes == 1) {
            values[c] = float(bytes[at]) / 255;
        } else if (layout.bytes == 2) {
            PrFloat16 half{};
            prMemCopy(&half, bytes + at * 2, 2);
            values[c] = prFloat16ToFloat(half);
        } else {
            prMemCopy(&values[c], bytes + at * 4, 4);
        }
    }
}

// Returns `value`, which is in [0, 1], rounded to the nearest of 255 steps.
uint8_t unorm8(float value) {
    float clamped = value > 0 ? (value < 1 ? value : 1) : 0;
    return uint8_t(clamped * 255 + 0.5f);
}

// Writes the first stored channels of `values` as the texel at `bytes`.
void storeStored(PlainLayout layout, const float* values, uint8_t* bytes) {
    for (uint32_t c = 0; c < layout.channels; ++c) {
        uint32_t at = layout.bgra && c < 3 ? 2 - c : c;
        if (layout.bytes == 1) {
            bytes[at] = unorm8(values[c]);
        } else if (layout.bytes == 2) {
            PrFloat16 half = prFloat16FromFloat(values[c]);
            prMemCopy(bytes + at * 2, &half, 2);
        } else {
            prMemCopy(bytes + at * 4, &values[c], 4);
        }
    }
}

// Reads the texel of `format` at `bytes` as a sampler reads it. The result is red, green, blue and alpha, after the
// swizzle of the format. A color channel that the format does not store is zero, and alpha that it does not store is
// one.
void loadTexel(PrGpuFormat format, PlainLayout layout, const uint8_t* bytes, float* rgba) {
    float stored[4] = {0, 0, 0, 1};
    loadStored(layout, bytes, stored);
    switch (prGpuFormatSwizzle(format)) {
        case PrGpuSwizzle_Alpha:
            rgba[0] = rgba[1] = rgba[2] = 0;
            rgba[3] = stored[0];
            return;
        case PrGpuSwizzle_Luminance:
            rgba[0] = rgba[1] = rgba[2] = stored[0];
            rgba[3] = 1;
            return;
        case PrGpuSwizzle_Intensity:
            rgba[0] = rgba[1] = rgba[2] = rgba[3] = stored[0];
            return;
        case PrGpuSwizzle_Mask:
            rgba[0] = rgba[1] = rgba[2] = 1;
            rgba[3] = stored[0];
            return;
        case PrGpuSwizzle_LuminanceAlpha:
            rgba[0] = rgba[1] = rgba[2] = stored[0];
            rgba[3] = stored[1];
            return;
        default:
            for (uint32_t c = 0; c < 4; ++c)
                rgba[c] = stored[c];
            return;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Color

// Converts the color channels of a texel between the sRGB encoding and linear values. It decodes if `decodes` is set,
// and encodes otherwise.
void convertTransfer(bool decodes, float* rgba) {
    for (uint32_t c = 0; c < 3; ++c) {
        if (decodes)
            rgba[c] = prColorTransferDecode(PrColorTransfer_Srgb, rgba[c]);
        else
            rgba[c] = prColorTransferEncode(PrColorTransfer_Srgb, rgba[c]);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Conversions

// How a conversion makes its texels.
enum {
    ConvertMode_Relabel,        // the source's bits unchanged, under another format
    ConvertMode_Bytes,          // bytes of 8 bits moved between channels, and premultiplied color made straight
    ConvertMode_Numbers,        // each texel read as numbers, converted, and written back
};
typedef uint8_t ConvertMode;

// A source that converts the texels of another source.
struct ConvertSource {
    const PrTextureSource* inner;
    PrTextureDesc innerDesc;
    PrTextureDesc desc;
    PrTextureConvertDesc convert;
    ConvertMode mode;
    bool converts;              // true if the values change between the sRGB encoding and linear floats
    bool decodes;               // true if that change is from the sRGB encoding to linear floats
    bool straightens;           // true if the source's alpha is premultiplied and the target's is not
    bool premultiplies;         // true if the target's alpha is premultiplied and the source's is not
};

// A read of a conversion in progress: the source, and the read that it completes.
struct ConvertRead {
    const ConvertSource* source;
    PrTextureRead* outer;
};

// Returns true if `channels` maps each channel to itself.
bool identityChannels(const uint8_t* channels) {
    return channels[0] == 0 && channels[1] == 1 && channels[2] == 2 && channels[3] == 3;
}

// Returns true if the conversion leaves each byte of a texel where it is. Both formats then have four channels of 8
// bits in the same order, and the conversion only premultiplies the texels or makes them opaque. Such a conversion
// needs no storage of its own: its source is read into the storage of the conversion's read, and the texels are
// converted there.
bool convertsInPlace(const ConvertSource& source) {
    if (source.mode != ConvertMode_Bytes || source.straightens)
        return false;
    PlainLayout in = plainLayout(source.innerDesc.format);
    PlainLayout out = plainLayout(source.desc.format);
    const uint8_t* channels = source.convert.channels;
    bool alphaOne = channels[3] == kPrTextureChannelOne || source.desc.alpha == PrTextureAlpha_None;
    return in.channels == 4 && out.channels == 4 && in.bytes == 1 && out.bytes == 1 && in.bgra == out.bgra &&
        prGpuFormatSwizzle(source.innerDesc.format) == PrGpuSwizzle_None && channels[0] == 0 && channels[1] == 1 &&
        channels[2] == 2 && (channels[3] == 3 || alphaOne);
}

// Makes `count` texels of four bytes at `texels` opaque.
void makeOpaque(uint8_t* texels, size_t count) {
    for (size_t i = 0; i < count; ++i)
        texels[i * 4 + 3] = 255;
}

// Premultiplies `count` texels of four bytes at `texels`.
void premultiplyTexels(uint8_t* texels, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        uint8_t* texel = texels + i * 4;
        uint32_t a = texel[3];
        if (a == 255)
            continue;
        for (uint32_t c = 0; c < 3; ++c)
            texel[c] = uint8_t((uint32_t(texel[c]) * a + 127) / 255);
    }
}

void convertTexels(const ConvertSource& source, const uint8_t* from, uint8_t* to, size_t count) {
    if (convertsInPlace(source)) {
        PR_ASSERT(from == to);
        if (source.convert.channels[3] == kPrTextureChannelOne || source.desc.alpha == PrTextureAlpha_None)
            makeOpaque(to, count);
        else if (source.premultiplies)
            premultiplyTexels(to, count);
        return;
    }
    PlainLayout in = plainLayout(source.innerDesc.format);
    PlainLayout out = plainLayout(source.desc.format);
    uint32_t inBytes = uint32_t(in.channels) * in.bytes;
    uint32_t outBytes = uint32_t(out.channels) * out.bytes;
    const uint8_t* channels = source.convert.channels;
    bool opaque = source.desc.alpha == PrTextureAlpha_None;
    if (source.mode == ConvertMode_Bytes) {
        // The stored byte of each channel of the source's texel, indexed by channel as a sampler reads it. The entry is
        // -1 for a channel that is a constant.
        int32_t places[4] = {0, in.channels > 1 ? 1 : -1, in.channels > 2 ? 2 : -1, in.channels > 3 ? 3 : -1};
        if (in.bgra) {
            places[0] = 2;
            places[2] = 0;
        }
        PrGpuSwizzle swizzle = prGpuFormatSwizzle(source.innerDesc.format);
        if (swizzle == PrGpuSwizzle_Luminance || swizzle == PrGpuSwizzle_Intensity)
            places[1] = places[2] = 0;
        if (swizzle == PrGpuSwizzle_Intensity)
            places[3] = 0;
        // Moving one channel alone is a copy of bytes.
        if (out.channels == 1 && !source.straightens && channels[0] < 4 && places[channels[0]] >= 0) {
            const uint8_t* moved = from + places[channels[0]];
            for (size_t i = 0; i < count; ++i)
                to[i] = moved[i * inBytes];
            return;
        }
        for (size_t i = 0; i < count; ++i) {
            const uint8_t* texel = from + i * inBytes;
            uint8_t rgba[6] = {0, 0, 0, 255, 0, 255};
            for (uint32_t c = 0; c < 4; ++c) {
                if (places[c] >= 0)
                    rgba[c] = texel[places[c]];
            }
            if (source.straightens && rgba[3] != 0) {
                for (uint32_t c = 0; c < 3; ++c) {
                    uint32_t straight = (uint32_t(rgba[c]) * 255 + rgba[3] / 2) / rgba[3];
                    rgba[c] = uint8_t(straight > 255 ? 255 : straight);
                }
            }
            if (source.premultiplies) {
                for (uint32_t c = 0; c < 3; ++c)
                    rgba[c] = uint8_t((uint32_t(rgba[c]) * rgba[3] + 127) / 255);
            }
            uint8_t* written = to + i * outBytes;
            for (uint32_t c = 0; c < out.channels; ++c) {
                uint8_t value = rgba[channels[c]];
                if (c == 3 && opaque)
                    value = 255;
                written[out.bgra && c < 3 ? 2 - c : c] = value;
            }
        }
        return;
    }
    const float constants[2] = {0, 1};
    for (size_t i = 0; i < count; ++i) {
        float rgba[4];
        loadTexel(source.innerDesc.format, in, from + i * inBytes, rgba);
        float mapped[4];
        for (uint32_t c = 0; c < 4; ++c)
            mapped[c] = channels[c] < 4 ? rgba[channels[c]] : constants[channels[c] - kPrTextureChannelZero];
        if (source.straightens && mapped[3] > 0) {
            for (uint32_t c = 0; c < 3; ++c)
                mapped[c] /= mapped[3];
        }
        if (source.converts)
            convertTransfer(source.decodes, mapped);
        if (opaque)
            mapped[3] = 1;
        if (source.premultiplies) {
            for (uint32_t c = 0; c < 3; ++c)
                mapped[c] *= mapped[3];
        }
        storeStored(out, mapped, to + i * outBytes);
    }
}

// Completes the read of a conversion after the read of its source is done.
void convertDone(void* PR_NULLABLE context, PrTextureRead* inner) {
    ConvertRead* held = static_cast<ConvertRead*>(context);
    const ConvertSource& source = *held->source;
    PrTextureRead* outer = held->outer;
    prMemoryFree(held);
    PrStatus status = prTextureReadStatus(inner);
    const PrTextureReadDesc& read = *prTextureReadInfo(outer);
    if (status == PrStatus_Ok && source.mode != ConvertMode_Relabel) {
        const uint8_t* from = prTextureReadStorage(inner);
        uint8_t* to = prTextureReadStorage(outer);
        for (uint32_t level = read.firstLevel; level < read.firstLevel + read.levelCount; ++level) {
            PrUVec2 extent = prTextureLevelExtent(source.desc.extent, level);
            size_t count = size_t(extent.x) * extent.y * (prTextureLevelSize(&source.desc, level) /
                prGpuFormatLevelBytes(source.desc.format, extent));
            for (uint32_t layer = read.firstLayer; layer < read.firstLayer + read.layerCount; ++layer) {
                convertTexels(source, from + prTextureReadOffset(&source.innerDesc, &read, level, layer),
                    to + prTextureReadOffset(&source.desc, &read, level, layer), count);
            }
        }
    }
    prTextureReadDestroy(inner);
    prTextureReadComplete(outer, status);
}

void convertRead(void* PR_NULLABLE context, PrTextureRead* read) {
    const ConvertSource* source = static_cast<const ConvertSource*>(context);
    ConvertRead* held = static_cast<ConvertRead*>(prMemoryAlloc(sizeof(ConvertRead)));
    *held = ConvertRead{source, read};
    PrTextureReadDesc inner = *prTextureReadInfo(read);
    // A relabel reads its source into the storage of the outer read. So does a conversion that keeps its bytes in
    // place. A relabel's read into a texture is a read of its source into that texture.
    bool inPlace = source->mode == ConvertMode_Relabel || convertsInPlace(*source);
    inner.storage = inPlace ? prTextureReadStorage(read) : nullptr;
    inner.done = convertDone;
    inner.context = held;
    prTextureReadCreate(source->inner, &inner);
}

void convertRelease(void* PR_NULLABLE context) {
    ConvertSource* source = static_cast<ConvertSource*>(context);
    prTextureSourceRelease(source->inner);
    prMemoryFree(source);
}

// ---------------------------------------------------------------------------------------------------------------------
// Mip levels

// A source that generates the mip levels that another source lacks.
struct MipsSource {
    const PrTextureSource* inner;
    PrTextureDesc innerDesc;
    PrTextureDesc desc;
};

// A read of the levels of a MipsSource in progress: the source, the read that it completes, and the first level that it
// reads from the inner source.
struct MipsRead {
    const MipsSource* source;
    PrTextureRead* outer;
    uint32_t firstInner;
};

// The linear light of each 8-bit value of the sRGB encoding, and the sRGB value of each 4096th of linear light.
struct SrgbTables {
    float linear[256];
    uint8_t encoded[4097];
};

// Returns the tables. The first call fills them.
const SrgbTables& srgbTables() {
    static SrgbTables* tables = [] {
        SrgbTables* made = allocZeroed<SrgbTables>();
        for (uint32_t i = 0; i < 256; ++i)
            made->linear[i] = prColorTransferDecode(PrColorTransfer_Srgb, float(i) / 255);
        for (uint32_t i = 0; i <= 4096; ++i)
            made->encoded[i] = unorm8(prColorTransferEncode(PrColorTransfer_Srgb, float(i) / 4096));
        return made;
    }();
    return *tables;
}

// Writes at `to` the level that follows the level at `from`, which has `extent` texels. Each texel is the average of
// two by two texels of the level at `from`. If a side of that level is odd, its last row or column is repeated.
void halve(PrGpuFormat format, const uint8_t* from, PrUVec2 extent, uint8_t* to) {
    PlainLayout layout = plainLayout(format);
    uint32_t texelBytes = uint32_t(layout.channels) * layout.bytes;
    PrUVec2 half = prTextureLevelExtent(extent, 1);
    bool srgb = prGpuFormatIsSrgb(format);
    const SrgbTables* tables = srgb ? &srgbTables() : nullptr;
    for (uint32_t y = 0; y < half.y; ++y) {
        uint32_t rows[2] = {y * 2 < extent.y ? y * 2 : extent.y - 1, y * 2 + 1 < extent.y ? y * 2 + 1 : extent.y - 1};
        for (uint32_t x = 0; x < half.x; ++x) {
            uint32_t columns[2] = {
                x * 2 < extent.x ? x * 2 : extent.x - 1,
                x * 2 + 1 < extent.x ? x * 2 + 1 : extent.x - 1,
            };
            uint8_t* written = to + (size_t(y) * half.x + x) * texelBytes;
            if (layout.bytes == 1) {
                for (uint32_t c = 0; c < layout.channels; ++c) {
                    bool encoded = tables != nullptr && c < 3;
                    float sum = 0;
                    uint32_t integers = 0;
                    for (uint32_t row : rows) {
                        for (uint32_t column : columns) {
                            uint8_t value = from[(size_t(row) * extent.x + column) * texelBytes + c];
                            if (encoded)
                                sum += tables->linear[value];
                            else
                                integers += value;
                        }
                    }
                    if (encoded)
                        written[c] = tables->encoded[uint32_t(sum / 4 * 4096 + 0.5f)];
                    else
                        written[c] = uint8_t((integers + 2) / 4);
                }
                continue;
            }
            float sum[4] = {0, 0, 0, 0};
            for (uint32_t row : rows) {
                for (uint32_t column : columns) {
                    float values[4];
                    loadStored(layout, from + (size_t(row) * extent.x + column) * texelBytes, values);
                    for (uint32_t c = 0; c < layout.channels; ++c)
                        sum[c] += values[c];
                }
            }
            for (float& value : sum)
                value /= 4;
            storeStored(layout, sum, written);
        }
    }
}

// Completes a read of levels after the read of the inner source's own levels is done. It copies the levels that the
// read asks for and generates the rest.
void mipsDone(void* PR_NULLABLE context, PrTextureRead* inner) {
    MipsRead* held = static_cast<MipsRead*>(context);
    const MipsSource& source = *held->source;
    PrTextureRead* outer = held->outer;
    uint32_t firstInner = held->firstInner;
    prMemoryFree(held);
    PrStatus status = prTextureReadStatus(inner);
    const PrTextureReadDesc& read = *prTextureReadInfo(outer);
    const PrTextureReadDesc& innerRead = *prTextureReadInfo(inner);
    uint32_t last = read.firstLevel + read.levelCount - 1;
    uint32_t innerLast = source.innerDesc.levelCount - 1;
    const PrTextureDesc& desc = source.desc;
    uint8_t* scratch[2] = {nullptr, nullptr};
    for (uint32_t layer = read.firstLayer; status == PrStatus_Ok && layer < read.firstLayer + read.layerCount; ++layer) {
        for (uint32_t level = firstInner; level <= innerLast && level <= last; ++level) {
            if (level < read.firstLevel)
                continue;
            prMemCopy(prTextureReadStorage(outer) + prTextureReadOffset(&desc, &read, level, layer),
                prTextureReadStorage(inner) + prTextureReadOffset(&source.innerDesc, &innerRead, level, layer),
                prTextureLevelSize(&desc, level));
        }
        const uint8_t* before =
            prTextureReadStorage(inner) + prTextureReadOffset(&source.innerDesc, &innerRead, innerLast, layer);
        for (uint32_t level = innerLast + 1; level <= last; ++level) {
            uint8_t* made = nullptr;
            if (level >= read.firstLevel)
                made = prTextureReadStorage(outer) + prTextureReadOffset(&desc, &read, level, layer);
            if (made == nullptr) {
                uint32_t which = level % 2;
                if (scratch[which] == nullptr)
                    scratch[which] = static_cast<uint8_t*>(prMemoryAlloc(prTextureLevelSize(&desc, innerLast + 1)));
                made = scratch[which];
            }
            halve(desc.format, before, prTextureLevelExtent(desc.extent, level - 1), made);
            before = made;
        }
    }
    prMemoryFree(scratch[0]);
    prMemoryFree(scratch[1]);
    prTextureReadDestroy(inner);
    prTextureReadComplete(outer, status);
}

void mipsRead(void* PR_NULLABLE context, PrTextureRead* read) {
    const MipsSource* source = static_cast<const MipsSource*>(context);
    const PrTextureReadDesc& asked = *prTextureReadInfo(read);
    uint32_t innerLast = source->innerDesc.levelCount - 1;
    PrTextureReadDesc inner = asked;
    if (asked.firstLevel + asked.levelCount - 1 <= innerLast) {
        // The source has every level that the read asks for.
        inner.storage = prTextureReadStorage(read);
        inner.done = [](void* PR_NULLABLE outer, PrTextureRead* done) {
            PrStatus status = prTextureReadStatus(done);
            prTextureReadDestroy(done);
            prTextureReadComplete(static_cast<PrTextureRead*>(outer), status);
        };
        inner.context = read;
        prTextureReadCreate(source->inner, &inner);
        return;
    }
    MipsRead* held = static_cast<MipsRead*>(prMemoryAlloc(sizeof(MipsRead)));
    uint32_t firstInner = asked.firstLevel < innerLast ? asked.firstLevel : innerLast;
    *held = MipsRead{source, read, firstInner};
    inner.firstLevel = firstInner;
    inner.levelCount = innerLast - firstInner + 1;
    inner.storage = nullptr;
    inner.done = mipsDone;
    inner.context = held;
    prTextureReadCreate(source->inner, &inner);
}

void mipsRelease(void* PR_NULLABLE context) {
    MipsSource* source = static_cast<MipsSource*>(context);
    prTextureSourceRelease(source->inner);
    prMemoryFree(source);
}

// ---------------------------------------------------------------------------------------------------------------------
// Tiles

// A source of a grid of tiles. It holds the extent of a cell and the tiles. Each tile has its pixels in the texture's
// format and its cell.
struct TilesSource {
    PrTextureDesc desc;
    PrUVec2 cellExtent;
    RawArray<PrTextureTile> tiles;
};

// A read of a grid in progress: the read that it completes, and the reads of the first level of each tile. `pending`
// counts the tile reads that have not completed, plus one until every read has started.
struct TilesRead {
    const TilesSource* source;
    PrTextureRead* outer;
    Atomic<uint32_t> pending;
    PrTextureRead* reads[];
};

// Returns a source of the pixels of `source` in a plain format. It is `source` itself if its format is plain, and
// otherwise a source of a format that `source` decodes to. Returns null if `source` decodes to no plain format.
const PrTextureSource* PR_NULLABLE tilePixels(const PrTextureSource* source) {
    if (isPlain(prTextureSourceDesc(source)->format)) {
        prTextureSourceRetain(source);
        return source;
    }
    const PrGpuFormat decoded[] = {PrGpuFormat_Rgba8, PrGpuFormat_Rgba8Srgb, PrGpuFormat_Rgba16F, PrGpuFormat_Rgba32F};
    for (PrGpuFormat candidate : decoded) {
        const PrTextureSource* pixels = nullptr;
        if (prTextureSourceProduces(source, candidate) &&
                prTextureSourceReformat(source, candidate, &pixels) == PrStatus_Ok)
            return pixels;
    }
    return nullptr;
}

// The tile texels along one axis that a resampling reads for one cell texel. They are `count` texels starting at
// `first`, each with a weight.
struct TileTaps {
    uint32_t first;
    uint32_t count;
    float weights[2];           // used if `count` is at most 2; otherwise the weights are equal
};

// Returns the texels that the cell texel `at` reads along one axis of a tile. The tile has `from` texels on that axis
// and the cell has `to`. A cell texel that covers one tile texel or more averages the texels that it covers. A cell
// texel that covers less than one interpolates the two nearest texels.
TileTaps tileTaps(uint32_t at, uint32_t from, uint32_t to) {
    if (from >= to) {
        uint32_t first = uint32_t(uint64_t(at) * from / to);
        uint32_t end = uint32_t((uint64_t(at) + 1) * from / to);
        return TileTaps{.first = first, .count = end > first ? end - first : 1, .weights = {1, 0}};
    }
    float center = (float(at) + 0.5f) * float(from) / float(to) - 0.5f;
    if (center <= 0)
        return TileTaps{.first = 0, .count = 1, .weights = {1, 0}};
    uint32_t first = uint32_t(center);
    if (first + 1 >= from)
        return TileTaps{.first = from - 1, .count = 1, .weights = {1, 0}};
    float t = center - float(first);
    return TileTaps{.first = first, .count = 2, .weights = {1 - t, t}};
}

// Returns the weight of tap `k` of `taps`.
float tapWeight(const TileTaps& taps, uint32_t k) {
    return taps.count <= 2 ? taps.weights[k] : 1.0f / float(taps.count);
}

// Writes the texels of a tile to its cell of a level. The tile has `extent` texels in the texture's format. The rows of
// the level are `rowBytes` bytes long.
void tileWrite(const TilesSource& source, const uint8_t* texels, PrUVec2 extent, PrUVec2 cell, uint8_t* level,
        size_t rowBytes) {
    PlainLayout layout = plainLayout(source.desc.format);
    uint32_t texelBytes = uint32_t(layout.channels) * layout.bytes;
    PrUVec2 cellExtent = source.cellExtent;
    uint8_t* origin = level + size_t(cell.y) * cellExtent.y * rowBytes + size_t(cell.x) * cellExtent.x * texelBytes;
    if (extent.x == cellExtent.x && extent.y == cellExtent.y) {
        for (uint32_t y = 0; y < extent.y; ++y)
            prMemCopy(origin + y * rowBytes, texels + size_t(y) * extent.x * texelBytes, size_t(extent.x) * texelBytes);
        return;
    }
    for (uint32_t y = 0; y < cellExtent.y; ++y) {
        TileTaps rows = tileTaps(y, extent.y, cellExtent.y);
        for (uint32_t x = 0; x < cellExtent.x; ++x) {
            TileTaps columns = tileTaps(x, extent.x, cellExtent.x);
            float sum[4] = {0, 0, 0, 0};
            for (uint32_t j = 0; j < rows.count; ++j) {
                for (uint32_t i = 0; i < columns.count; ++i) {
                    float weight = tapWeight(rows, j) * tapWeight(columns, i);
                    float values[4] = {0, 0, 0, 0};
                    size_t at = (size_t(rows.first + j) * extent.x + columns.first + i) * texelBytes;
                    loadStored(layout, texels + at, values);
                    for (uint32_t c = 0; c < 4; ++c)
                        sum[c] += values[c] * weight;
                }
            }
            storeStored(layout, sum, origin + y * rowBytes + size_t(x) * texelBytes);
        }
    }
}

// Completes a read of a grid after the last tile read has completed. It writes each tile that was read to its cell.
void tilesFinish(TilesRead* held) {
    const TilesSource& source = *held->source;
    PrTextureRead* outer = held->outer;
    const PrTextureReadDesc& read = *prTextureReadInfo(outer);
    uint8_t* level = prTextureReadStorage(outer) + prTextureReadOffset(&source.desc, &read, 0, 0);
    size_t rowBytes = prTextureLevelSize(&source.desc, 0) / source.desc.extent.y;
    prMemZero(level, prTextureLevelSize(&source.desc, 0));
    for (uint32_t i = 0; i < source.tiles.count; ++i) {
        PrTextureRead* tile = held->reads[i];
        if (prTextureReadStatus(tile) == PrStatus_Ok) {
            PrUVec2 extent = prTextureSourceDesc(source.tiles.data[i].source)->extent;
            tileWrite(source, prTextureReadStorage(tile), extent, source.tiles.data[i].cell, level, rowBytes);
        }
        prTextureReadDestroy(tile);
    }
    prMemoryFree(held);
    prTextureReadComplete(outer, PrStatus_Ok);
}

void tilesRead(void* PR_NULLABLE context, PrTextureRead* read) {
    const TilesSource* source = static_cast<const TilesSource*>(context);
    uint32_t count = source->tiles.count;
    TilesRead* held = static_cast<TilesRead*>(prMemoryAllocZeroed(sizeof(TilesRead) + count * sizeof(PrTextureRead*)));
    held->source = source;
    held->outer = read;
    held->pending.store(count + 1);
    for (uint32_t i = 0; i < count; ++i) {
        PrTextureReadDesc tile{
            .firstLevel = 0,
            .levelCount = 1,
            .firstLayer = 0,
            .layerCount = 1,
            .done = [](void* PR_NULLABLE context, PrTextureRead*) {
                TilesRead* held = static_cast<TilesRead*>(context);
                if (held->pending.fetchSub(1) == 1)
                    tilesFinish(held);
            },
            .context = held,
        };
        held->reads[i] = prTextureReadCreate(source->tiles.data[i].source, &tile);
    }
    if (held->pending.fetchSub(1) == 1)
        tilesFinish(held);
}

void tilesRelease(void* PR_NULLABLE context) {
    TilesSource* source = static_cast<TilesSource*>(context);
    for (const PrTextureTile& tile : source->tiles.span())
        prTextureSourceRelease(tile.source);
    source->tiles.reset();
    prMemoryFree(source);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrTextureRead : TextureRead {};
struct PrTextureFormatSet : TextureFormatSet {};

// ---------------------------------------------------------------------------------------------------------------------
// Descriptions

PrTextureDesc prTextureDesc2d(PrGpuFormat format, PrUVec2 extent, PrTextureAlpha alpha) {
    return PrTextureDesc{
        .kind = PrGpuTextureKind_2d,
        .extent = extent,
        .depth = 1,
        .layerCount = 1,
        .levelCount = 1,
        .format = format,
        .alpha = alpha,
    };
}

PrUVec2 prTextureDescDisplayedExtent(const PrTextureDesc* desc) {
    if ((desc->orientation & PrTextureOrientationFlag_Transpose) != 0)
        return PrUVec2{desc->extent.y, desc->extent.x};
    return desc->extent;
}

PrUVec2 prTextureLevelExtent(PrUVec2 extent, uint32_t level) {
    uint32_t width = extent.x >> level;
    uint32_t height = extent.y >> level;
    return PrUVec2{width > 0 ? width : 1, height > 0 ? height : 1};
}

size_t prTextureLevelSize(const PrTextureDesc* desc, uint32_t level) {
    uint32_t depth = 1;
    if (desc->kind == PrGpuTextureKind_3d && desc->depth >> level > 1)
        depth = desc->depth >> level;
    return size_t(prGpuFormatLevelBytes(desc->format, prTextureLevelExtent(desc->extent, level))) * depth;
}

PrTextureLimits prTextureLimitsDefault(void) {
    uint64_t pixels = uint64_t(8192) * 8192;
    return PrTextureLimits{.pixels = pixels, .workingBytes = pixels * 64};
}

// ---------------------------------------------------------------------------------------------------------------------
// Sources

const PrTextureSource* prTextureSourceCreate(const PrTextureSourceDesc* desc) {
    PR_ASSERT(desc->texture.levelCount >= 1 && desc->texture.levelCount <= kPrTextureMaxLevels);
    PR_ASSERT(desc->texture.layerCount >= 1 && desc->formatCount <= kPrTextureSourceMaxFormats);
    PR_ASSERT(desc->formatCount == 0 || desc->reformat != nullptr);
    PR_ASSERT(desc->previewCount <= kPrTextureSourceMaxPreviews);
    PR_ASSERT(desc->usage == 0 || (desc->flags & PrTextureSourceFlag_FillsTextures) != 0);
    PrTextureSource* source = allocZeroed<PrTextureSource>();
    source->references.storeRelaxed(1);
    source->id = gNextSourceId.fetchAdd(1) + 1;
    source->desc = *desc;
    if (desc->formatCount != 0)
        prMemCopy(source->formats, desc->formats, desc->formatCount * sizeof(PrGpuFormat));
    source->desc.formats = source->formats;
    for (uint32_t i = 0; i < desc->previewCount; ++i) {
        prTextureSourceRetain(desc->previews[i]);
        source->previews[i] = desc->previews[i];
    }
    source->desc.previews = nullptr;
    return source;
}

const PrTextureSource* prTextureSourceCreatePixels(const PrTextureDesc* desc, PrPixelView pixels) {
    PR_ASSERT(desc->kind == PrGpuTextureKind_2d && desc->levelCount == 1 && desc->layerCount == 1 &&
        isPlain(desc->format));
    PR_ASSERT(pixels.extent.x == desc->extent.x && pixels.extent.y == desc->extent.y);
    size_t row = prGpuFormatRowBytes(desc->format, desc->extent.x);
    PR_ASSERT(row <= pixels.stride || desc->extent.y <= 1);
    size_t size = row * desc->extent.y;
    // One allocation holds the size and then the pixels. The allocation is the context of the source.
    uint8_t* kept = static_cast<uint8_t*>(prMemoryAlloc(sizeof(size_t) + size + 1));
    prMemCopy(kept, &size, sizeof(size_t));
    for (uint32_t y = 0; y < desc->extent.y; ++y)
        prMemCopy(kept + sizeof(size_t) + y * row, pixels.data + y * pixels.stride, row);
    // The alpha of an opaque texture reads as one, whatever the alpha of the pixels holds. The copy therefore stores
    // one.
    PlainLayout layout = plainLayout(desc->format);
    if (desc->alpha == PrTextureAlpha_None && layout.channels == 4) {
        uint8_t one[4] = {255};
        if (layout.bytes == 2) {
            PrFloat16 half = prFloat16FromFloat(1);
            prMemCopy(one, &half, 2);
        } else if (layout.bytes == 4) {
            float full = 1;
            prMemCopy(one, &full, 4);
        }
        size_t texel = size_t(layout.bytes) * 4;
        uint8_t* texels = kept + sizeof(size_t);
        for (size_t at = texel - layout.bytes; at < size; at += texel)
            prMemCopy(texels + at, one, layout.bytes);
    }
    PrTextureSourceDesc source{
        .texture = *desc,
        .read = [](void* PR_NULLABLE context, PrTextureRead* read) {
            const uint8_t* held = static_cast<const uint8_t*>(context);
            size_t bytes = 0;
            prMemCopy(&bytes, held, sizeof(size_t));
            prMemCopy(prTextureReadStorage(read), held + sizeof(size_t), bytes);
            prTextureReadComplete(read, PrStatus_Ok);
        },
        .release = [](void* PR_NULLABLE context) { prMemoryFree(context); },
        .context = kept,
    };
    return prTextureSourceCreate(&source);
}

const PrTextureSource* prTextureSourceCreateCopy(PrPixelView pixels, PrPixelFormat format, PrTextureAlpha alpha) {
    PrTextureDesc desc = prTextureDesc2d(prGpuFormatFromPixelFormat(format), pixels.extent, alpha);
    return prTextureSourceCreatePixels(&desc, pixels);
}

void prTextureSourceRetain(const PrTextureSource* source) {
    source->references.fetchAdd(1);
}

void prTextureSourceRelease(const PrTextureSource* PR_NULLABLE source) {
    if (source == nullptr || source->references.fetchSub(1) != 1)
        return;
    PrTextureSource* held = const_cast<PrTextureSource*>(source);
    if (held->released != nullptr)
        held->released(held->releasedContext, held);
    for (uint32_t i = 0; i < held->desc.previewCount; ++i)
        prTextureSourceRelease(held->previews[i]);
    if (held->desc.release != nullptr)
        held->desc.release(held->desc.context);
    prMemoryFree(held);
}

const PrTextureDesc* prTextureSourceDesc(const PrTextureSource* source) {
    return &source->desc.texture;
}

uint32_t prTextureSourceReferences(const PrTextureSource* source) {
    return source->references.load();
}

uint64_t prTextureSourceId(const PrTextureSource* source) {
    return source->id;
}

uint32_t prTextureSourceFormats(const PrTextureSource* source, PrGpuFormat* PR_NULLABLE out, uint32_t capacity) {
    for (uint32_t i = 0; i < source->desc.formatCount && i < capacity; ++i)
        out[i] = source->formats[i];
    return source->desc.formatCount;
}

bool prTextureSourceProduces(const PrTextureSource* source, PrGpuFormat format) {
    if (format == source->desc.texture.format)
        return true;
    for (uint32_t i = 0; i < source->desc.formatCount; ++i) {
        if (source->formats[i] == format)
            return true;
    }
    return false;
}

uint32_t prTextureSourcePreviewCount(const PrTextureSource* source) {
    return source->desc.previewCount;
}

const PrTextureSource* prTextureSourcePreview(const PrTextureSource* source, uint32_t index) {
    PR_ASSERT(index < source->desc.previewCount);
    return source->previews[index];
}

bool prTextureSourcePlays(const PrTextureSource* source) {
    return source->desc.frame != nullptr;
}

void prTextureSourceFrameAt(const PrTextureSource* source, double time, PrTextureFrame* out) {
    *out = PrTextureFrame{};
    if (source->desc.frame != nullptr)
        source->desc.frame(source->desc.context, time, out);
    if (out->source == nullptr) {
        prTextureSourceRetain(source);
        out->source = source;
    }
}

PrStatus prTextureSourceReformat(const PrTextureSource* source, PrGpuFormat format,
        const PrTextureSource* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    if (format == source->desc.texture.format) {
        prTextureSourceRetain(source);
        *out = source;
        return PrStatus_Ok;
    }
    if (!prTextureSourceProduces(source, format))
        return PrStatus_NotSupported;
    PrStatus status = source->desc.reformat(source->desc.context, format, out);
    if (status != PrStatus_Ok) {
        prTextureSourceRelease(*out);
        *out = nullptr;
    }
    return status;
}

// ---------------------------------------------------------------------------------------------------------------------
// Reads

PrTextureReadDesc prTextureReadDescAll(const PrTextureDesc* desc) {
    return PrTextureReadDesc{.levelCount = desc->levelCount, .layerCount = desc->layerCount};
}

size_t prTextureReadSize(const PrTextureDesc* desc, const PrTextureReadDesc* read) {
    size_t size = 0;
    for (uint32_t level = read->firstLevel; level < read->firstLevel + read->levelCount; ++level)
        size += prTextureLevelSize(desc, level) * read->layerCount;
    return size;
}

size_t prTextureReadOffset(const PrTextureDesc* desc, const PrTextureReadDesc* read, uint32_t level, uint32_t layer) {
    PR_ASSERT(level >= read->firstLevel && level < read->firstLevel + read->levelCount);
    PR_ASSERT(layer >= read->firstLayer && layer < read->firstLayer + read->layerCount);
    size_t offset = 0;
    for (uint32_t before = read->firstLevel; before < level; ++before)
        offset += prTextureLevelSize(desc, before) * read->layerCount;
    return offset + prTextureLevelSize(desc, level) * (layer - read->firstLayer);
}

PrTextureRead* prTextureReadCreate(const PrTextureSource* source, const PrTextureReadDesc* desc) {
    const PrTextureDesc& texture = source->desc.texture;
    PR_ASSERT(desc->levelCount >= 1 && desc->firstLevel + desc->levelCount <= texture.levelCount);
    PR_ASSERT(desc->layerCount >= 1 && desc->firstLayer + desc->layerCount <= texture.layerCount);
    PR_ASSERT(desc->device == nullptr || desc->storage == nullptr);
    PrTextureRead* read = allocZeroed<PrTextureRead>();
    prTextureSourceRetain(source);
    read->source = source;
    read->desc = *desc;
    bool fills = desc->device != nullptr && (source->desc.flags & PrTextureSourceFlag_FillsTextures) != 0;
    if (desc->device != nullptr && !fills) {
        // The provider reads into memory, and the completion uploads the result.
        read->device = desc->device;
        read->texture = desc->texture;
        read->origin = desc->origin;
        read->desc.device = nullptr;
        read->desc.texture = PrGpuTexture{};
        read->desc.origin = PrIVec2{};
    }
    if (read->desc.storage == nullptr && !fills) {
        // The allocation has one extra byte, so that a read of no bytes also has storage.
        read->desc.storage = static_cast<uint8_t*>(prMemoryAlloc(prTextureReadSize(&texture, desc) + 1));
        read->ownsStorage = true;
    }
    read->done.store(false);
    prMutexInit(&read->lock);
    prConditionInit(&read->completed);
    source->desc.read(source->desc.context, read);
    return read;
}

bool prTextureReadIsDone(const PrTextureRead* read) {
    return read->done.load();
}

void prTextureReadWait(PrTextureRead* read) {
    if (read->done.load())
        return;
    prMutexLock(&read->lock);
    while (!read->done.load())
        prConditionWait(&read->completed, &read->lock);
    prMutexUnlock(&read->lock);
}

PrStatus prTextureReadStatus(const PrTextureRead* read) {
    PR_ASSERT(read->done.load());
    return read->status;
}

uint8_t* PR_NULLABLE prTextureReadStorage(const PrTextureRead* read) {
    return read->desc.storage;
}

void prTextureReadDestroy(PrTextureRead* PR_NULLABLE read) {
    if (read == nullptr)
        return;
    // A provider may still hold the lock while it wakes the waiters.
    prMutexLock(&read->lock);
    PR_ASSERT(read->done.load());
    prMutexUnlock(&read->lock);
    if (read->ownsStorage)
        prMemoryFree(read->desc.storage);
    prTextureSourceRelease(read->source);
    prMutexReset(&read->lock);
    prConditionReset(&read->completed);
    prMemoryFree(read);
}

const PrTextureSource* prTextureReadSource(const PrTextureRead* read) {
    return read->source;
}

const PrTextureReadDesc* prTextureReadInfo(const PrTextureRead* read) {
    return &read->desc;
}

void prTextureReadComplete(PrTextureRead* read, PrStatus status) {
    PR_ASSERT(!read->done.load());
    if (read->device != nullptr) {
        // The storage is uploaded and freed before any waiter or callback learns of the completion, so it does not
        // outlive the read.
        if (status == PrStatus_Ok) {
            uploadRead(*read);
            if (prGpuDeviceIsLost(read->device))
                status = PrStatus_Lost;
        }
        prMemoryFree(read->desc.storage);
        read->desc.storage = nullptr;
        read->ownsStorage = false;
    }
    read->status = status;
    // The callback may destroy the read. The waiters are therefore woken first, under the lock that the destroy takes.
    PrTextureReadDoneFn done = read->desc.done;
    void* context = read->desc.context;
    prMutexLock(&read->lock);
    read->done.store(true);
    prConditionBroadcast(&read->completed);
    prMutexUnlock(&read->lock);
    if (done != nullptr)
        done(context, read);
}

// ---------------------------------------------------------------------------------------------------------------------
// Sources that wrap a source

PrTextureConvertDesc prTextureConvertDescDefault(PrGpuFormat format) {
    return PrTextureConvertDesc{.format = format, .alpha = PrTextureAlpha_Straight, .channels = {0, 1, 2, 3}};
}

const PrTextureSource* prTextureSourceCreateConvert(const PrTextureSource* source, const PrTextureConvertDesc* desc) {
    const PrTextureDesc& innerDesc = *prTextureSourceDesc(source);
    for (uint8_t channel : desc->channels)
        PR_ASSERT(channel <= kPrTextureChannelOne);
    ConvertSource* convert = allocZeroed<ConvertSource>();
    prTextureSourceRetain(source);
    convert->inner = source;
    convert->innerDesc = innerDesc;
    convert->convert = *desc;
    PlainLayout in = plainLayout(innerDesc.format);
    PlainLayout out = plainLayout(desc->format);
    // Values change their encoding only in a conversion between 8 bits and floats, as they do when a sampler reads
    // them.
    convert->decodes = prGpuFormatIsSrgb(innerDesc.format) && out.bytes > 1;
    convert->converts = convert->decodes || (in.bytes > 1 && prGpuFormatIsSrgb(desc->format));
    // A transfer function applies to straight alpha. A conversion that changes the transfer therefore straightens
    // premultiplied values and then premultiplies them again.
    PrTextureAlpha innerAlpha = innerDesc.alpha;
    bool premultipliedIn = innerAlpha == PrTextureAlpha_Premultiplied;
    bool premultipliedOut = desc->alpha == PrTextureAlpha_Premultiplied;
    convert->straightens = premultipliedIn && (!premultipliedOut || convert->converts);
    convert->premultiplies = premultipliedOut && innerAlpha != PrTextureAlpha_None &&
        (!premultipliedIn || convert->converts);
    PrTextureDesc made = innerDesc;
    made.format = desc->format;
    made.alpha = desc->alpha;
    // A target of straight alpha over an opaque source stays opaque.
    if (innerAlpha == PrTextureAlpha_None && desc->alpha == PrTextureAlpha_Straight)
        made.alpha = PrTextureAlpha_None;
    convert->desc = made;
    bool sameBits = prGpuFormatPlain(prGpuFormatStored(innerDesc.format)) ==
        prGpuFormatPlain(prGpuFormatStored(desc->format));
    bool alphaKept = innerAlpha == made.alpha || innerAlpha == PrTextureAlpha_None;
    PrGpuSwizzle swizzle = prGpuFormatSwizzle(innerDesc.format);
    bool bytesSwizzle = swizzle == PrGpuSwizzle_None || swizzle == PrGpuSwizzle_Luminance ||
        swizzle == PrGpuSwizzle_Intensity;
    if (sameBits && swizzle == PrGpuSwizzle_None && identityChannels(desc->channels) && alphaKept)
        convert->mode = ConvertMode_Relabel;
    else if (in.bytes == 1 && out.bytes == 1 && bytesSwizzle)
        convert->mode = ConvertMode_Bytes;
    else
        convert->mode = ConvertMode_Numbers;
    PR_ASSERT(convert->mode == ConvertMode_Relabel || (isPlain(innerDesc.format) && isPlain(desc->format)));
    PrTextureSourceDesc wrapped{.texture = made, .read = convertRead, .release = convertRelease, .context = convert};
    // A relabel has the bits of its source. If the source fills textures, it can fill textures for the relabel too.
    if (convert->mode == ConvertMode_Relabel) {
        wrapped.flags = source->desc.flags & PrTextureSourceFlag_FillsTextures;
        wrapped.usage = source->desc.usage;
    }
    return prTextureSourceCreate(&wrapped);
}

const PrTextureSource* prTextureSourceCreateMips(const PrTextureSource* source, uint32_t levelCount) {
    const PrTextureDesc& innerDesc = *prTextureSourceDesc(source);
    PR_ASSERT(isPlain(innerDesc.format) && innerDesc.kind != PrGpuTextureKind_3d);
    uint32_t chain = prGpuMipLevels(innerDesc.extent);
    if (chain > kPrTextureMaxLevels)
        chain = kPrTextureMaxLevels;
    if (levelCount == 0 || levelCount > chain)
        levelCount = chain;
    if (levelCount < innerDesc.levelCount)
        levelCount = innerDesc.levelCount;
    MipsSource* mips = allocZeroed<MipsSource>();
    prTextureSourceRetain(source);
    mips->inner = source;
    mips->innerDesc = innerDesc;
    mips->desc = innerDesc;
    mips->desc.levelCount = levelCount;
    PrTextureSourceDesc wrapped{.texture = mips->desc, .read = mipsRead, .release = mipsRelease, .context = mips};
    return prTextureSourceCreate(&wrapped);
}

const PrTextureSource* PR_NULLABLE prTextureSourceCreateTiles(const PrTextureTilesDesc* desc) {
    PR_ASSERT(desc->grid.x != 0 && desc->grid.y != 0 && desc->maxExtent != 0);
    TilesSource* tiles = allocZeroed<TilesSource>();
    bool floats = false;
    bool alpha = false;
    PrUVec2 largest{1, 1};
    for (uint32_t i = 0; i < desc->tileCount; ++i) {
        const PrTextureTile& tile = desc->tiles[i];
        PR_ASSERT(tile.cell.x < desc->grid.x && tile.cell.y < desc->grid.y);
        const PrTextureSource* pixels = tilePixels(tile.source);
        if (pixels == nullptr)
            continue;
        const PrTextureDesc& pixelsDesc = *prTextureSourceDesc(pixels);
        floats = floats || plainLayout(pixelsDesc.format).bytes > 1;
        alpha = alpha || pixelsDesc.alpha != PrTextureAlpha_None;
        largest.x = largest.x > pixelsDesc.extent.x ? largest.x : pixelsDesc.extent.x;
        largest.y = largest.y > pixelsDesc.extent.y ? largest.y : pixelsDesc.extent.y;
        tiles->tiles.push(PrTextureTile{.source = pixels, .cell = tile.cell});
    }
    if (tiles->tiles.count == 0) {
        tilesRelease(tiles);
        return nullptr;
    }
    // The pixels of each tile, converted to the texture's format and alpha.
    PrGpuFormat format = floats ? PrGpuFormat_Rgba16F : PrGpuFormat_Rgba8;
    for (PrTextureTile& tile : tiles->tiles.span()) {
        PrTextureConvertDesc convert = prTextureConvertDescDefault(format);
        convert.alpha = PrTextureAlpha_Straight;
        const PrTextureSource* converted = prTextureSourceCreateConvert(tile.source, &convert);
        prTextureSourceRelease(tile.source);
        tile.source = converted;
    }
    while ((uint64_t(largest.x) * desc->grid.x > desc->maxExtent || uint64_t(largest.y) * desc->grid.y >
            desc->maxExtent) && (largest.x > 1 || largest.y > 1)) {
        largest.x = largest.x > 1 ? largest.x / 2 : 1;
        largest.y = largest.y > 1 ? largest.y / 2 : 1;
    }
    tiles->cellExtent = largest;
    PrTextureAlpha textureAlpha = alpha ? PrTextureAlpha_Straight : PrTextureAlpha_None;
    tiles->desc = prTextureDesc2d(format, PrUVec2{largest.x * desc->grid.x, largest.y * desc->grid.y}, textureAlpha);
    PrTextureSourceDesc wrapped{.texture = tiles->desc, .read = tilesRead, .release = tilesRelease, .context = tiles};
    return prTextureSourceCreate(&wrapped);
}

// ---------------------------------------------------------------------------------------------------------------------
// Format sets

void prTextureFormatDestroy(PrTextureFormat* PR_NULLABLE format) {
    if (format != nullptr && format->destroyFn != nullptr)
        format->destroyFn(format);
}

bool prTextureFormatSupportsType(const PrTextureFormat* format, PrStringView type) {
    return format->supportsTypeFn(format, type);
}

PrTextureFormatSet* prTextureFormatSetCreate(PrTextureFormat* PR_NONNULL const* PR_NULLABLE formats, uint32_t count) {
    PrTextureFormatSet* set = allocZeroed<PrTextureFormatSet>();
    for (uint32_t i = 0; i < count; ++i) {
        PR_ASSERT(formats[i] != nullptr);
        set->formats.push(formats[i]);
    }
    return set;
}

void prTextureFormatSetDestroy(PrTextureFormatSet* PR_NULLABLE set) {
    if (set == nullptr)
        return;
    for (PrTextureFormat* format : set->formats.span())
        prTextureFormatDestroy(format);
    set->formats.reset();
    prMemoryFree(set);
}

bool prTextureFormatSetSupportsType(const PrTextureFormatSet* set, PrStringView type) {
    for (const PrTextureFormat* format : set->formats.span()) {
        if (prTextureFormatSupportsType(format, type))
            return true;
    }
    return false;
}

void prTextureFormatSetOpen(const PrTextureFormatSet* set, const PrBlobSource* bytes, PrStringView name,
        const PrTextureLimits* PR_NULLABLE limits, PrTextureOpenFn done, void* PR_NULLABLE context) {
    TextureOpen* open = static_cast<TextureOpen*>(prMemoryAlloc(sizeof(TextureOpen) + name.size + 1));
    prBlobSourceRetain(bytes);
    *open = TextureOpen{
        .set = set,
        .bytes = bytes,
        .name = reinterpret_cast<char*>(open + 1),
        .nameSize = name.size,
        .limits = limits != nullptr ? *limits : prTextureLimitsDefault(),
        .done = done,
        .context = context,
    };
    if (name.size != 0)
        prMemCopy(open->name, name.data, name.size);
    PrBlobReadDesc read = prBlobReadDescDefault();
    read.done = [](void* PR_NULLABLE held, PrBlobRead* read) {
        TextureOpen* open = static_cast<TextureOpen*>(held);
        PrStatus status = prBlobReadStatus(read);
        const PrTextureSource* source = nullptr;
        if (status == PrStatus_Ok)
            status = textureOpen(*open, prBlobReadBlob(read), &source);
        prBlobReadDestroy(read);
        prBlobSourceRelease(open->bytes);
        PrTextureOpenFn opened = open->done;
        void* context = open->context;
        prMemoryFree(open);
        opened(context, status, status == PrStatus_Ok ? source : nullptr);
    };
    read.context = open;
    prBlobReadCreate(bytes, &read);
}

PrStatus prTextureFormatSetRead(const PrTextureFormatSet* set, const PrBlobSource* bytes, PrStringView name,
        const PrTextureLimits* PR_NULLABLE limits, const PrTextureSource* PR_NULLABLE* PR_NONNULL out) {
    TextureWait wait{};
    wait.init();
    prTextureFormatSetOpen(set, bytes, name, limits,
        [](void* PR_NULLABLE held, PrStatus status, const PrTextureSource* PR_NULLABLE source) {
            static_cast<TextureWait*>(held)->complete(status, source);
        }, &wait);
    wait.wait();
    *out = static_cast<const PrTextureSource*>(wait.result);
    return wait.status;
}

// ---------------------------------------------------------------------------------------------------------------------
// Textures

PrRect prTextureRegionRect(const PrTextureRegion* region, PrRect rect, PrTextureOrientationFlags orientation) {
    PrVec2 at{region->rect.left, region->rect.top};
    // The scale of each stored axis, and then of each displayed axis. A transposed picture swaps the two axes.
    PrVec2 scale{1, 1};
    if (region->source.x != 0 && region->source.y != 0) {
        scale.x = (region->rect.right - region->rect.left) / float(region->source.x);
        scale.y = (region->rect.bottom - region->rect.top) / float(region->source.y);
    }
    if ((orientation & PrTextureOrientationFlag_Transpose) != 0)
        scale = PrVec2{scale.y, scale.x};
    return PrRect{
        at.x + rect.left * scale.x, at.y + rect.top * scale.y, at.x + rect.right * scale.x, at.y + rect.bottom * scale.y,
    };
}

void prTextureUpload(PrGpuDevice* device, const PrTextureSource* source, PrTextureUploadFlags flags, PrStringView name,
        PrTextureUploadFn done, void* PR_NULLABLE context) {
    const PrTextureDesc& desc = source->desc.texture;
    // If the device cannot take the texture, nothing is read.
    PrStatus status = PrStatus_Ok;
    if (prGpuDeviceIsLost(device))
        status = PrStatus_Lost;
    else if (!prGpuDeviceSupportsFormat(device, desc.format, PrGpuTextureUsageFlag_Sampled | source->desc.usage))
        status = PrStatus_NotSupported;
    PrGpuTextureDesc gpu = uploadDesc(device, desc, flags);
    gpu.usage |= source->desc.usage;
    char label[64] = {};
    size_t length = name.size < sizeof(label) - 1 ? name.size : sizeof(label) - 1;
    if (length != 0)
        prMemCopy(label, name.data, length);
    gpu.name = label;
    PrGpuTexture texture{};
    if (status == PrStatus_Ok)
        status = prGpuTextureCreate(device, &gpu, &texture);
    if (status != PrStatus_Ok) {
        done(context, status, PrTexture{});
        return;
    }
    TextureUpload* upload = allocZeroed<TextureUpload>();
    *upload = TextureUpload{.device = device, .texture = texture, .gpu = gpu, .desc = desc, .done = done, .context = context};
    PrTextureReadDesc read = prTextureReadDescAll(&desc);
    read.device = device;
    read.texture = texture;
    read.done = [](void* PR_NULLABLE held, PrTextureRead* read) {
        TextureUpload* upload = static_cast<TextureUpload*>(held);
        PrStatus status = prTextureReadStatus(read);
        prTextureReadDestroy(read);
        PrGpuCommands* commands = nullptr;
        if (status == PrStatus_Ok && upload->gpu.levels > upload->desc.levelCount &&
                prGpuCommandsBegin(upload->device, nullptr, &commands)) {
            prGpuCommandsGenerateMipmaps(commands, upload->texture, PrGpuAccessFlag_ShaderRead);
            prGpuCommandsSubmit(commands);
        }
        PrTexture made{.texture = upload->texture, .alpha = upload->desc.alpha};
        if (status != PrStatus_Ok) {
            prGpuTextureRelease(upload->device, upload->texture);
            made = PrTexture{};
        }
        PrTextureUploadFn finished = upload->done;
        void* context = upload->context;
        prMemoryFree(upload);
        finished(context, status, made);
    };
    read.context = upload;
    prTextureReadCreate(source, &read);
}

PrStatus prTextureUploadRead(PrGpuDevice* device, const PrTextureSource* source, PrTextureUploadFlags flags,
        PrStringView name, PrTexture* out) {
    TextureWait wait{};
    wait.init();
    PrTexture made{};
    struct Held {
        TextureWait* wait;
        PrTexture* made;
    } held{&wait, &made};
    prTextureUpload(device, source, flags, name, [](void* PR_NULLABLE context, PrStatus status, PrTexture texture) {
        Held* h = static_cast<Held*>(context);
        *h->made = texture;
        h->wait->complete(status, nullptr);
    }, &held);
    wait.wait();
    *out = made;
    return wait.status;
}

uint64_t prTextureUploadSize(const PrGpuDevice* device, const PrTextureDesc* desc, PrTextureUploadFlags flags) {
    PrGpuTextureDesc gpu = uploadDesc(device, *desc, flags);
    PrTextureDesc all = *desc;
    all.levelCount = gpu.levels;
    uint64_t size = 0;
    for (uint32_t level = 0; level < all.levelCount; ++level)
        size += prTextureLevelSize(&all, level) * (desc->kind == PrGpuTextureKind_3d ? 1 : desc->layerCount);
    return size;
}

// ---------------------------------------------------------------------------------------------------------------------
// Helpers for sources

uint32_t prTexturePixelSize(PrTextureChannels channels, PrTextureSample sample) {
    return layoutOf(channels).count * sampleSize(sample);
}

void prTexturePixelsUnpack(PrPixelView stored, PrTextureChannels channels, PrTextureSample sample, PrGpuFormat format,
        PrPixels out) {
    PR_ASSERT(format == PrGpuFormat_R8 || format == PrGpuFormat_Rgba8 || format == PrGpuFormat_Rgba16F ||
        format == PrGpuFormat_Rgba32F);
    ChannelLayout layout = layoutOf(channels);
    uint32_t bytes = sampleSize(sample);
    uint32_t pixelBytes = layout.count * bytes;
    bool gray = channels == PrTextureChannels_Gray || channels == PrTextureChannels_GrayAlpha;
    if (format == PrGpuFormat_R8) {
        // The first channel alone, which is gray or red.
        for (uint32_t y = 0; y < stored.extent.y; ++y) {
            const uint8_t* row = stored.data + y * stored.stride;
            uint8_t* to = out.data + y * out.stride;
            for (uint32_t x = 0; x < stored.extent.x; ++x)
                to[x] = sampleU8(sample, row + size_t(x) * pixelBytes);
        }
        return;
    }
    if (format == PrGpuFormat_Rgba8 && sample == PrTextureSample_U8 && (channels == PrTextureChannels_Rgb ||
            channels == PrTextureChannels_Rgba)) {
        // 8-bit color channels. Alpha is opaque if the pixels store none.
        for (uint32_t y = 0; y < stored.extent.y; ++y) {
            const uint8_t* row = stored.data + y * stored.stride;
            uint8_t* to = out.data + y * out.stride;
            if (channels == PrTextureChannels_Rgba)
                prMemCopy(to, row, size_t(stored.extent.x) * 4);
            else
                expandRgbRow(row, to, stored.extent.x);
        }
        return;
    }
    // The stored channel that fills each channel of the texel. The entry is -1 for a channel that is not stored.
    int32_t sources[4] = {0, gray ? 0 : 1, gray ? 0 : 2, -1};
    if (channels == PrTextureChannels_Rg)
        sources[2] = -1;
    if (layout.alpha)
        sources[3] = int32_t(layout.count) - 1;
    for (uint32_t y = 0; y < stored.extent.y; ++y) {
        const uint8_t* row = stored.data + y * stored.stride;
        uint8_t* to = out.data + y * out.stride;
        for (uint32_t x = 0; x < stored.extent.x; ++x) {
            const uint8_t* from = row + size_t(x) * pixelBytes;
            for (uint32_t c = 0; c < 4; ++c) {
                const uint8_t* sampleBytes = sources[c] >= 0 ? from + uint32_t(sources[c]) * bytes : nullptr;
                if (format == PrGpuFormat_Rgba8) {
                    uint8_t value = c == 3 ? 255 : 0;
                    if (sampleBytes != nullptr)
                        value = sampleU8(sample, sampleBytes);
                    to[size_t(x) * 4 + c] = value;
                    continue;
                }
                float value = c == 3 ? 1.0f : 0.0f;
                if (sampleBytes != nullptr)
                    value = sampleValue(sample, sampleBytes);
                if (format == PrGpuFormat_Rgba16F) {
                    PrFloat16 half = prFloat16FromFloat(value);
                    prMemCopy(to + size_t(x) * 8 + c * 2, &half, 2);
                } else {
                    prMemCopy(to + size_t(x) * 16 + c * 4, &value, 4);
                }
            }
        }
    }
}
