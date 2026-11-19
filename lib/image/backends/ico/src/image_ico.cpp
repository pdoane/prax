// The ICO codec: lib/ico's icons as the image interface describes them. An icon's PNG image opens as an image of its
// own, through the set that opens the icon.

#include "pr/image_ico.h"

#include "pr/alloc.h"
#include "pr/ico.h"
#include "pr/image_backend.h"

#include "image/backends/shared/src/image_shared.h"

namespace pr_impl {

namespace {

// An open icon: the decoder, the bytes of the file that it reads, and the decoder of its PNG image, if it holds one.
struct IcoReader : PrImageReader {
    uint8_t* bytes;
    PrIcoDecoder* ico;
    PrImageDecoder* PR_NULLABLE png;
};

void close(PrImageReader* reader) {
    IcoReader* ico = static_cast<IcoReader*>(reader);
    prImageDecoderDestroy(ico->png);
    prIcoDecoderDestroy(ico->ico);
    prMemoryFree(ico->bytes);
    prMemoryFree(ico);
}

PrStatus frameInfo(PrImageReader* reader, uint32_t frame, PrImageFrameInfo* info) {
    IcoReader* ico = static_cast<IcoReader*>(reader);
    if (ico->png != nullptr)
        return prImageDecoderFrameInfo(ico->png, frame, info);
    info->durationMs = 0;
    return PrStatus_Ok;
}

PrStatus read(PrImageReader* reader, uint32_t frame, uint32_t, PrPixels output) {
    IcoReader* ico = static_cast<IcoReader*>(reader);
    if (ico->png != nullptr)
        return prImageDecoderRead(ico->png, frame, output);
    return prIcoDecoderRead(ico->ico, output);
}

uint32_t headerBytes(const PrImageCodec*) {
    return kPrIcoSignatureBytes;
}

bool supportsType(const PrImageCodec*, PrStringView type) {
    return type == "image/x-icon" || type == "image/vnd.microsoft.icon";
}

bool recognize(const PrImageCodec*, PrByteView prefix) {
    return prIcoHasSignature(prefix);
}

PrStatus open(const PrImageCodec*, const PrImageCodecSet* set, const PrImageSource* source,
        const PrImageLimits* limits, PrImageReader* PR_NULLABLE* PR_NONNULL out, PrImageInfo* info) {
    *out = nullptr;
    uint8_t* bytes = nullptr;
    size_t size = 0;
    PrStatus status = imageReadSource(source, *limits, sizeof(IcoReader), &bytes, &size);
    if (status != PrStatus_Ok)
        return status;
    const PrIcoLimits icoLimits{
        .pixels = limits->pixels,
        .workingBytes = limits->workingBytes,
        .outputBytes = limits->outputBytes,
        .work = limits->work,
    };
    PrIcoDecoder* ico = nullptr;
    status = prIcoDecoderCreate(PrByteView{bytes, size}, &icoLimits, size + sizeof(IcoReader), &ico);
    if (ico == nullptr) {
        prMemoryFree(bytes);
        return status;
    }
    const PrIcoInfo& icoInfo = *prIcoDecoderInfo(ico);
    PrImageDecoder* png = nullptr;
    if (icoInfo.png.size != 0) {
        // The PNG image opens within what the icon leaves of the limits.
        PrImageLimits pngLimits = *limits;
        pngLimits.workingBytes = limits->workingBytes - icoInfo.workingBytes;
        png = prImageDecoderCreate(set);
        PrImageSource pngSource{};
        pngSource.memory = icoInfo.png;
        status = prImageDecoderOpen(png, pngSource, &pngLimits);
        if (status != PrStatus_Ok && status != PrStatus_Truncated) {
            prImageDecoderDestroy(png);
            prIcoDecoderDestroy(ico);
            prMemoryFree(bytes);
            return status;
        }
        *info = *prImageDecoderInfo(png);
        info->workingBytes += icoInfo.workingBytes;
    } else {
        *info = PrImageInfo{
            .extent = icoInfo.extent,
            .format = PrImageFormat{
                .model = PrColorModel_Rgb,
                .sample = PrImageSample_U8,
                .alpha = PrImageAlpha_Straight,
                .color = PrImageColor{.encoding = PrImageColorEncoding_Named, .space = PrColorSpace_Srgb},
            },
            .frameCount = 1,
            .playCount = 1,
            .workingBytes = icoInfo.workingBytes,
        };
    }
    IcoReader* reader = allocZeroed<IcoReader>();
    reader->closeFn = close;
    reader->frameInfoFn = frameInfo;
    reader->readFn = read;
    reader->bytes = bytes;
    reader->ico = ico;
    reader->png = png;
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

PrImageCodec* prImageIcoCreate(void) {
    return &gCodec;
}
