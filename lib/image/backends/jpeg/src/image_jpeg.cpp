// The JPEG codec: lib/jpeg's images as the image interface describes them.

#include "pr/image_jpeg.h"

#include "pr/alloc.h"
#include "pr/image_backend.h"
#include "pr/jpeg.h"

#include "image/backends/shared/src/image_shared.h"
#include "image/backends/shared/src/metadata.h"

namespace pr_impl {

namespace {

// An open JPEG: the decoder, and the bytes of the file that it reads.
struct JpegReader : PrImageReader {
    uint8_t* bytes;
    PrJpegDecoder* jpeg;
    PrStatus openStatus;    // Ok, or Truncated for a file that ends early
};

void close(PrImageReader* reader) {
    JpegReader* jpeg = static_cast<JpegReader*>(reader);
    prJpegDecoderDestroy(jpeg->jpeg);
    prMemoryFree(jpeg->bytes);
    prMemoryFree(jpeg);
}

PrStatus frameInfo(PrImageReader*, uint32_t, PrImageFrameInfo* info) {
    info->durationMs = 0;
    return PrStatus_Ok;
}

PrStatus read(PrImageReader* reader, uint32_t, uint32_t, PrPixels output) {
    JpegReader* jpeg = static_cast<JpegReader*>(reader);
    PrStatus status = prJpegDecoderRead(jpeg->jpeg, output);
    return status == PrStatus_Ok ? jpeg->openStatus : status;
}

uint32_t headerBytes(const PrImageCodec*) {
    return kPrJpegSignatureBytes;
}

bool supportsType(const PrImageCodec*, PrStringView type) {
    return type == "image/jpeg";
}

bool recognize(const PrImageCodec*, PrByteView prefix) {
    return prJpegHasSignature(prefix);
}

PrStatus open(const PrImageCodec*, const PrImageCodecSet*, const PrImageSource* source, const PrImageLimits* limits,
        PrImageReader* PR_NULLABLE* PR_NONNULL out, PrImageInfo* info) {
    *out = nullptr;
    uint8_t* bytes = nullptr;
    size_t size = 0;
    PrStatus status = imageReadSource(source, *limits, sizeof(JpegReader), &bytes, &size);
    if (status != PrStatus_Ok)
        return status;
    const PrJpegLimits jpegLimits{
        .pixels = limits->pixels,
        .workingBytes = limits->workingBytes,
        .outputBytes = limits->outputBytes,
        .work = limits->work,
        .metadataBytes = limits->metadataBytes,
    };
    PrJpegDecoder* jpeg = nullptr;
    status = prJpegDecoderCreate(PrByteView{bytes, size}, &jpegLimits, size + sizeof(JpegReader), &jpeg);
    if (jpeg == nullptr) {
        prMemoryFree(bytes);
        return status;
    }
    const PrJpegInfo& jpegInfo = *prJpegDecoderInfo(jpeg);
    ImageMetadata exif{};
    if (jpegInfo.exif.size != 0)
        imageReadExif(jpegInfo.exif.data, jpegInfo.exif.size, &exif);
    // The Exif data gives the resolution before the JFIF density does.
    PrVec2 pixelsPerMeter = jpegInfo.pixelsPerMeter;
    if (exif.pixelsPerMeterX != 0)
        pixelsPerMeter = PrVec2{exif.pixelsPerMeterX, exif.pixelsPerMeterY};
    PrColorModel model = PrColorModel_Rgb;
    if (jpegInfo.components == 1)
        model = PrColorModel_Gray;
    else if (jpegInfo.components == 4)
        model = PrColorModel_Cmyk;
    *info = PrImageInfo{
        .extent = jpegInfo.extent,
        .format = PrImageFormat{
            .model = model,
            .sample = PrImageSample_U8,
            .alpha = PrImageAlpha_None,
            .color = PrImageColor{.encoding = PrImageColorEncoding_Unknown},
        },
        .orientation = exif.orientation,
        .pixelsPerMeter = pixelsPerMeter,
        .frameCount = 1,
        .playCount = 1,
        .workingBytes = jpegInfo.workingBytes,
    };
    JpegReader* reader = allocZeroed<JpegReader>();
    reader->closeFn = close;
    reader->frameInfoFn = frameInfo;
    reader->readFn = read;
    reader->bytes = bytes;
    reader->jpeg = jpeg;
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

PrImageCodec* prImageJpegCreate(void) {
    return &gCodec;
}
