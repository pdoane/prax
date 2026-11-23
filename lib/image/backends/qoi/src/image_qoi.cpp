// The QOI codec: lib/qoi's images as the image interface describes them.

#include "pr/image_qoi.h"

#include "pr/alloc.h"
#include "pr/image_backend.h"
#include "pr/qoi.h"

#include "image/backends/shared/src/image_shared.h"

namespace pr_impl {

namespace {

// An open image: the decoder, and the bytes of the file that it reads.
struct QoiReader : PrImageReader {
    uint8_t* bytes;
    PrQoiDecoder* qoi;
};

void close(PrImageReader* reader) {
    QoiReader* qoi = static_cast<QoiReader*>(reader);
    prQoiDecoderDestroy(qoi->qoi);
    prMemoryFree(qoi->bytes);
    prMemoryFree(qoi);
}

PrStatus frameInfo(PrImageReader*, uint32_t, PrImageFrameInfo* info) {
    info->durationMs = 0;
    return PrStatus_Ok;
}

PrStatus read(PrImageReader* reader, uint32_t, uint32_t, PrPixels output) {
    return prQoiDecoderRead(static_cast<QoiReader*>(reader)->qoi, output);
}

uint32_t headerBytes(const PrImageCodec*) {
    return kPrQoiHeaderBytes;
}

bool supportsType(const PrImageCodec*, PrStringView type) {
    return type == "image/qoi";
}

bool recognize(const PrImageCodec*, PrByteView prefix) {
    return prQoiHasSignature(prefix);
}

PrStatus open(const PrImageCodec*, const PrImageCodecSet*, const PrImageSource* source, const PrImageLimits* limits,
        PrImageReader* PR_NULLABLE* PR_NONNULL out, PrImageInfo* info) {
    *out = nullptr;
    uint8_t* bytes = nullptr;
    size_t size = 0;
    PrStatus status = imageReadSource(source, *limits, sizeof(QoiReader), &bytes, &size);
    if (status != PrStatus_Ok)
        return status;
    const PrQoiLimits qoiLimits{
        .pixels = limits->pixels,
        .workingBytes = limits->workingBytes,
        .outputBytes = limits->outputBytes,
        .work = limits->work,
    };
    PrQoiDecoder* qoi = nullptr;
    status = prQoiDecoderCreate(PrByteView{bytes, size}, &qoiLimits, size + sizeof(QoiReader), &qoi);
    if (qoi == nullptr) {
        prMemoryFree(bytes);
        return status;
    }
    const PrQoiInfo& qoiInfo = *prQoiDecoderInfo(qoi);
    // The colorspace field says that the color is sRGB, with linear alpha, or that every channel is linear.
    const PrColorSpace space =
        qoiInfo.colorspace == PrQoiColorspace_Linear ? PrColorSpace_SrgbLinear : PrColorSpace_Srgb;
    *info = PrImageInfo{
        .extent = qoiInfo.extent,
        .format = PrImageFormat{
            .model = PrColorModel_Rgb,
            .sample = PrImageSample_U8,
            .alpha = PrImageAlpha(qoiInfo.channels == 4 ? PrImageAlpha_Straight : PrImageAlpha_None),
            .color = PrImageColor{.encoding = PrImageColorEncoding_Named, .space = space},
        },
        .frameCount = 1,
        .playCount = 1,
        .workingBytes = qoiInfo.workingBytes,
    };
    QoiReader* reader = allocZeroed<QoiReader>();
    reader->closeFn = close;
    reader->frameInfoFn = frameInfo;
    reader->readFn = read;
    reader->bytes = bytes;
    reader->qoi = qoi;
    *out = reader;
    return PrStatus_Ok;
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

PrImageCodec* prImageQoiCreate(void) {
    return &gCodec;
}
