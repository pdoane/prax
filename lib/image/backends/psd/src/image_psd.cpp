// The PSD codec: lib/psd's documents as the image interface describes them. The JPEG thumbnail of a document opens as
// an image of its own, through the set that opens the document, and is the document's preview.

#include "pr/image_psd.h"

#include "pr/alloc.h"
#include "pr/color_profile.h"
#include "pr/image_backend.h"
#include "pr/psd.h"

#include "image/backends/shared/src/image_shared.h"

namespace pr_impl {

namespace {

// An open document: the decoder, the bytes of the file that it reads, and the decoder of its thumbnail, if the
// thumbnail is its preview.
struct PsdReader : PrImageReader {
    uint8_t* bytes;
    PrPsdDecoder* psd;
    PrImageDecoder* PR_NULLABLE thumbnail;
    PrImageFormat format;           // of the document's pixels
};

constexpr PrImageSample kSamples[] = {PrImageSample_U8, PrImageSample_U16, PrImageSample_F32};
constexpr PrColorModel kModels[] = {PrColorModel_Gray, PrColorModel_Rgb, PrColorModel_Cmyk, PrColorModel_Lab};

// Returns the color of a document of `info`: its profile, or the named space that the profile is, the named space of
// Lab, linear sRGB for 32-bit gray and RGB, and uncharacterized otherwise.
PrImageColor colorOf(const PrPsdInfo& info) {
    if (info.colors == PrPsdColors_Lab)
        return PrImageColor{.encoding = PrImageColorEncoding_Named, .space = PrColorSpace_Lab};
    if (info.profile.size != 0) {
        PrColorProfile* profile = nullptr;
        PrColorSpace named = PrColorSpace_Count;
        if (prColorProfileCreate(info.profile, &profile) == PrStatus_Ok)
            named = prColorProfileNamedSpace(profile);
        prColorProfileDestroy(profile);
        if (named != PrColorSpace_Count && info.colors != PrPsdColors_Cmyk)
            return PrImageColor{.encoding = PrImageColorEncoding_Named, .space = named};
        return PrImageColor{.encoding = PrImageColorEncoding_Icc, .profile = info.profile};
    }
    if (info.depth == 32 && info.colors != PrPsdColors_Cmyk)
        return PrImageColor{.encoding = PrImageColorEncoding_Named, .space = PrColorSpace_SrgbLinear};
    return PrImageColor{.encoding = PrImageColorEncoding_Unknown};
}

void close(PrImageReader* reader) {
    PsdReader* psd = static_cast<PsdReader*>(reader);
    prImageDecoderDestroy(psd->thumbnail);
    prPsdDecoderDestroy(psd->psd);
    prMemoryFree(psd->bytes);
    prMemoryFree(psd);
}

PrStatus frameInfo(PrImageReader*, uint32_t, PrImageFrameInfo* info) {
    info->durationMs = 0;
    return PrStatus_Ok;
}

PrStatus read(PrImageReader* reader, uint32_t, uint32_t, PrPixels output) {
    return prPsdDecoderRead(static_cast<PsdReader*>(reader)->psd, output);
}

PrUVec2 previewExtent(const PrImageReader* reader, uint32_t) {
    return prImageDecoderInfo(static_cast<const PsdReader*>(reader)->thumbnail)->extent;
}

// Decodes the thumbnail in its own format and converts it to the document's.
PrStatus readPreview(PrImageReader* reader, uint32_t, PrPixels output) {
    PsdReader* psd = static_cast<PsdReader*>(reader);
    const PrImageInfo& thumbnail = *prImageDecoderInfo(psd->thumbnail);
    const uint32_t pixel = prImageFormatSize(&thumbnail.format);
    const size_t stride = size_t(thumbnail.extent.x) * pixel;
    uint8_t* stored = static_cast<uint8_t*>(prMemoryAlloc(stride * thumbnail.extent.y));
    PrStatus status = prImageDecoderRead(psd->thumbnail, 0, PrPixels{stored, stride, thumbnail.extent});
    if (status == PrStatus_Ok || status == PrStatus_Truncated) {
        const PrStatus converted = prImageConvert(PrPixelView{stored, stride, thumbnail.extent}, &thumbnail.format,
            output, &psd->format, PrImageConversionFlag_AssumeSrgb);
        status = converted != PrStatus_Ok ? converted : status;
    }
    prMemoryFree(stored);
    return status;
}

// Returns true if a thumbnail of `format` converts to the document's `to`.
bool converts(const PrImageFormat& format, const PrImageFormat& to) {
    uint8_t from[16] = {};
    uint8_t into[32] = {};
    return prImageConvert(PrPixelView{from, sizeof(from), {1, 1}}, &format, PrPixels{into, sizeof(into), {1, 1}}, &to,
        PrImageConversionFlag_AssumeSrgb) == PrStatus_Ok;
}

// Opens the thumbnail of `psd` through `set`, within what the document leaves of `limits`. Returns null if it does not
// open as a preview: smaller than the document on an axis, larger on neither, and of pixels that convert to the
// document's.
PrImageDecoder* PR_NULLABLE openThumbnail(const PrImageCodecSet* set, const PrPsdInfo& psd, const PrImageFormat& format,
        const PrImageLimits* limits) {
    if (psd.thumbnail.size == 0)
        return nullptr;
    PrImageLimits thumbnailLimits = *limits;
    thumbnailLimits.workingBytes = limits->workingBytes - psd.workingBytes;
    PrImageDecoder* decoder = prImageDecoderCreate(set);
    PrImageSource source{};
    source.memory = psd.thumbnail;
    const PrStatus status = prImageDecoderOpen(decoder, source, &thumbnailLimits);
    if (status == PrStatus_Ok) {
        const PrImageInfo& info = *prImageDecoderInfo(decoder);
        const PrUVec2 e = info.extent;
        const bool smaller = e.x <= psd.extent.x && e.y <= psd.extent.y && (e.x < psd.extent.x || e.y < psd.extent.y);
        if (smaller && converts(info.format, format))
            return decoder;
    }
    prImageDecoderDestroy(decoder);
    return nullptr;
}

uint32_t headerBytes(const PrImageCodec*) {
    return kPrPsdHeaderBytes;
}

bool supportsType(const PrImageCodec*, PrStringView type) {
    return type == "image/vnd.adobe.photoshop";
}

bool recognize(const PrImageCodec*, PrByteView prefix) {
    return prPsdHasSignature(prefix);
}

PrStatus open(const PrImageCodec*, const PrImageCodecSet* set, const PrImageSource* source, const PrImageLimits* limits,
        PrImageReader* PR_NULLABLE* PR_NONNULL out, PrImageInfo* info) {
    *out = nullptr;
    uint8_t* bytes = nullptr;
    size_t size = 0;
    PrStatus status = imageReadSource(source, *limits, sizeof(PsdReader), &bytes, &size);
    if (status != PrStatus_Ok)
        return status;
    const PrPsdLimits psdLimits{
        .pixels = limits->pixels,
        .workingBytes = limits->workingBytes,
        .outputBytes = limits->outputBytes,
        .work = limits->work,
    };
    PrPsdDecoder* psd = nullptr;
    status = prPsdDecoderCreate(PrByteView{bytes, size}, &psdLimits, size + sizeof(PsdReader), &psd);
    if (psd == nullptr) {
        prMemoryFree(bytes);
        return status;
    }
    const PrPsdInfo& psdInfo = *prPsdDecoderInfo(psd);
    const PrImageFormat format{
        .model = kModels[psdInfo.colors],
        .sample = kSamples[psdInfo.sample],
        .alpha = PrImageAlpha(psdInfo.alpha ? PrImageAlpha_Straight : PrImageAlpha_None),
        .color = colorOf(psdInfo),
    };
    PrImageDecoder* thumbnail = openThumbnail(set, psdInfo, format, limits);
    *info = PrImageInfo{
        .extent = psdInfo.extent,
        .format = format,
        .pixelsPerMeter = psdInfo.pixelsPerMeter,
        .frameCount = 1,
        .playCount = 1,
        .workingBytes = psdInfo.workingBytes,
        .previewCount = thumbnail != nullptr ? 1u : 0u,
        .exif = psdInfo.exif,
    };
    if (thumbnail != nullptr)
        info->workingBytes += prImageDecoderInfo(thumbnail)->workingBytes;
    PsdReader* reader = allocZeroed<PsdReader>();
    reader->closeFn = close;
    reader->frameInfoFn = frameInfo;
    reader->readFn = read;
    if (thumbnail != nullptr) {
        reader->previewExtentFn = previewExtent;
        reader->readPreviewFn = readPreview;
    }
    reader->bytes = bytes;
    reader->psd = psd;
    reader->thumbnail = thumbnail;
    reader->format = format;
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

PrImageCodec* prImagePsdCreate(void) {
    return &gCodec;
}
