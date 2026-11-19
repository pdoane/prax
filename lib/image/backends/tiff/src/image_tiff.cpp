// The TIFF codec: lib/tiff's images as the image interface describes them.

#include "pr/image_tiff.h"

#include "pr/alloc.h"
#include "pr/image_backend.h"
#include "pr/tiff.h"

#include "image/backends/shared/src/image_shared.h"

namespace pr_impl {

namespace {

// An open TIFF: the decoder, and the bytes of the file that it reads.
struct TiffReader : PrImageReader {
    uint8_t* bytes;
    PrTiffDecoder* tiff;
};

// The meaning of each extra sample, by PrTiffAlpha.
constexpr PrImageAlpha kAlphas[] = {
    PrImageAlpha_None,
    PrImageAlpha_Premultiplied,
    PrImageAlpha_Straight,
    PrImageAlpha_Independent,
};

void close(PrImageReader* reader) {
    TiffReader* tiff = static_cast<TiffReader*>(reader);
    prTiffDecoderDestroy(tiff->tiff);
    prMemoryFree(tiff->bytes);
    prMemoryFree(tiff);
}

PrStatus frameInfo(PrImageReader*, uint32_t, PrImageFrameInfo* info) {
    info->durationMs = 0;
    return PrStatus_Ok;
}

PrStatus read(PrImageReader* reader, uint32_t, uint32_t, PrPixels output) {
    return prTiffDecoderRead(static_cast<TiffReader*>(reader)->tiff, output);
}

uint32_t headerBytes(const PrImageCodec*) {
    return kPrTiffSignatureBytes;
}

bool supportsType(const PrImageCodec*, PrStringView type) {
    return type == "image/tiff";
}

bool recognize(const PrImageCodec*, PrByteView prefix) {
    return prTiffHasSignature(prefix);
}

PrStatus open(const PrImageCodec*, const PrImageCodecSet*, const PrImageSource* source, const PrImageLimits* limits,
        PrImageReader* PR_NULLABLE* PR_NONNULL out, PrImageInfo* info) {
    *out = nullptr;
    uint8_t* bytes = nullptr;
    size_t size = 0;
    PrStatus status = imageReadSource(source, *limits, sizeof(TiffReader), &bytes, &size);
    if (status != PrStatus_Ok)
        return status;
    const PrTiffLimits tiffLimits{
        .pixels = limits->pixels,
        .workingBytes = limits->workingBytes,
        .outputBytes = limits->outputBytes,
        .work = limits->work,
    };
    PrTiffDecoder* tiff = nullptr;
    status = prTiffDecoderCreate(PrByteView{bytes, size}, &tiffLimits, size + sizeof(TiffReader), &tiff);
    if (tiff == nullptr) {
        prMemoryFree(bytes);
        return status;
    }
    const PrTiffInfo& tiffInfo = *prTiffDecoderInfo(tiff);
    *info = PrImageInfo{
        .extent = tiffInfo.extent,
        .format = PrImageFormat{
            .model = PrColorModel(tiffInfo.gray ? PrColorModel_Gray : PrColorModel_Rgb),
            .sample = PrImageSample_U8,
            .alpha = kAlphas[tiffInfo.alpha],
            .color = PrImageColor{.encoding = PrImageColorEncoding_Unknown},
        },
        .frameCount = 1,
        .playCount = 1,
        .workingBytes = tiffInfo.workingBytes,
    };
    TiffReader* reader = allocZeroed<TiffReader>();
    reader->closeFn = close;
    reader->frameInfoFn = frameInfo;
    reader->readFn = read;
    reader->bytes = bytes;
    reader->tiff = tiff;
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

PrImageCodec* prImageTiffCreate(void) {
    return &gCodec;
}
