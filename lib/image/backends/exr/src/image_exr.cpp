// The OpenEXR codec: lib/exr's images as the image interface describes them.

#include "pr/image_exr.h"

#include "pr/alloc.h"
#include "pr/color_space.h"
#include "pr/exr.h"
#include "pr/image_backend.h"

#include "image/backends/shared/src/image_shared.h"

namespace pr_impl {

namespace {

struct ExrCodec : PrImageCodec {
    const PrCompressCodecSet* compress;
};

// An open image: lib/exr's, and the bytes of the file that it reads.
struct ExrReader : PrImageReader {
    uint8_t* bytes;
    PrExrImage* exr;
};

// Returns the image's color of values in the RGB space of `chromaticities`: a named linear space whose primaries and
// white they are, or else the chromaticities themselves.
PrImageColor colorOf(const PrColorChromaticities& chromaticities) {
    PrColorPrimaries primaries = prColorPrimariesFromChromaticities(&chromaticities, 1e-4f);
    if (primaries == PrColorPrimaries_Rec709)
        return PrImageColor{.encoding = PrImageColorEncoding_Named, .space = PrColorSpace_SrgbLinear};
    if (primaries == PrColorPrimaries_DisplayP3)
        return PrImageColor{.encoding = PrImageColorEncoding_Named, .space = PrColorSpace_DisplayP3Linear};
    if (primaries == PrColorPrimaries_Rec2020)
        return PrImageColor{.encoding = PrImageColorEncoding_Named, .space = PrColorSpace_Rec2100Linear};
    return PrImageColor{.encoding = PrImageColorEncoding_Chromaticities, .chromaticities = chromaticities};
}

void close(PrImageReader* reader) {
    ExrReader* exr = static_cast<ExrReader*>(reader);
    prExrImageDestroy(exr->exr);
    prMemoryFree(exr->bytes);
    prMemoryFree(exr);
}

PrStatus frameInfo(PrImageReader*, uint32_t, PrImageFrameInfo* info) {
    info->durationMs = 0;
    return PrStatus_Ok;
}

PrStatus read(PrImageReader* reader, uint32_t, uint32_t level, PrPixels output) {
    return prExrImageRead(static_cast<ExrReader*>(reader)->exr, level, output);
}

PrUVec2 levelExtent(const PrImageReader* reader, uint32_t level) {
    return prExrImageLevelExtent(static_cast<const ExrReader*>(reader)->exr, level);
}

uint32_t headerBytes(const PrImageCodec*) {
    return kPrExrSignatureBytes;
}

bool supportsType(const PrImageCodec*, PrStringView type) {
    return type == "image/x-exr" || type == "image/aces";
}

bool recognize(const PrImageCodec*, PrByteView prefix) {
    return prExrHasSignature(prefix);
}

PrStatus open(const PrImageCodec* imageCodec, const PrImageCodecSet*, const PrImageSource* source,
        const PrImageLimits* limits, PrImageReader* PR_NULLABLE* PR_NONNULL out, PrImageInfo* info) {
    *out = nullptr;
    uint8_t* bytes = nullptr;
    size_t size = 0;
    PrStatus status = imageReadSource(source, *limits, sizeof(ExrReader), &bytes, &size);
    if (status != PrStatus_Ok)
        return status;
    const PrExrLimits exrLimits{
        .pixels = limits->pixels,
        .workingBytes = limits->workingBytes,
        .work = limits->work,
        .parts = limits->frames,
        .metadataBytes = limits->metadataBytes,
    };
    PrExrImage* exr = nullptr;
    status = prExrImageCreate(PrByteView{bytes, size}, static_cast<const ExrCodec*>(imageCodec)->compress, &exrLimits,
        size + sizeof(ExrReader), &exr);
    if (exr == nullptr) {
        prMemoryFree(bytes);
        return status;
    }
    const PrExrPartInfo& part = *prExrImagePartInfo(exr);
    *info = PrImageInfo{
        .extent = part.extent,
        .format = PrImageFormat{
            .model = PrColorModel(part.colors == 3 ? PrColorModel_Rgb : PrColorModel_Gray),
            .sample = PrImageSample(part.sample == PrExrSample_Half ? PrImageSample_F16 : PrImageSample_F32),
            .alpha = PrImageAlpha(part.alpha ? PrImageAlpha_Premultiplied : PrImageAlpha_None),
            .color = colorOf(part.chromaticities),
        },
        .frameCount = 1,
        .levelCount = part.levelCount,
        .playCount = 1,
        .workingBytes = part.workingBytes,
        .levels = PrImageLevels(part.levels == PrExrLevels_Rip ? PrImageLevels_Rip : PrImageLevels_Mip),
        .levelColumns = part.levelColumns,
    };
    ExrReader* reader = allocZeroed<ExrReader>();
    reader->closeFn = close;
    reader->frameInfoFn = frameInfo;
    reader->readFn = read;
    reader->levelExtentFn = levelExtent;
    reader->bytes = bytes;
    reader->exr = exr;
    *out = reader;
    return status;
}

void destroy(PrImageCodec* imageCodec) {
    prMemoryFree(imageCodec);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrImageCodec* prImageExrCreate(const PrCompressCodecSet* compress) {
    ExrCodec* codec = allocZeroed<ExrCodec>();
    codec->destroyFn = destroy;
    codec->headerBytesFn = headerBytes;
    codec->supportsTypeFn = supportsType;
    codec->recognizeFn = recognize;
    codec->decoderOpenFn = open;
    codec->compress = compress;
    return codec;
}
