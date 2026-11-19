// The emjc codec: lib/emjc's images as the image interface describes them.

#include "pr/image_emjc.h"

#include "pr/alloc.h"
#include "pr/emjc.h"
#include "pr/image_backend.h"

#include "image/backends/shared/src/image_shared.h"

namespace pr_impl {

namespace {

struct EmjcCodec : PrImageCodec {
    const PrCompressCodecSet* compress;
};

// An open emjc image: the decoder, and the bytes that it reads.
struct EmjcReader : PrImageReader {
    uint8_t* bytes;
    PrEmjcDecoder* emjc;
};

void close(PrImageReader* reader) {
    EmjcReader* emjc = static_cast<EmjcReader*>(reader);
    prEmjcDecoderDestroy(emjc->emjc);
    prMemoryFree(emjc->bytes);
    prMemoryFree(emjc);
}

PrStatus frameInfo(PrImageReader*, uint32_t, PrImageFrameInfo* info) {
    info->durationMs = 0;
    return PrStatus_Ok;
}

PrStatus read(PrImageReader* reader, uint32_t, uint32_t, PrPixels output) {
    return prEmjcDecoderRead(static_cast<EmjcReader*>(reader)->emjc, output);
}

uint32_t headerBytes(const PrImageCodec*) {
    return kPrEmjcSignatureBytes;
}

bool supportsType(const PrImageCodec*, PrStringView type) {
    return type == "image/x-emjc";
}

bool recognize(const PrImageCodec*, PrByteView prefix) {
    return prEmjcHasSignature(prefix);
}

PrStatus open(const PrImageCodec* imageCodec, const PrImageCodecSet*, const PrImageSource* source,
        const PrImageLimits* limits, PrImageReader* PR_NULLABLE* PR_NONNULL out, PrImageInfo* info) {
    *out = nullptr;
    uint8_t* bytes = nullptr;
    size_t size = 0;
    PrStatus status = imageReadSource(source, *limits, sizeof(EmjcReader), &bytes, &size);
    if (status != PrStatus_Ok)
        return status;
    const PrEmjcLimits emjcLimits{
        .pixels = limits->pixels,
        .workingBytes = limits->workingBytes,
        .outputBytes = limits->outputBytes,
        .work = limits->work,
    };
    PrEmjcDecoder* emjc = nullptr;
    status = prEmjcDecoderCreate(PrByteView{bytes, size}, static_cast<const EmjcCodec*>(imageCodec)->compress,
        &emjcLimits, size + sizeof(EmjcReader), &emjc);
    if (emjc == nullptr) {
        prMemoryFree(bytes);
        return status;
    }
    const PrEmjcInfo& emjcInfo = *prEmjcDecoderInfo(emjc);
    *info = PrImageInfo{
        .extent = emjcInfo.extent,
        .format = PrImageFormat{
            .model = PrColorModel_Rgb,
            .sample = PrImageSample_U8,
            .alpha = PrImageAlpha_Straight,
            .color = PrImageColor{.encoding = PrImageColorEncoding_Unknown},
        },
        .frameCount = 1,
        .playCount = 1,
        .workingBytes = emjcInfo.workingBytes,
    };
    EmjcReader* reader = allocZeroed<EmjcReader>();
    reader->closeFn = close;
    reader->frameInfoFn = frameInfo;
    reader->readFn = read;
    reader->bytes = bytes;
    reader->emjc = emjc;
    *out = reader;
    return PrStatus_Ok;
}

void destroy(PrImageCodec* imageCodec) {
    prMemoryFree(imageCodec);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrImageCodec* prImageEmjcCreate(const PrCompressCodecSet* compress) {
    EmjcCodec* codec = allocZeroed<EmjcCodec>();
    codec->destroyFn = destroy;
    codec->headerBytesFn = headerBytes;
    codec->supportsTypeFn = supportsType;
    codec->recognizeFn = recognize;
    codec->decoderOpenFn = open;
    codec->compress = compress;
    return codec;
}
