// The KTX 2.0 codec. It presents the textures of lib/ktx through the image interface.

#include "pr/image_ktx.h"

#include "pr/alloc.h"
#include "pr/image_backend.h"
#include "pr/ktx.h"

#include "image/backends/shared/src/image_shared.h"

namespace pr_impl {

namespace {

// The bytes of the identifier that begins a KTX 2.0 file.
constexpr uint32_t kIdentifierBytes = 12;

struct KtxCodec : PrImageCodec {
    const PrCompressCodecSet* compress;
};

// An open texture: the PrKtxTexture, and the bytes of the file that it reads.
struct KtxReader : PrImageReader {
    uint8_t* bytes;
    PrKtxTexture* texture;
};

// The sample type of the pixels that a texture decodes to, indexed by PrKtxSample.
constexpr PrImageSample kSamples[] = {PrImageSample_U8, PrImageSample_U16, PrImageSample_F16, PrImageSample_F32};

// The alpha of the pixels that a texture decodes to, indexed by PrKtxAlpha.
constexpr PrImageAlpha kAlphas[] = {PrImageAlpha_None, PrImageAlpha_Straight, PrImageAlpha_Premultiplied};

// Returns the image format of the pixels that a texture of `info` decodes to.
PrImageFormat formatOf(const PrKtxInfo& info) {
    PrImageFormat format{
        .model = PrColorModel(info.channels == 1 ? PrColorModel_Gray : PrColorModel_Rgb),
        .sample = kSamples[info.sample],
        .alpha = kAlphas[info.alpha],
    };
    if (info.color == PrKtxColor_Srgb)
        format.color = PrImageColor{.encoding = PrImageColorEncoding_Named, .space = PrColorSpace_Srgb};
    else if (info.color == PrKtxColor_SrgbLinear)
        format.color = PrImageColor{.encoding = PrImageColorEncoding_Named, .space = PrColorSpace_SrgbLinear};
    return format;
}

// Returns the orientation that a texture's KTXorientation gives.
PrImageOrientation orientationOf(const PrKtxInfo& info) {
    if (info.fromRight && info.fromBottom)
        return PrImageOrientation_Rotate180;
    if (info.fromRight)
        return PrImageOrientation_FlipHorizontal;
    return info.fromBottom ? PrImageOrientation_FlipVertical : PrImageOrientation_Identity;
}

// Returns true if a texture created with `status` is open.
bool usable(PrStatus status) {
    return status == PrStatus_Ok || status == PrStatus_Truncated;
}

void close(PrImageReader* reader) {
    KtxReader* ktx = static_cast<KtxReader*>(reader);
    prKtxTextureDestroy(ktx->texture);
    prMemoryFree(ktx->bytes);
    prMemoryFree(ktx);
}

PrStatus frameInfo(PrImageReader*, uint32_t, PrImageFrameInfo* info) {
    info->durationMs = 0;
    return PrStatus_Ok;
}

PrStatus read(PrImageReader* reader, uint32_t frame, uint32_t level, PrPixels output) {
    return prKtxTextureRead(static_cast<KtxReader*>(reader)->texture, frame, level, output);
}

uint32_t headerBytes(const PrImageCodec*) {
    return kIdentifierBytes;
}

bool supportsType(const PrImageCodec*, PrStringView type) {
    return type == "image/ktx2";
}

bool recognize(const PrImageCodec*, PrByteView prefix) {
    return prKtxHasSignature(prefix);
}

PrStatus open(const PrImageCodec* imageCodec, const PrImageCodecSet*, const PrImageSource* source,
        const PrImageLimits* limits, PrImageReader* PR_NULLABLE* PR_NONNULL out, PrImageInfo* info) {
    *out = nullptr;
    uint8_t* bytes = nullptr;
    size_t size = 0;
    PrStatus status = imageReadSource(source, *limits, sizeof(KtxReader), &bytes, &size);
    if (status != PrStatus_Ok)
        return status;
    PrKtxTexture* texture = nullptr;
    status = PrStatus_Limit;
    if (limits->work != 0)
        status = prKtxTextureCreate(PrByteView{bytes, size}, static_cast<const KtxCodec*>(imageCodec)->compress, &texture);
    const PrKtxInfo* ktx = texture != nullptr ? prKtxTextureInfo(texture) : nullptr;
    // A block texture whose blocks do not decode has no pixels to read.
    if (ktx != nullptr && ktx->payload == PrKtxPayload_Blocks && !ktx->blocksDecode)
        status = PrStatus_NotSupported;
    const uint64_t frames = ktx != nullptr ? uint64_t(ktx->layerCount) * ktx->faceCount : 0;
    if (usable(status) && frames > limits->frames)
        status = PrStatus_Limit;
    // The levels below the first level together hold a third as many texels as the first level.
    const uint64_t pixels = ktx != nullptr ? uint64_t(ktx->extent.x) * ktx->extent.y : 0;
    if (usable(status) && (pixels > limits->pixels || pixels * frames * 2 > limits->work))
        status = PrStatus_Limit;
    // An inflated level is held whole in memory while it is read, and the first level is the largest.
    uint64_t working = uint64_t(size) + sizeof(KtxReader);
    if (usable(status) && ktx->inflatedBytes > limits->workingBytes)
        status = PrStatus_Limit;
    working += ktx != nullptr ? ktx->inflatedBytes : 0;
    // The data that the levels share is read only after the texture passes the limits.
    if (usable(status)) {
        const PrStatus prepared = prKtxTexturePrepare(texture);
        status = prepared != PrStatus_Ok ? prepared : status;
    }
    if (usable(status)) {
        working += prKtxTextureSharedBytes(texture);
        *info = PrImageInfo{
            .extent = ktx->extent,
            .format = formatOf(*ktx),
            .orientation = orientationOf(*ktx),
            .frameCount = uint32_t(frames),
            .levelCount = ktx->levelCount,
            .faceCount = ktx->faceCount,
            .playCount = 1,
        };
        const uint64_t output = pixels * prImageFormatSize(&info->format);
        if (output > limits->outputBytes || working > limits->workingBytes)
            status = PrStatus_Limit;
        info->workingBytes = working;
    }
    if (!usable(status)) {
        prKtxTextureDestroy(texture);
        prMemoryFree(bytes);
        return status;
    }
    KtxReader* reader = allocZeroed<KtxReader>();
    reader->closeFn = close;
    reader->frameInfoFn = frameInfo;
    reader->readFn = read;
    reader->bytes = bytes;
    reader->texture = texture;
    *out = reader;
    return status;
}

void destroy(PrImageCodec* imageCodec) {
    prMemoryFree(imageCodec);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrImageCodec* prImageKtxCreate(const PrCompressCodecSet* compress) {
    KtxCodec* codec = allocZeroed<KtxCodec>();
    codec->destroyFn = destroy;
    codec->headerBytesFn = headerBytes;
    codec->supportsTypeFn = supportsType;
    codec->recognizeFn = recognize;
    codec->decoderOpenFn = open;
    codec->compress = compress;
    return codec;
}
