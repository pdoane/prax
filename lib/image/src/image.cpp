// Image decoding and pixel representation.

#include "pr/image.h"

#include "pr/alloc.h"
#include "pr/ascii.h"
#include "pr/assert.h"
#include "pr/mem.h"

#include "image/src/convert.h"

#if defined(PR_IMAGE_HAS_BUILTIN)
#include "pr/image_builtin.h"
#endif

namespace pr_impl {

namespace {

constexpr PrStringView kBackendNames[PrImageCodecBackend_Count] = {"default", "builtin"};

// The backends that the build has, which Default creates the first of, and then Default, so that a build without
// backends creates none.
constexpr PrImageCodecBackend kBuilt[] = {
#if defined(PR_IMAGE_HAS_BUILTIN)
    PrImageCodecBackend_Builtin,
#endif
    PrImageCodecBackend_Default,
};

// A decoder of one open image. Reads of the source use the prefix shown to the backend instead of reading it again.
struct ImageDecoder {
    PrImageCodec* codec;
    PrImageReader* reader;
    PrImageSource original;
    PrImageSource source;
    PrImageLimits limits;
    PrImageInfo info;
    uint8_t* PR_NULLABLE prefix;
    size_t prefixSize;
    size_t prefixCapacity;
};

bool usable(PrStatus status) {
    return status == PrStatus_Ok || status == PrStatus_Truncated;
}

PrStatus readCached(void* PR_NULLABLE context, uint64_t offset, void* out, size_t size) {
    ImageDecoder* decoder = static_cast<ImageDecoder*>(context);
    uint8_t* target = static_cast<uint8_t*>(out);
    if (offset < decoder->prefixSize) {
        size_t remaining = decoder->prefixSize - size_t(offset);
        size_t copied = size < remaining ? size : remaining;
        memCopy(target, decoder->prefix + size_t(offset), copied);
        offset += copied;
        size -= copied;
        target += copied;
    }
    return size == 0 ? PrStatus_Ok : prImageSourceRead(&decoder->original, offset, target, size);
}

// Computes the number of bytes of packed pixels of the format and size. Returns false for a format that is not valid,
// an empty size, or a size whose byte count overflows.
bool packedBytes(const PrImageFormat& format, uint32_t width, uint32_t height, size_t* out) {
    *out = 0;
    const uint32_t pixel = imagePixelBytes(format);
    if (pixel == 0 || width == 0 || height == 0 || uint64_t(width) * pixel > SIZE_MAX / height)
        return false;
    *out = size_t(width) * pixel * height;
    return true;
}

// Checks that the information a backend reported for an image is possible and within the limits.
PrStatus validateInfo(const PrImageInfo& info, const PrImageLimits& limits) {
    size_t bytes = 0;
    if (info.orientation > PrImageOrientation_Rotate270 || info.frameCount == 0 ||
            !(info.pixelsPerMeter.x >= 0 && info.pixelsPerMeter.x <= 1e30f) ||
            !(info.pixelsPerMeter.y >= 0 && info.pixelsPerMeter.y <= 1e30f) ||
            !packedBytes(info.format, info.extent.x, info.extent.y, &bytes))
        return PrStatus_InvalidData;
    if (info.frameCount > limits.frames || uint64_t(info.extent.x) * info.extent.y > limits.pixels ||
            info.workingBytes > limits.workingBytes || (info.format.color.encoding == PrImageColorEncoding_Icc &&
                info.format.color.profile.size > limits.metadataBytes) || bytes > limits.outputBytes)
        return PrStatus_Limit;
    return PrStatus_Ok;
}

bool hasFrame(const PrImageInfo& info, uint32_t frame) {
    return frame < info.frameCount || (frame == kPrImageFrameCover && info.hasCover);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrImageCodecBackend prImageCodecBackendFromName(PrStringView name) {
    for (PrImageCodecBackend backend = 0; backend < PrImageCodecBackend_Count; ++backend) {
        if (asciiEqualsFold(kBackendNames[backend], name))
            return backend;
    }
    return PrImageCodecBackend_Count;
}

PrStringView prImageCodecBackendName(PrImageCodecBackend backend) {
    return backend < PrImageCodecBackend_Count ? kBackendNames[backend] : PrStringView{};
}

bool prImageCodecBackendIsBuilt(PrImageCodecBackend backend) {
    for (PrImageCodecBackend built : kBuilt) {
        if (built != PrImageCodecBackend_Default && (built == backend || backend == PrImageCodecBackend_Default))
            return true;
    }
    return false;
}

PrImageCodec* PR_NULLABLE prImageCodecCreate(PrImageCodecBackend backend, PrCompressCodec* compressCodec) {
    switch (backend == PrImageCodecBackend_Default ? kBuilt[0] : backend) {
#if defined(PR_IMAGE_HAS_BUILTIN)
        case PrImageCodecBackend_Builtin: return prImageBuiltinCreate(compressCodec);
#endif
        default: (void)compressCodec; return nullptr;
    }
}

// C opaque types
struct PrImageDecoder : ImageDecoder {};

void prImageCodecDestroy(PrImageCodec* PR_NULLABLE codec) {
    if (codec != nullptr && codec->destroy != nullptr)
        codec->destroy(codec);
}

bool prImageCodecSupportsType(PrImageCodec* codec, PrStringView type) {
    return codec->supportsType(codec, type);
}

PrStatus prImageCodecEncode(PrImageCodec* codec, PrStringView type, PrPixelView pixels, uint8_t* PR_NULLABLE out,
        size_t capacity, size_t* size) {
    PR_ASSERT(pixels.extent.x != 0 && pixels.extent.y != 0 && size_t(pixels.extent.x) * 4 <= pixels.stride);
    PR_ASSERT(out != nullptr || capacity == 0);
    *size = 0;
    if (codec->encode == nullptr)
        return PrStatus_NotSupported;
    return codec->encode(codec, type, pixels, out, capacity, size);
}

PrImageLimits prImageLimitsDefault(void) {
    return PrImageLimits{
        256u * 1024u * 1024u, 64u * 1024u * 1024u, 512u * 1024u * 1024u,
        512u * 1024u * 1024u, 1000000000u,         4096,
        16u * 1024u * 1024u,  64u * 1024u,
    };
}

uint64_t prImageSourceSize(const PrImageSource* source) {
    return source->readAt == nullptr ? source->memory.size : source->stream.size;
}

PrStatus prImageSourceRead(const PrImageSource* source, uint64_t offset, void* PR_NULLABLE out, size_t size) {
    PR_ASSERT(out != nullptr || size == 0);
    uint64_t length = prImageSourceSize(source);
    if (offset > length || uint64_t(size) > length - offset)
        return PrStatus_Io;
    if (size == 0)
        return PrStatus_Ok;
    if (source->readAt == nullptr) {
        memCopy(out, source->memory.data + size_t(offset), size);
        return PrStatus_Ok;
    }
    PrStatus status = source->readAt(source->stream.context, offset, out, size);
    return status == PrStatus_Ok ? status : PrStatus_Io;
}

PrImageDecoder* prImageDecoderCreate(PrImageCodec* codec) {
    PrImageDecoder* decoder = allocZeroed<PrImageDecoder>();
    decoder->codec = codec;
    return decoder;
}

void prImageDecoderDestroy(PrImageDecoder* PR_NULLABLE decoder) {
    if (decoder == nullptr)
        return;
    prImageDecoderClose(decoder);
    prMemoryFree(decoder->prefix);
    prMemoryFree(decoder);
}

PrStatus prImageDecoderOpen(PrImageDecoder* decoder, PrImageSource source, const PrImageLimits* PR_NULLABLE limits) {
    ImageDecoder* data = decoder;
    prImageDecoderClose(decoder);
    PrImageCodec* codec = data->codec;
    PrImageLimits budget = limits != nullptr ? *limits : prImageLimitsDefault();
    PR_ASSERT(source.readAt != nullptr || source.memory.data != nullptr || source.memory.size == 0);
    uint64_t size = prImageSourceSize(&source);
    if (size > budget.inputBytes)
        return PrStatus_Limit;
    if (size == 0)
        return PrStatus_NotSupported;
    if (codec->headerBytes > budget.headerBytes)
        return PrStatus_Limit;
    size_t prefixSize = size < codec->headerBytes ? size_t(size) : codec->headerBytes;
    uint64_t overhead = sizeof(ImageDecoder) + uint64_t(prefixSize);
    if (overhead > budget.workingBytes)
        return PrStatus_Limit;
    if (prefixSize > data->prefixCapacity) {
        prMemoryFree(data->prefix);
        data->prefix = static_cast<uint8_t*>(prMemoryAlloc(prefixSize));
        data->prefixCapacity = prefixSize;
    }
    PrStatus status = prImageSourceRead(&source, 0, data->prefix, prefixSize);
    if (status != PrStatus_Ok)
        return status;
    if (!codec->recognize(codec, PrByteView{data->prefix, prefixSize}))
        return PrStatus_NotSupported;
    data->original = source;
    data->limits = budget;
    data->limits.workingBytes -= overhead;
    data->prefixSize = prefixSize;
    data->source.readAt = readCached;
    data->source.stream.context = data;
    data->source.stream.size = size;
    status = codec->decoderOpen(codec, &data->source, &data->limits, &data->reader, &data->info);
    if (usable(status)) {
        PrStatus validation = validateInfo(data->info, data->limits);
        if (data->reader == nullptr)
            status = PrStatus_InvalidData;
        else if (validation != PrStatus_Ok)
            status = validation;
    }
    if (!usable(status))
        prImageDecoderClose(decoder);
    return status;
}

void prImageDecoderClose(PrImageDecoder* decoder) {
    if (decoder->reader != nullptr)
        decoder->codec->decoderClose(decoder->codec, decoder->reader);
    decoder->reader = nullptr;
    decoder->info = PrImageInfo{};
    decoder->prefixSize = 0;
}

const PrImageInfo* prImageDecoderInfo(const PrImageDecoder* decoder) {
    return &decoder->info;
}

PrStatus prImageDecoderFrameInfo(PrImageDecoder* decoder, uint32_t frame, PrImageFrameInfo* out) {
    *out = PrImageFrameInfo{};
    if (!hasFrame(decoder->info, frame))
        return PrStatus_OutOfRange;
    return decoder->codec->decoderFrameInfo(decoder->codec, decoder->reader, frame, out);
}

PrStatus prImageDecoderRead(PrImageDecoder* decoder, uint32_t frame, PrPixels output) {
    PR_ASSERT(output.extent.x == decoder->info.extent.x && output.extent.y == decoder->info.extent.y);
    PR_ASSERT(size_t(output.extent.x) * imagePixelBytes(decoder->info.format) <= output.stride);
    if (!hasFrame(decoder->info, frame))
        return PrStatus_OutOfRange;
    size_t required = 0;
    packedBytes(decoder->info.format, output.extent.x, output.extent.y, &required);
    if (required > decoder->limits.outputBytes)
        return PrStatus_Limit;
    return decoder->codec->decoderRead(decoder->codec, decoder->reader, frame, output);
}

PrStatus prImageDecoderReadRgba8(PrImageDecoder* decoder, uint32_t frame, PrImageAlpha alpha,
        PrImageOrientation orientation, PrPixels output) {
    const PrImageInfo& info = decoder->info;
    PR_ASSERT(alpha == PrImageAlpha_Straight || alpha == PrImageAlpha_Premultiplied);
    PR_ASSERT(orientation <= PrImageOrientation_Rotate270);
    const bool swaps = prImageOrientationSwapsAxes(orientation);
    PR_ASSERT(output.extent.x == (swaps ? info.extent.y : info.extent.x));
    PR_ASSERT(output.extent.y == (swaps ? info.extent.x : info.extent.y));
    PR_ASSERT(size_t(output.extent.x) * 4 <= output.stride);
    PrImageFormat format{};
    format.model = PrColorModel_Rgb;
    format.sample = PrImageSample_U8;
    format.alpha = alpha;
    format.color.encoding = PrImageColorEncoding_Named;
    format.color.space = PrColorSpace_Srgb;
    size_t rgbaSize = 0;
    if (!packedBytes(format, info.extent.x, info.extent.y, &rgbaSize))
        return PrStatus_Limit;
    if (!hasFrame(info, frame))
        return PrStatus_OutOfRange;
    if (info.format.alpha == PrImageAlpha_Independent)
        return PrStatus_NotSupported;
    size_t nativeSize = 0;
    if (!packedBytes(info.format, info.extent.x, info.extent.y, &nativeSize))
        return PrStatus_InvalidData;
    bool orient = orientation != PrImageOrientation_Identity;
    uint64_t available = decoder->limits.workingBytes - info.workingBytes;
    if (rgbaSize > decoder->limits.outputBytes || nativeSize > available)
        return PrStatus_Limit;
    available -= nativeSize;
    if (orient && rgbaSize > available)
        return PrStatus_Limit;

    uint8_t* native = static_cast<uint8_t*>(prMemoryAlloc(nativeSize));
    const PrPixels stored{native, nativeSize / info.extent.y, info.extent};
    PrStatus status = prImageDecoderRead(decoder, frame, stored);
    if (!usable(status)) {
        prMemoryFree(native);
        return status;
    }
    bool truncated = status == PrStatus_Truncated;
    uint8_t* converted = orient ? static_cast<uint8_t*>(prMemoryAlloc(rgbaSize)) : nullptr;
    const PrPixels destination = orient ? PrPixels{converted, size_t(info.extent.x) * 4, info.extent} : output;
    status = imageConvert(prPixelsView(stored), info.format, destination, format,
        PrImageConversionFlag_AssumeSrgb | PrImageConversionFlag_ApproximateCmyk);
    if (usable(status) && orient)
        prImageOrient(PrPixelView{converted, size_t(info.extent.x) * 4, info.extent}, 4, orientation, output);
    if (orient)
        prMemoryFree(converted);
    prMemoryFree(native);
    return status == PrStatus_Ok && truncated ? PrStatus_Truncated : status;
}

uint32_t prImageFormatSize(const PrImageFormat* format) {
    return imagePixelBytes(*format);
}

PrStatus prImageConvert(PrPixelView source, const PrImageFormat* sourceFormat, PrPixels destination,
        const PrImageFormat* destinationFormat, PrImageConversionFlags flags) {
    return imageConvert(source, *sourceFormat, destination, *destinationFormat, flags);
}

bool prImageOrientationSwapsAxes(PrImageOrientation orientation) {
    return orientation >= PrImageOrientation_Transpose;
}

PrImageOrientation prImageOrientationInverse(PrImageOrientation orientation) {
    PR_ASSERT(orientation <= PrImageOrientation_Rotate270);
    switch (orientation) {
        case PrImageOrientation_Rotate90: return PrImageOrientation_Rotate270;
        case PrImageOrientation_Rotate270: return PrImageOrientation_Rotate90;
        default: return orientation;
    }
}

void prImageOrient(PrPixelView source, uint32_t pixelSize, PrImageOrientation orientation, PrPixels output) {
    PR_ASSERT(source.data != output.data && orientation <= PrImageOrientation_Rotate270);
    uint32_t width = source.extent.x;
    uint32_t height = source.extent.y;
    bool swaps = prImageOrientationSwapsAxes(orientation);
    PR_ASSERT(output.extent.x == (swaps ? height : width) && output.extent.y == (swaps ? width : height));
    for (uint32_t y = 0; y < output.extent.y; ++y) {
        for (uint32_t x = 0; x < output.extent.x; ++x) {
            uint32_t sx = x;
            uint32_t sy = y;
            switch (orientation) {
                case PrImageOrientation_FlipHorizontal:
                    sx = width - 1 - x;
                    break;
                case PrImageOrientation_Rotate180:
                    sx = width - 1 - x;
                    sy = height - 1 - y;
                    break;
                case PrImageOrientation_FlipVertical:
                    sy = height - 1 - y;
                    break;
                case PrImageOrientation_Transpose:
                    sx = y;
                    sy = x;
                    break;
                case PrImageOrientation_Rotate90:
                    sx = y;
                    sy = height - 1 - x;
                    break;
                case PrImageOrientation_Transverse:
                    sx = width - 1 - y;
                    sy = height - 1 - x;
                    break;
                case PrImageOrientation_Rotate270:
                    sx = width - 1 - y;
                    sy = x;
                    break;
                default:
                    break;
            }
            memCopy(output.data + size_t(y) * output.stride + size_t(x) * pixelSize,
                source.data + size_t(sy) * source.stride + size_t(sx) * pixelSize, pixelSize);
        }
    }
}
