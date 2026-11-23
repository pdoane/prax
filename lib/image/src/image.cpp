// Image decoding and pixel representation.

#include "pr/image.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/image_backend.h"
#include "pr/mem.h"
#include "pr/raw_array.h"

#include "image/src/convert.h"

namespace pr_impl {

// The codecs of a set, in priority order, which the set owns.
struct ImageCodecSet {
    PrImageCodec** codecs;
    uint32_t count;
    uint32_t headerBytes;   // the most bytes that a codec of the set recognizes an image by
};

namespace {

// A decoder of one open image. Reads of the source use the prefix shown to the codecs instead of reading it again.
struct ImageDecoder {
    const PrImageCodecSet* set;
    PrImageReader* reader;
    PrImageSource original;
    PrImageSource source;
    PrImageLimits limits;
    PrImageInfo info;
    uint8_t* PR_NULLABLE prefix;
    size_t prefixSize;
    size_t prefixCapacity;

    // A stream of appended bytes (prImageDecoderOpenStream): the bytes kept until a codec recognizes them, and all of
    // them for a codec that decodes only whole files, the codec, and whether the image has been described.
    bool streaming;
    bool streamDescribed;
    PrStatus streamStatus;
    RawArray<uint8_t> streamBytes;
    const PrImageCodec* PR_NULLABLE streamCodec;
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
        prMemCopy(target, decoder->prefix + size_t(offset), copied);
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

// The most levels that an image has: those of a rip map of 2^31 pixels to a side.
constexpr uint32_t kMaxLevels = 32 * 32;

// Checks that the information a backend reported for an image is possible and within the limits. A reader that reports
// its levels' extents may have levels of any extent, and any other has levels that halve.
PrStatus validateInfo(const PrImageInfo& info, const PrImageLimits& limits, bool levelExtents) {
    size_t bytes = 0;
    const bool halving = info.levels == PrImageLevels_Mip && info.levelCount <= 32 &&
        (info.extent.x >> (info.levelCount - 1) != 0 || info.extent.y >> (info.levelCount - 1) != 0);
    const bool rip = info.levels == PrImageLevels_Rip && info.levelColumns != 0 && info.levelCount % info.levelColumns == 0;
    if (info.orientation > PrImageOrientation_Rotate270 || info.frameCount == 0 || info.levelCount == 0 ||
            info.levelCount > kMaxLevels || info.levels > PrImageLevels_Rip || (!levelExtents && !halving) ||
            (info.levels == PrImageLevels_Rip && !rip) || (info.levels == PrImageLevels_Mip && info.levelColumns != 1) ||
            (info.faceCount != 1 && info.faceCount != 6) || info.frameCount % info.faceCount != 0 ||
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

// Returns true if the reader's previews are valid. Each preview is no larger than the image on either axis and is
// smaller on at least one axis. The previews are ordered from the largest to the smallest.
bool validPreviews(const PrImageReader& reader, const PrImageInfo& info) {
    if (info.previewCount == 0)
        return true;
    if (reader.previewExtentFn == nullptr || reader.readPreviewFn == nullptr || info.previewCount > kMaxLevels)
        return false;
    PrUVec2 above = info.extent;
    for (uint32_t i = 0; i < info.previewCount; ++i) {
        PrUVec2 extent = reader.previewExtentFn(&reader, i);
        if (extent.x == 0 || extent.y == 0 || extent.x > above.x || extent.y > above.y ||
                (extent.x == above.x && extent.y == above.y))
            return false;
        above = extent;
    }
    return true;
}

bool hasFrame(const PrImageInfo& info, uint32_t frame) {
    return frame < info.frameCount || (frame == kPrImageFrameCover && info.hasCover);
}

PrUVec2 levelExtent(const ImageDecoder& data, uint32_t level) {
    if (data.reader->levelExtentFn != nullptr)
        return data.reader->levelExtentFn(data.reader, level);
    return prImageLevelExtent(data.info.extent, level);
}

// Reads one picture of the open image in the image's own format. The picture is the frame or the preview at `index`.
typedef PrStatus (*PictureReadFn)(PrImageDecoder* decoder, uint32_t index, PrPixels output);

// Decodes the picture that `read` reads at `index` as sRGB Rgba8 with `alpha`. The picture has `extent` pixels as
// stored. The function applies `orientation` to the stored pixels. `state` is the state of the decoder `handle`.
PrStatus readRgba8(PrImageDecoder* handle, const ImageDecoder& state, PictureReadFn read, uint32_t index, PrUVec2 extent,
        PrImageAlpha alpha, PrImageOrientation orientation, PrPixels output) {
    const PrImageInfo& info = state.info;
    PR_ASSERT(alpha == PrImageAlpha_Straight || alpha == PrImageAlpha_Premultiplied);
    PR_ASSERT(orientation <= PrImageOrientation_Rotate270);
    const bool swaps = prImageOrientationSwapsAxes(orientation);
    PR_ASSERT(output.extent.x == (swaps ? extent.y : extent.x));
    PR_ASSERT(output.extent.y == (swaps ? extent.x : extent.y));
    PR_ASSERT(size_t(output.extent.x) * 4 <= output.stride);
    PrImageFormat format{};
    format.model = PrColorModel_Rgb;
    format.sample = PrImageSample_U8;
    format.alpha = alpha;
    format.color.encoding = PrImageColorEncoding_Named;
    format.color.space = PrColorSpace_Srgb;
    size_t rgbaSize = 0;
    if (!packedBytes(format, extent.x, extent.y, &rgbaSize))
        return PrStatus_Limit;
    if (info.format.alpha == PrImageAlpha_Independent)
        return PrStatus_NotSupported;
    size_t nativeSize = 0;
    if (!packedBytes(info.format, extent.x, extent.y, &nativeSize))
        return PrStatus_InvalidData;
    bool orient = orientation != PrImageOrientation_Identity;
    uint64_t available = state.limits.workingBytes - info.workingBytes;
    if (rgbaSize > state.limits.outputBytes || nativeSize > available)
        return PrStatus_Limit;
    available -= nativeSize;
    if (orient && rgbaSize > available)
        return PrStatus_Limit;

    uint8_t* native = static_cast<uint8_t*>(prMemoryAlloc(nativeSize));
    const PrPixels stored{native, nativeSize / extent.y, extent};
    PrStatus status = read(handle, index, stored);
    if (!usable(status)) {
        prMemoryFree(native);
        return status;
    }
    bool truncated = status == PrStatus_Truncated;
    uint8_t* converted = orient ? static_cast<uint8_t*>(prMemoryAlloc(rgbaSize)) : nullptr;
    const PrPixels destination = orient ? PrPixels{converted, size_t(extent.x) * 4, extent} : output;
    status = imageConvert(prPixelsView(stored), info.format, destination, format,
        PrImageConversionFlag_AssumeSrgb | PrImageConversionFlag_ApproximateCmyk);
    if (usable(status) && orient)
        prImageOrient(PrPixelView{converted, size_t(extent.x) * 4, extent}, 4, orientation, output);
    if (orient)
        prMemoryFree(converted);
    prMemoryFree(native);
    return status == PrStatus_Ok && truncated ? PrStatus_Truncated : status;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrImageCodecSet : ImageCodecSet {};
struct PrImageDecoder : ImageDecoder {};

void prImageCodecDestroy(PrImageCodec* PR_NULLABLE codec) {
    if (codec != nullptr && codec->destroyFn != nullptr)
        codec->destroyFn(codec);
}

PrImageCodecSet* prImageCodecSetCreate(PrImageCodec* PR_NONNULL const* PR_NULLABLE codecs, uint32_t count) {
    PrImageCodecSet* set = allocZeroed<PrImageCodecSet>();
    set->codecs = count != 0 ? static_cast<PrImageCodec**>(prMemoryAlloc(sizeof(PrImageCodec*) * count)) : nullptr;
    for (uint32_t i = 0; i < count; ++i) {
        PR_ASSERT(codecs[i] != nullptr);
        set->codecs[i] = codecs[i];
        uint32_t headerBytes = codecs[i]->headerBytesFn(codecs[i]);
        set->headerBytes = headerBytes > set->headerBytes ? headerBytes : set->headerBytes;
    }
    set->count = count;
    return set;
}

void prImageCodecSetDestroy(PrImageCodecSet* PR_NULLABLE set) {
    if (set == nullptr)
        return;
    for (uint32_t i = 0; i < set->count; ++i)
        prImageCodecDestroy(set->codecs[i]);
    prMemoryFree(set->codecs);
    prMemoryFree(set);
}

bool prImageCodecSetSupportsType(const PrImageCodecSet* set, PrStringView type) {
    for (uint32_t i = 0; i < set->count; ++i) {
        if (set->codecs[i]->supportsTypeFn(set->codecs[i], type))
            return true;
    }
    return false;
}

PrStatus prImageCodecSetEncode(const PrImageCodecSet* set, PrStringView type, PrPixelView pixels,
        uint8_t* PR_NULLABLE out, size_t capacity, size_t* size) {
    PR_ASSERT(pixels.extent.x != 0 && pixels.extent.y != 0 && size_t(pixels.extent.x) * 4 <= pixels.stride);
    PR_ASSERT(out != nullptr || capacity == 0);
    *size = 0;
    for (uint32_t i = 0; i < set->count; ++i) {
        const PrImageCodec* codec = set->codecs[i];
        if (codec->encodeFn == nullptr)
            continue;
        PrStatus status = codec->encodeFn(codec, type, pixels, out, capacity, size);
        if (status != PrStatus_NotSupported)
            return status;
    }
    return PrStatus_NotSupported;
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
        prMemCopy(out, source->memory.data + size_t(offset), size);
        return PrStatus_Ok;
    }
    PrStatus status = source->readAt(source->stream.context, offset, out, size);
    return status == PrStatus_Ok ? status : PrStatus_Io;
}

PrImageDecoder* prImageDecoderCreate(const PrImageCodecSet* set) {
    PrImageDecoder* decoder = allocZeroed<PrImageDecoder>();
    decoder->set = set;
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
    const PrImageCodecSet* set = data->set;
    PrImageLimits budget = limits != nullptr ? *limits : prImageLimitsDefault();
    PR_ASSERT(source.readAt != nullptr || source.memory.data != nullptr || source.memory.size == 0);
    uint64_t size = prImageSourceSize(&source);
    if (size > budget.inputBytes)
        return PrStatus_Limit;
    if (size == 0)
        return PrStatus_NotSupported;
    uint32_t headerBytes = set->headerBytes;
    if (headerBytes > budget.headerBytes)
        return PrStatus_Limit;
    size_t prefixSize = size < headerBytes ? size_t(size) : headerBytes;
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
    const PrImageCodec* codec = nullptr;
    for (uint32_t i = 0; i < set->count && codec == nullptr; ++i) {
        if (set->codecs[i]->recognizeFn(set->codecs[i], PrByteView{data->prefix, prefixSize}))
            codec = set->codecs[i];
    }
    if (codec == nullptr)
        return PrStatus_NotSupported;
    data->original = source;
    data->limits = budget;
    data->limits.workingBytes -= overhead;
    data->prefixSize = prefixSize;
    data->source.readAt = readCached;
    data->source.stream.context = data;
    data->source.stream.size = size;
    status = codec->decoderOpenFn(codec, set, &data->source, &data->limits, &data->reader, &data->info);
    if (usable(status)) {
        // A reader that reports no levels or faces has one of each. The image is its own mip chain and is not a cube.
        if (data->info.levelCount == 0)
            data->info.levelCount = 1;
        if (data->info.levelColumns == 0)
            data->info.levelColumns = 1;
        if (data->info.faceCount == 0)
            data->info.faceCount = 1;
        PrStatus validation = PrStatus_InvalidData;
        if (data->reader != nullptr)
            validation = validateInfo(data->info, data->limits, data->reader->levelExtentFn != nullptr);
        if (validation == PrStatus_Ok && !validPreviews(*data->reader, data->info))
            validation = PrStatus_InvalidData;
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
        decoder->reader->closeFn(decoder->reader);
    decoder->reader = nullptr;
    decoder->info = PrImageInfo{};
    decoder->prefixSize = 0;
    decoder->streaming = false;
    decoder->streamDescribed = false;
    decoder->streamStatus = PrStatus_Ok;
    decoder->streamBytes.reset();
    decoder->streamCodec = nullptr;
}

PrStatus prImageDecoderOpenStream(PrImageDecoder* decoder, const PrImageLimits* PR_NULLABLE limits) {
    prImageDecoderClose(decoder);
    decoder->limits = limits != nullptr ? *limits : prImageLimitsDefault();
    decoder->streaming = true;
    decoder->streamStatus = PrStatus_Truncated;
    return PrStatus_Ok;
}

PrStatus prImageDecoderAppend(PrImageDecoder* decoder, PrByteView bytes, bool end) {
    ImageDecoder& d = *decoder;
    PR_ASSERT(d.streaming);
    if (d.streamStatus != PrStatus_Truncated)
        return d.streamStatus;
    if (d.streamBytes.count + bytes.size > d.limits.inputBytes) {
        d.streamStatus = PrStatus_Limit;
        return d.streamStatus;
    }
    PrByteView appended = bytes;
    // The first bytes are kept until they are enough for a codec to recognize, or are all that there are.
    if (d.streamCodec == nullptr) {
        if (bytes.size != 0)
            d.streamBytes.append(bytes.data, uint32_t(bytes.size));
        if (d.streamBytes.count < d.set->headerBytes && !end)
            return d.streamStatus;
        for (uint32_t i = 0; i < d.set->count && d.streamCodec == nullptr; ++i) {
            if (d.set->codecs[i]->recognizeFn(d.set->codecs[i], PrByteView{d.streamBytes.data, d.streamBytes.count}))
                d.streamCodec = d.set->codecs[i];
        }
        if (d.streamCodec == nullptr) {
            d.streamStatus = PrStatus_NotSupported;
            return d.streamStatus;
        }
        if (d.streamCodec->streamOpenFn != nullptr) {
            PrStatus status = d.streamCodec->streamOpenFn(d.streamCodec, &d.limits, &d.reader);
            if (status != PrStatus_Ok) {
                d.streamStatus = status;
                return status;
            }
            appended = PrByteView{d.streamBytes.data, d.streamBytes.count};
        }
    } else if (d.reader == nullptr && bytes.size != 0) {
        d.streamBytes.append(bytes.data, uint32_t(bytes.size));
    }
    PrStatus status = PrStatus_Truncated;
    if (d.reader != nullptr && d.reader->appendFn != nullptr) {
        PrImageInfo info = d.info;
        status = d.reader->appendFn(d.reader, appended, end, &info);
        if (appended.data == d.streamBytes.data)
            d.streamBytes.reset();
        if (usable(status) && info.extent.x != 0) {
            // A reader that reports no levels or faces has one of each.
            info.levelCount = info.levelCount != 0 ? info.levelCount : 1;
            info.levelColumns = info.levelColumns != 0 ? info.levelColumns : 1;
            info.faceCount = info.faceCount != 0 ? info.faceCount : 1;
            if (!d.streamDescribed && validateInfo(info, d.limits, false) != PrStatus_Ok)
                status = PrStatus_Limit;
            d.info = info;
            d.streamDescribed = true;
        }
    } else if (end) {
        // A codec that decodes only whole files opens the stream's bytes once they are all there.
        d.source = PrImageSource{};
        d.source.memory = PrByteView{d.streamBytes.data, d.streamBytes.count};
        status = d.streamCodec->decoderOpenFn(d.streamCodec, d.set, &d.source, &d.limits, &d.reader, &d.info);
        if (usable(status) && d.reader != nullptr) {
            d.info.levelCount = d.info.levelCount != 0 ? d.info.levelCount : 1;
            d.info.levelColumns = d.info.levelColumns != 0 ? d.info.levelColumns : 1;
            d.info.faceCount = d.info.faceCount != 0 ? d.info.faceCount : 1;
            d.streamDescribed = true;
        }
    }
    if (!usable(status) || status == PrStatus_Ok || end)
        d.streamStatus = status;
    return status;
}

uint64_t prImageDecoderWork(const PrImageDecoder* decoder) {
    if (decoder->reader != nullptr && decoder->reader->workFn != nullptr)
        return decoder->reader->workFn(decoder->reader);
    return decoder->streamBytes.count;
}

const PrImageInfo* prImageDecoderInfo(const PrImageDecoder* decoder) {
    return &decoder->info;
}

PrStatus prImageDecoderFrameInfo(PrImageDecoder* decoder, uint32_t frame, PrImageFrameInfo* out) {
    *out = PrImageFrameInfo{};
    if (!hasFrame(decoder->info, frame))
        return PrStatus_OutOfRange;
    return decoder->reader->frameInfoFn(decoder->reader, frame, out);
}

PrStatus prImageDecoderRead(PrImageDecoder* decoder, uint32_t frame, PrPixels output) {
    return prImageDecoderReadLevel(decoder, frame, 0, output);
}

PrUVec2 prImageLevelExtent(PrUVec2 extent, uint32_t level) {
    const PrUVec2 size{level < 32 ? extent.x >> level : 0, level < 32 ? extent.y >> level : 0};
    return PrUVec2{size.x != 0 ? size.x : 1, size.y != 0 ? size.y : 1};
}

PrUVec2 prImageDecoderLevelExtent(const PrImageDecoder* decoder, uint32_t level) {
    PR_ASSERT(decoder->reader != nullptr && level < decoder->info.levelCount);
    return levelExtent(*decoder, level);
}

PrStatus prImageDecoderReadLevel(PrImageDecoder* decoder, uint32_t frame, uint32_t level, PrPixels output) {
    if (!hasFrame(decoder->info, frame) || level >= decoder->info.levelCount)
        return PrStatus_OutOfRange;
    const PrUVec2 extent = levelExtent(*decoder, level);
    PR_ASSERT(output.extent.x == extent.x && output.extent.y == extent.y);
    PR_ASSERT(size_t(output.extent.x) * imagePixelBytes(decoder->info.format) <= output.stride);
    size_t required = 0;
    packedBytes(decoder->info.format, output.extent.x, output.extent.y, &required);
    if (required > decoder->limits.outputBytes)
        return PrStatus_Limit;
    return decoder->reader->readFn(decoder->reader, frame, level, output);
}

PrUVec2 prImageDecoderPreviewExtent(const PrImageDecoder* decoder, uint32_t preview) {
    PR_ASSERT(decoder->reader != nullptr && preview < decoder->info.previewCount);
    return decoder->reader->previewExtentFn(decoder->reader, preview);
}

bool prImageDecoderPreviewEmbedded(const PrImageDecoder* decoder, uint32_t preview) {
    PR_ASSERT(decoder->reader != nullptr && preview < decoder->info.previewCount);
    return decoder->reader->previewEmbeddedFn != nullptr && decoder->reader->previewEmbeddedFn(decoder->reader, preview);
}

PrStatus prImageDecoderReadPreview(PrImageDecoder* decoder, uint32_t preview, PrPixels output) {
    if (decoder->reader == nullptr || preview >= decoder->info.previewCount)
        return PrStatus_OutOfRange;
    PR_ASSERT(output.extent == prImageDecoderPreviewExtent(decoder, preview));
    PR_ASSERT(size_t(output.extent.x) * imagePixelBytes(decoder->info.format) <= output.stride);
    size_t required = 0;
    packedBytes(decoder->info.format, output.extent.x, output.extent.y, &required);
    if (required > decoder->limits.outputBytes)
        return PrStatus_Limit;
    return decoder->reader->readPreviewFn(decoder->reader, preview, output);
}

PrStatus prImageDecoderReadPreviewRgba8(PrImageDecoder* decoder, uint32_t preview, PrImageAlpha alpha,
        PrImageOrientation orientation, PrPixels output) {
    if (decoder->reader == nullptr || preview >= decoder->info.previewCount)
        return PrStatus_OutOfRange;
    PrUVec2 extent = prImageDecoderPreviewExtent(decoder, preview);
    return readRgba8(decoder, *decoder, prImageDecoderReadPreview, preview, extent, alpha, orientation, output);
}

PrStatus prImageDecoderReadRgba8(PrImageDecoder* decoder, uint32_t frame, PrImageAlpha alpha,
        PrImageOrientation orientation, PrPixels output) {
    if (!hasFrame(decoder->info, frame))
        return PrStatus_OutOfRange;
    return readRgba8(decoder, *decoder, prImageDecoderRead, frame, decoder->info.extent, alpha, orientation, output);
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
            prMemCopy(output.data + size_t(y) * output.stride + size_t(x) * pixelSize,
                source.data + size_t(sy) * source.stride + size_t(sx) * pixelSize, pixelSize);
        }
    }
}
