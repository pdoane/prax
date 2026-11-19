// The Radiance HDR codec: lib/hdr's pictures as the image interface describes them.

#include "pr/image_hdr.h"

#include "pr/alloc.h"
#include "pr/hdr.h"
#include "pr/image_backend.h"

#include "image/backends/shared/src/image_shared.h"

namespace pr_impl {

namespace {

// An open picture: the decoder, and the bytes of the file that it reads.
struct HdrReader : PrImageReader {
    uint8_t* bytes;
    PrHdrDecoder* hdr;
};

// Returns how stored scanlines map to the displayed picture, from the axis that scanlines follow one another along and
// the axis along a scanline. Radiance's y axis runs up, so scanlines from the top down run against it.
PrImageOrientation orientationOf(const PrHdrAxis& major, const PrHdrAxis& minor) {
    if (major.y) {
        if (major.decreasing)
            return minor.decreasing ? PrImageOrientation_FlipHorizontal : PrImageOrientation_Identity;
        return minor.decreasing ? PrImageOrientation_Rotate180 : PrImageOrientation_FlipVertical;
    }
    // Each scanline is a column of the displayed picture.
    if (major.decreasing)
        return minor.decreasing ? PrImageOrientation_Rotate90 : PrImageOrientation_Transverse;
    return minor.decreasing ? PrImageOrientation_Transpose : PrImageOrientation_Rotate270;
}

void close(PrImageReader* reader) {
    HdrReader* hdr = static_cast<HdrReader*>(reader);
    prHdrDecoderDestroy(hdr->hdr);
    prMemoryFree(hdr->bytes);
    prMemoryFree(hdr);
}

PrStatus frameInfo(PrImageReader*, uint32_t, PrImageFrameInfo* info) {
    info->durationMs = 0;
    return PrStatus_Ok;
}

PrStatus read(PrImageReader* reader, uint32_t, uint32_t, PrPixels output) {
    return prHdrDecoderRead(static_cast<HdrReader*>(reader)->hdr, output);
}

uint32_t headerBytes(const PrImageCodec*) {
    return kPrHdrSignatureBytes;
}

bool supportsType(const PrImageCodec*, PrStringView type) {
    return type == "image/vnd.radiance";
}

bool recognize(const PrImageCodec*, PrByteView prefix) {
    return prHdrHasSignature(prefix);
}

PrStatus open(const PrImageCodec*, const PrImageCodecSet*, const PrImageSource* source, const PrImageLimits* limits,
        PrImageReader* PR_NULLABLE* PR_NONNULL out, PrImageInfo* info) {
    *out = nullptr;
    uint8_t* bytes = nullptr;
    size_t size = 0;
    PrStatus status = imageReadSource(source, *limits, sizeof(HdrReader), &bytes, &size);
    if (status != PrStatus_Ok)
        return status;
    const PrHdrLimits hdrLimits{
        .pixels = limits->pixels,
        .workingBytes = limits->workingBytes,
        .outputBytes = limits->outputBytes,
        .work = limits->work,
    };
    PrHdrDecoder* hdr = nullptr;
    status = prHdrDecoderCreate(PrByteView{bytes, size}, &hdrLimits, size + sizeof(HdrReader), &hdr);
    if (hdr == nullptr) {
        prMemoryFree(bytes);
        return status;
    }
    const PrHdrInfo& hdrInfo = *prHdrDecoderInfo(hdr);
    // A Radiance picture holds radiance in linear light. Its primaries are close to sRGB's, and are taken as sRGB's.
    *info = PrImageInfo{
        .extent = hdrInfo.extent,
        .format = PrImageFormat{
            .model = PrColorModel_Rgb,
            .sample = PrImageSample_F32,
            .alpha = PrImageAlpha_None,
            .color = PrImageColor{.encoding = PrImageColorEncoding_Named, .space = PrColorSpace_SrgbLinear},
        },
        .orientation = orientationOf(hdrInfo.scanlineAxis, hdrInfo.pixelAxis),
        .frameCount = 1,
        .playCount = 1,
        .workingBytes = hdrInfo.workingBytes,
    };
    HdrReader* reader = allocZeroed<HdrReader>();
    reader->closeFn = close;
    reader->frameInfoFn = frameInfo;
    reader->readFn = read;
    reader->bytes = bytes;
    reader->hdr = hdr;
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

PrImageCodec* prImageHdrCreate(void) {
    return &gCodec;
}
