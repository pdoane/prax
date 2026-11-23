// The DDS codec. It presents the textures of lib/dds through the image interface.

#include "pr/image_dds.h"

#include "pr/alloc.h"
#include "pr/dds.h"
#include "pr/image_backend.h"

#include "image/backends/shared/src/image_shared.h"

namespace pr_impl {

namespace {

// The bytes of the magic that begins a DDS file.
constexpr uint32_t kMagicBytes = 4;

// An open texture: the PrDdsTexture, and the bytes of the file that it reads.
struct DdsReader : PrImageReader {
    uint8_t* bytes;
    PrDdsTexture* texture;
};

// The sample type and the alpha of the pixels that a texture decodes to, indexed by PrDdsSample and PrDdsAlpha.
constexpr PrImageSample kSamples[] = {PrImageSample_U8, PrImageSample_U16, PrImageSample_F16, PrImageSample_F32};
constexpr PrImageAlpha kAlphas[] = {PrImageAlpha_None, PrImageAlpha_Straight, PrImageAlpha_Premultiplied};

// Returns the image format of the pixels that a texture of `info` decodes to.
PrImageFormat formatOf(const PrDdsInfo& info) {
    PrImageFormat format{
        .model = PrColorModel(info.channels <= 2 ? PrColorModel_Gray : PrColorModel_Rgb),
        .sample = kSamples[info.sample],
        .alpha = kAlphas[info.alpha],
    };
    if (info.srgb)
        format.color = PrImageColor{.encoding = PrImageColorEncoding_Named, .space = PrColorSpace_Srgb};
    return format;
}

bool usable(PrStatus status) {
    return status == PrStatus_Ok || status == PrStatus_Truncated;
}

void close(PrImageReader* reader) {
    DdsReader* dds = static_cast<DdsReader*>(reader);
    prDdsTextureDestroy(dds->texture);
    prMemoryFree(dds->bytes);
    prMemoryFree(dds);
}

PrStatus frameInfo(PrImageReader*, uint32_t, PrImageFrameInfo* info) {
    info->durationMs = 0;
    return PrStatus_Ok;
}

PrStatus read(PrImageReader* reader, uint32_t frame, uint32_t level, PrPixels output) {
    return prDdsTextureRead(static_cast<DdsReader*>(reader)->texture, frame, level, output);
}

uint32_t headerBytes(const PrImageCodec*) {
    return kMagicBytes;
}

bool supportsType(const PrImageCodec*, PrStringView type) {
    return type == "image/vnd.ms-dds";
}

bool recognize(const PrImageCodec*, PrByteView prefix) {
    return prDdsHasSignature(prefix);
}

PrStatus open(const PrImageCodec*, const PrImageCodecSet*, const PrImageSource* source, const PrImageLimits* limits,
        PrImageReader* PR_NULLABLE* PR_NONNULL out, PrImageInfo* info) {
    *out = nullptr;
    uint8_t* bytes = nullptr;
    size_t size = 0;
    PrStatus status = imageReadSource(source, *limits, sizeof(DdsReader), &bytes, &size);
    if (status != PrStatus_Ok)
        return status;
    PrDdsTexture* texture = nullptr;
    status = prDdsTextureCreate(PrByteView{bytes, size}, &texture);
    const PrDdsInfo* dds = texture != nullptr ? prDdsTextureInfo(texture) : nullptr;
    // A texture whose texels do not decode has no pixels to read, and a volume has no frames.
    if (dds != nullptr && (!dds->decodes || dds->depth > 1))
        status = PrStatus_NotSupported;
    const uint64_t frames = dds != nullptr ? uint64_t(dds->layerCount) * dds->faceCount : 0;
    if (usable(status) && frames > limits->frames)
        status = PrStatus_Limit;
    // The levels below the first level together hold a third as many texels as the first level.
    const uint64_t pixels = dds != nullptr ? uint64_t(dds->extent.x) * dds->extent.y : 0;
    if (usable(status) && (pixels > limits->pixels || pixels > limits->work / 2 / frames))
        status = PrStatus_Limit;
    if (usable(status)) {
        *info = PrImageInfo{
            .extent = dds->extent,
            .format = formatOf(*dds),
            .frameCount = uint32_t(frames),
            .levelCount = dds->levelCount,
            .faceCount = dds->faceCount,
            .playCount = 1,
            .workingBytes = uint64_t(size) + sizeof(DdsReader),
        };
        if (pixels * prImageFormatSize(&info->format) > limits->outputBytes)
            status = PrStatus_Limit;
    }
    if (!usable(status)) {
        prDdsTextureDestroy(texture);
        prMemoryFree(bytes);
        return status;
    }
    DdsReader* reader = allocZeroed<DdsReader>();
    reader->closeFn = close;
    reader->frameInfoFn = frameInfo;
    reader->readFn = read;
    reader->bytes = bytes;
    reader->texture = texture;
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

PrImageCodec* prImageDdsCreate(void) {
    return &gCodec;
}
