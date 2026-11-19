// The GIF codec: lib/gif's images as the image interface describes them.

#include "pr/image_gif.h"

#include "pr/alloc.h"
#include "pr/gif.h"
#include "pr/image_backend.h"

#include "image/backends/shared/src/image_shared.h"

namespace pr_impl {

namespace {

// An open GIF: the decoder, and the bytes of the file that it reads.
struct GifReader : PrImageReader {
    uint8_t* bytes;
    PrGifDecoder* gif;
    PrStatus openStatus;    // Ok, or Truncated for a file that ends early
    bool hasFrames;         // false for a file cut before its first frame, which the interface counts as one
};

void close(PrImageReader* reader) {
    GifReader* gif = static_cast<GifReader*>(reader);
    prGifDecoderDestroy(gif->gif);
    prMemoryFree(gif->bytes);
    prMemoryFree(gif);
}

PrStatus frameInfo(PrImageReader* reader, uint32_t frame, PrImageFrameInfo* info) {
    GifReader* gif = static_cast<GifReader*>(reader);
    if (!gif->hasFrames)
        return PrStatus_InvalidData;
    info->durationMs = prGifDecoderFrameDuration(gif->gif, frame);
    return PrStatus_Ok;
}

PrStatus read(PrImageReader* reader, uint32_t frame, uint32_t, PrPixels output) {
    GifReader* gif = static_cast<GifReader*>(reader);
    if (!gif->hasFrames)
        return PrStatus_InvalidData;
    PrStatus status = prGifDecoderRead(gif->gif, frame, output);
    return status == PrStatus_Ok ? gif->openStatus : status;
}

uint32_t headerBytes(const PrImageCodec*) {
    return kPrGifSignatureBytes;
}

bool supportsType(const PrImageCodec*, PrStringView type) {
    return type == "image/gif";
}

bool recognize(const PrImageCodec*, PrByteView prefix) {
    return prGifHasSignature(prefix);
}

PrStatus open(const PrImageCodec*, const PrImageCodecSet*, const PrImageSource* source, const PrImageLimits* limits,
        PrImageReader* PR_NULLABLE* PR_NONNULL out, PrImageInfo* info) {
    *out = nullptr;
    uint8_t* bytes = nullptr;
    size_t size = 0;
    PrStatus status = imageReadSource(source, *limits, sizeof(GifReader), &bytes, &size);
    if (status != PrStatus_Ok)
        return status;
    const PrGifLimits gifLimits{
        .pixels = limits->pixels,
        .workingBytes = limits->workingBytes,
        .outputBytes = limits->outputBytes,
        .work = limits->work,
        .frames = limits->frames,
        .metadataBytes = limits->metadataBytes,
    };
    PrGifDecoder* gif = nullptr;
    status = prGifDecoderCreate(PrByteView{bytes, size}, &gifLimits, size + sizeof(GifReader), &gif);
    if (gif == nullptr) {
        prMemoryFree(bytes);
        return status;
    }
    const PrGifInfo& gifInfo = *prGifDecoderInfo(gif);
    *info = PrImageInfo{
        .extent = gifInfo.extent,
        .format = PrImageFormat{
            .model = PrColorModel_Rgb,
            .sample = PrImageSample_U8,
            .alpha = PrImageAlpha_Straight,
            .color = PrImageColor{.encoding = PrImageColorEncoding_Named, .space = PrColorSpace_Srgb},
        },
        .frameCount = gifInfo.frameCount != 0 ? gifInfo.frameCount : 1,
        .playCount = gifInfo.playCount,
        .workingBytes = gifInfo.workingBytes,
    };
    GifReader* reader = allocZeroed<GifReader>();
    reader->closeFn = close;
    reader->frameInfoFn = frameInfo;
    reader->readFn = read;
    reader->bytes = bytes;
    reader->gif = gif;
    reader->openStatus = status;
    reader->hasFrames = gifInfo.frameCount != 0;
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

PrImageCodec* prImageGifCreate(void) {
    return &gCodec;
}
