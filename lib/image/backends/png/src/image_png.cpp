// The PNG codec: lib/png's images as the image interface describes them.

#include "pr/image_png.h"

#include "pr/alloc.h"
#include "pr/image_backend.h"
#include "pr/png.h"

#include "image/backends/shared/src/image_shared.h"
#include "image/backends/shared/src/metadata.h"

namespace pr_impl {

namespace {

struct PngCodec : PrImageCodec {
    const PrCompressCodecSet* compress;
};

// An open PNG: the decoder, and the bytes of the file that it reads.
struct PngReader : PrImageReader {
    uint8_t* bytes;
    PrPngDecoder* png;
    PrStatus openStatus;    // Ok, or Truncated for a file that ends early
};

// Returns what the color of the image means. Code points name a space before a profile describes one, and an sRGB chunk
// names sRGB only without a profile (png-3/#11sRGB).
PrImageColor colorOf(const PrPngInfo& info) {
    PrColorSpace space = imageProfileCicpSpace(info.profile);
    if (info.hasCicp)
        space = imageCicpSpace(info.cicp.primaries, info.cicp.transfer, info.cicp.matrix, info.cicp.fullRange);
    if (space != PrColorSpace_Count)
        return PrImageColor{.encoding = PrImageColorEncoding_Named, .space = space};
    if (info.srgb)
        return PrImageColor{.encoding = PrImageColorEncoding_Named, .space = PrColorSpace_Srgb};
    if (info.profile.size != 0)
        return PrImageColor{.encoding = PrImageColorEncoding_Icc, .profile = info.profile};
    return PrImageColor{.encoding = PrImageColorEncoding_Unknown};
}

void close(PrImageReader* reader) {
    PngReader* png = static_cast<PngReader*>(reader);
    prPngDecoderDestroy(png->png);
    prMemoryFree(png->bytes);
    prMemoryFree(png);
}

PrStatus frameInfo(PrImageReader* reader, uint32_t frame, PrImageFrameInfo* info) {
    info->durationMs = prPngDecoderFrameDuration(static_cast<PngReader*>(reader)->png, frame);
    return PrStatus_Ok;
}

PrStatus read(PrImageReader* reader, uint32_t frame, uint32_t, PrPixels output) {
    PngReader* png = static_cast<PngReader*>(reader);
    // The cover frame is the same index in both.
    PrStatus status = prPngDecoderRead(png->png, frame, output);
    return status == PrStatus_Ok ? png->openStatus : status;
}

uint32_t headerBytes(const PrImageCodec*) {
    return kPrPngSignatureBytes;
}

bool supportsType(const PrImageCodec*, PrStringView type) {
    return type == "image/png" || type == "image/apng";
}

bool recognize(const PrImageCodec*, PrByteView prefix) {
    return prPngHasSignature(prefix);
}

// Returns the image interface's info of the PNG of `pngInfo`.
PrImageInfo infoOf(const PrPngInfo& pngInfo) {
    ImageMetadata exif{};
    if (pngInfo.exif.size != 0)
        imageReadExif(pngInfo.exif.data, pngInfo.exif.size, &exif);
    // pHYs gives the resolution before the Exif data does.
    PrVec2 pixelsPerMeter = pngInfo.pixelsPerMeter;
    if (pixelsPerMeter.x == 0)
        pixelsPerMeter = PrVec2{exif.pixelsPerMeterX, exif.pixelsPerMeterY};
    return PrImageInfo{
        .extent = pngInfo.extent,
        .format = PrImageFormat{
            .model = PrColorModel(pngInfo.channels <= 2 ? PrColorModel_Gray : PrColorModel_Rgb),
            .sample = PrImageSample(pngInfo.sampleBytes == 2 ? PrImageSample_U16 : PrImageSample_U8),
            .alpha = PrImageAlpha(pngInfo.alpha ? PrImageAlpha_Straight : PrImageAlpha_None),
            .color = colorOf(pngInfo),
        },
        .orientation = exif.orientation,
        .pixelsPerMeter = pixelsPerMeter,
        .frameCount = pngInfo.frameCount,
        .playCount = pngInfo.playCount,
        .workingBytes = pngInfo.workingBytes,
        .hasCover = pngInfo.hasCover,
        .exif = pngInfo.exif,
    };
}

// Returns the PNG limits of the image limits.
PrPngLimits pngLimitsOf(const PrImageLimits& limits) {
    return PrPngLimits{
        .pixels = limits.pixels,
        .workingBytes = limits.workingBytes,
        .outputBytes = limits.outputBytes,
        .work = limits.work,
        .frames = limits.frames,
        .metadataBytes = limits.metadataBytes,
    };
}

PrStatus append(PrImageReader* reader, PrByteView bytes, bool, PrImageInfo* info) {
    PngReader* png = static_cast<PngReader*>(reader);
    PrStatus status = prPngDecoderAppend(png->png, bytes);
    if (prPngDecoderInfo(png->png)->extent.x != 0)
        *info = infoOf(*prPngDecoderInfo(png->png));
    if (status == PrStatus_Ok)
        png->openStatus = PrStatus_Ok;
    return status;
}

uint64_t work(const PrImageReader* reader) {
    return prPngDecoderWork(static_cast<const PngReader*>(reader)->png);
}

PrStatus streamOpen(const PrImageCodec* imageCodec, const PrImageLimits* limits, PrImageReader* PR_NULLABLE* PR_NONNULL out) {
    const PrPngLimits pngLimits = pngLimitsOf(*limits);
    PrPngDecoder* png = nullptr;
    PrStatus status = prPngDecoderCreateStream(static_cast<const PngCodec*>(imageCodec)->compress, &pngLimits,
        sizeof(PngReader), &png);
    if (status != PrStatus_Ok)
        return status;
    PngReader* reader = allocZeroed<PngReader>();
    reader->closeFn = close;
    reader->frameInfoFn = frameInfo;
    reader->readFn = read;
    reader->appendFn = append;
    reader->workFn = work;
    reader->png = png;
    reader->openStatus = PrStatus_Truncated;
    *out = reader;
    return PrStatus_Ok;
}

PrStatus open(const PrImageCodec* imageCodec, const PrImageCodecSet*, const PrImageSource* source,
        const PrImageLimits* limits, PrImageReader* PR_NULLABLE* PR_NONNULL out, PrImageInfo* info) {
    *out = nullptr;
    uint8_t* bytes = nullptr;
    size_t size = 0;
    PrStatus status = imageReadSource(source, *limits, sizeof(PngReader), &bytes, &size);
    if (status != PrStatus_Ok)
        return status;
    const PrPngLimits pngLimits = pngLimitsOf(*limits);
    PrPngDecoder* png = nullptr;
    status = prPngDecoderCreate(PrByteView{bytes, size}, static_cast<const PngCodec*>(imageCodec)->compress,
        &pngLimits, size + sizeof(PngReader), &png);
    if (png == nullptr) {
        prMemoryFree(bytes);
        return status;
    }
    *info = infoOf(*prPngDecoderInfo(png));
    PngReader* reader = allocZeroed<PngReader>();
    reader->closeFn = close;
    reader->frameInfoFn = frameInfo;
    reader->readFn = read;
    reader->bytes = bytes;
    reader->png = png;
    reader->openStatus = status;
    *out = reader;
    return status;
}

PrStatus encode(const PrImageCodec* imageCodec, PrStringView type, PrPixelView pixels, uint8_t* PR_NULLABLE out,
        size_t capacity, size_t* size) {
    if (type != "image/png")
        return PrStatus_NotSupported;
    *size = prPngEncode(pixels, 0, static_cast<const PngCodec*>(imageCodec)->compress, out, capacity);
    return PrStatus_Ok;
}

void destroy(PrImageCodec* imageCodec) {
    prMemoryFree(imageCodec);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrImageCodec* prImagePngCreate(const PrCompressCodecSet* compress) {
    PngCodec* codec = allocZeroed<PngCodec>();
    codec->destroyFn = destroy;
    codec->headerBytesFn = headerBytes;
    codec->supportsTypeFn = supportsType;
    codec->recognizeFn = recognize;
    codec->decoderOpenFn = open;
    codec->streamOpenFn = streamOpen;
    codec->encodeFn = encode;
    codec->compress = compress;
    return codec;
}
