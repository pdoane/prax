// The Targa codec: lib/tga's images as the image interface describes them.

#include "pr/image_tga.h"

#include "pr/alloc.h"
#include "pr/image_backend.h"
#include "pr/tga.h"

#include "image/backends/shared/src/image_shared.h"

namespace pr_impl {

namespace {

// An open image: the decoder, and the bytes of the file that it reads.
struct TgaReader : PrImageReader {
    uint8_t* bytes;
    PrTgaDecoder* tga;
};

// The alpha of the decoded pixels, by PrTgaAlpha.
constexpr PrImageAlpha kAlphas[] = {PrImageAlpha_None, PrImageAlpha_Straight, PrImageAlpha_Premultiplied};

// Returns the orientation that an image's origin gives.
PrImageOrientation orientationOf(const PrTgaInfo& info) {
    if (info.fromRight && info.fromBottom)
        return PrImageOrientation_Rotate180;
    if (info.fromRight)
        return PrImageOrientation_FlipHorizontal;
    return info.fromBottom ? PrImageOrientation_FlipVertical : PrImageOrientation_Identity;
}

void close(PrImageReader* reader) {
    TgaReader* tga = static_cast<TgaReader*>(reader);
    prTgaDecoderDestroy(tga->tga);
    prMemoryFree(tga->bytes);
    prMemoryFree(tga);
}

PrStatus frameInfo(PrImageReader*, uint32_t, PrImageFrameInfo* info) {
    info->durationMs = 0;
    return PrStatus_Ok;
}

PrStatus read(PrImageReader* reader, uint32_t, uint32_t, PrPixels output) {
    return prTgaDecoderRead(static_cast<TgaReader*>(reader)->tga, output);
}

uint32_t headerBytes(const PrImageCodec*) {
    return kPrTgaHeaderBytes;
}

bool supportsType(const PrImageCodec*, PrStringView type) {
    return type == "image/x-tga";
}

bool recognize(const PrImageCodec*, PrByteView prefix) {
    return prTgaHasHeader(prefix);
}

PrStatus open(const PrImageCodec*, const PrImageCodecSet*, const PrImageSource* source, const PrImageLimits* limits,
        PrImageReader* PR_NULLABLE* PR_NONNULL out, PrImageInfo* info) {
    *out = nullptr;
    uint8_t* bytes = nullptr;
    size_t size = 0;
    PrStatus status = imageReadSource(source, *limits, sizeof(TgaReader), &bytes, &size);
    if (status != PrStatus_Ok)
        return status;
    const PrTgaLimits tgaLimits{
        .pixels = limits->pixels,
        .workingBytes = limits->workingBytes,
        .outputBytes = limits->outputBytes,
        .work = limits->work,
    };
    PrTgaDecoder* tga = nullptr;
    status = prTgaDecoderCreate(PrByteView{bytes, size}, &tgaLimits, size + sizeof(TgaReader), &tga);
    if (tga == nullptr) {
        prMemoryFree(bytes);
        return status;
    }
    const PrTgaInfo& tgaInfo = *prTgaDecoderInfo(tga);
    *info = PrImageInfo{
        .extent = tgaInfo.extent,
        .format = PrImageFormat{
            .model = PrColorModel(tgaInfo.gray ? PrColorModel_Gray : PrColorModel_Rgb),
            .sample = PrImageSample_U8,
            .alpha = kAlphas[tgaInfo.alpha],
            .color = PrImageColor{.encoding = PrImageColorEncoding_Unknown},
        },
        .orientation = orientationOf(tgaInfo),
        .frameCount = 1,
        .playCount = 1,
        .workingBytes = tgaInfo.workingBytes,
    };
    TgaReader* reader = allocZeroed<TgaReader>();
    reader->closeFn = close;
    reader->frameInfoFn = frameInfo;
    reader->readFn = read;
    reader->bytes = bytes;
    reader->tga = tga;
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

PrImageCodec* prImageTgaCreate(void) {
    return &gCodec;
}
