// The BMP codec: lib/bmp's files as the image interface describes them.

#include "pr/image_bmp.h"

#include "pr/alloc.h"
#include "pr/bmp.h"
#include "pr/image_backend.h"

#include "image/backends/shared/src/image_shared.h"

namespace pr_impl {

namespace {

// An open file: the decoder, and the bytes of the file that it reads.
struct BmpReader : PrImageReader {
    uint8_t* bytes;
    PrBmpDecoder* bmp;
};

void close(PrImageReader* reader) {
    BmpReader* bmp = static_cast<BmpReader*>(reader);
    prBmpDecoderDestroy(bmp->bmp);
    prMemoryFree(bmp->bytes);
    prMemoryFree(bmp);
}

PrStatus frameInfo(PrImageReader*, uint32_t, PrImageFrameInfo* info) {
    info->durationMs = 0;
    return PrStatus_Ok;
}

PrStatus read(PrImageReader* reader, uint32_t, uint32_t, PrPixels output) {
    return prBmpDecoderRead(static_cast<BmpReader*>(reader)->bmp, output);
}

uint32_t headerBytes(const PrImageCodec*) {
    return kPrBmpSignatureBytes;
}

bool supportsType(const PrImageCodec*, PrStringView type) {
    return type == "image/bmp" || type == "image/x-bmp" || type == "image/x-ms-bmp";
}

bool recognize(const PrImageCodec*, PrByteView prefix) {
    return prBmpHasSignature(prefix);
}

// Returns the color of the pixels of a file of `info`.
PrImageColor colorOf(const PrBmpInfo& info) {
    switch (info.color) {
        case PrBmpColor_Srgb: return PrImageColor{.encoding = PrImageColorEncoding_Named, .space = PrColorSpace_Srgb};
        case PrBmpColor_Profile: return PrImageColor{.encoding = PrImageColorEncoding_Icc, .profile = info.profile};
        default: return PrImageColor{.encoding = PrImageColorEncoding_Unknown};
    }
}

PrStatus open(const PrImageCodec*, const PrImageCodecSet*, const PrImageSource* source, const PrImageLimits* limits,
        PrImageReader* PR_NULLABLE* PR_NONNULL out, PrImageInfo* info) {
    *out = nullptr;
    uint8_t* bytes = nullptr;
    size_t size = 0;
    PrStatus status = imageReadSource(source, *limits, sizeof(BmpReader), &bytes, &size);
    if (status != PrStatus_Ok)
        return status;
    const PrBmpLimits bmpLimits{
        .pixels = limits->pixels,
        .workingBytes = limits->workingBytes,
        .outputBytes = limits->outputBytes,
        .work = limits->work,
    };
    PrBmpDecoder* bmp = nullptr;
    status = prBmpDecoderCreate(PrByteView{bytes, size}, PrBmpSource_File, &bmpLimits, size + sizeof(BmpReader), &bmp);
    if (bmp == nullptr) {
        prMemoryFree(bytes);
        return status;
    }
    const PrBmpInfo& bmpInfo = *prBmpDecoderInfo(bmp);
    *info = PrImageInfo{
        .extent = bmpInfo.extent,
        .format = PrImageFormat{
            .model = PrColorModel_Rgb,
            .sample = PrImageSample_U8,
            .alpha = PrImageAlpha(bmpInfo.channels == 4 ? PrImageAlpha_Straight : PrImageAlpha_None),
            .color = colorOf(bmpInfo),
        },
        .pixelsPerMeter = bmpInfo.pixelsPerMeter,
        .frameCount = 1,
        .playCount = 1,
        .workingBytes = bmpInfo.workingBytes,
    };
    BmpReader* reader = allocZeroed<BmpReader>();
    reader->closeFn = close;
    reader->frameInfoFn = frameInfo;
    reader->readFn = read;
    reader->bytes = bytes;
    reader->bmp = bmp;
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

PrImageCodec* prImageBmpCreate(void) {
    return &gCodec;
}
