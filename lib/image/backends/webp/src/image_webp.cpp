// The WebP codec: lib/webp's images as the image interface describes them.

#include "pr/image_webp.h"

#include "pr/alloc.h"
#include "pr/image_backend.h"
#include "pr/webp.h"

#include "image/backends/shared/src/image_shared.h"
#include "image/backends/shared/src/metadata.h"

namespace pr_impl {

namespace {

// An open WebP: the decoder, and the bytes of the file that it reads.
struct WebpReader : PrImageReader {
    uint8_t* bytes;
    PrWebpDecoder* webp;
    PrStatus openStatus;    // Ok, or Truncated for a file that ends before its RIFF size, which every read reports
};

void close(PrImageReader* reader) {
    WebpReader* webp = static_cast<WebpReader*>(reader);
    prWebpDecoderDestroy(webp->webp);
    prMemoryFree(webp->bytes);
    prMemoryFree(webp);
}

PrStatus frameInfo(PrImageReader* reader, uint32_t frame, PrImageFrameInfo* info) {
    info->durationMs = prWebpDecoderFrameDuration(static_cast<WebpReader*>(reader)->webp, frame);
    return PrStatus_Ok;
}

PrStatus read(PrImageReader* reader, uint32_t frame, uint32_t, PrPixels output) {
    WebpReader* webp = static_cast<WebpReader*>(reader);
    const PrStatus status = prWebpDecoderRead(webp->webp, frame, output);
    return status == PrStatus_Ok ? webp->openStatus : status;
}

uint32_t headerBytes(const PrImageCodec*) {
    return kPrWebpSignatureBytes;
}

bool supportsType(const PrImageCodec*, PrStringView type) {
    return type == "image/webp";
}

bool recognize(const PrImageCodec*, PrByteView prefix) {
    return prWebpHasSignature(prefix);
}

PrStatus open(const PrImageCodec*, const PrImageCodecSet*, const PrImageSource* source, const PrImageLimits* limits,
        PrImageReader* PR_NULLABLE* PR_NONNULL out, PrImageInfo* info) {
    *out = nullptr;
    uint8_t* bytes = nullptr;
    size_t size = 0;
    PrStatus status = imageReadSource(source, *limits, sizeof(WebpReader), &bytes, &size);
    if (status != PrStatus_Ok)
        return status;
    const PrWebpLimits webpLimits{
        .inputBytes = limits->inputBytes,
        .pixels = limits->pixels,
        .workingBytes = limits->workingBytes,
        .outputBytes = limits->outputBytes,
        .work = limits->work,
        .frames = limits->frames,
        .metadataBytes = limits->metadataBytes,
    };
    PrWebpDecoder* webp = nullptr;
    status = prWebpDecoderCreate(PrByteView{bytes, size}, &webpLimits, size + sizeof(WebpReader), &webp);
    if (webp == nullptr) {
        prMemoryFree(bytes);
        return status;
    }
    const PrWebpInfo& webpInfo = *prWebpDecoderInfo(webp);
    // A file that ends before its first frame is whole has no frame to read.
    if (webpInfo.frameCount == 0) {
        prWebpDecoderDestroy(webp);
        prMemoryFree(bytes);
        return PrStatus_Truncated;
    }
    ImageMetadata exif{};
    if (webpInfo.exif.size != 0)
        imageReadExif(webpInfo.exif.data, webpInfo.exif.size, &exif);
    PrImageColor color{.encoding = PrImageColorEncoding_Named, .space = PrColorSpace_Srgb};
    if (webpInfo.profile.size != 0)
        color = PrImageColor{.encoding = PrImageColorEncoding_Icc, .profile = webpInfo.profile};
    *info = PrImageInfo{
        .extent = webpInfo.extent,
        .format = PrImageFormat{
            .model = PrColorModel_Rgb,
            .sample = PrImageSample_U8,
            .alpha = PrImageAlpha(webpInfo.alpha ? PrImageAlpha_Straight : PrImageAlpha_None),
            .color = color,
        },
        .orientation = exif.orientation,
        .pixelsPerMeter = PrVec2{exif.pixelsPerMeterX, exif.pixelsPerMeterY},
        .frameCount = webpInfo.frameCount,
        .playCount = webpInfo.playCount,
        .workingBytes = webpInfo.workingBytes,
    };
    WebpReader* reader = allocZeroed<WebpReader>();
    reader->closeFn = close;
    reader->frameInfoFn = frameInfo;
    reader->readFn = read;
    reader->bytes = bytes;
    reader->webp = webp;
    reader->openStatus = status;
    *out = reader;
    return status;
}

// The codec has no state, so one serves every call and nothing is destroyed.
PrImageCodec gCodec = {
    .headerBytesFn = headerBytes,
    .supportsTypeFn = supportsType,
    .recognizeFn = recognize,
    .decoderOpenFn = open,
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrImageCodec* prImageWebpCreate(void) {
    return &gCodec;
}
