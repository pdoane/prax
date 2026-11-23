// The JPEG codec: lib/jpeg's images as the image interface describes them.

#include "pr/image_jpeg.h"

#include "pr/alloc.h"
#include "pr/image_backend.h"
#include "pr/jpeg.h"

#include "image/backends/shared/src/image_shared.h"
#include "image/backends/shared/src/metadata.h"

namespace pr_impl {

namespace {

// Returns the meaning of the image's color. It is the image's ICC profile, or uncharacterized if the image has none.
PrImageColor colorOf(const PrJpegInfo& info) {
    if (info.profile.size != 0)
        return PrImageColor{.encoding = PrImageColorEncoding_Icc, .profile = info.profile};
    return PrImageColor{.encoding = PrImageColorEncoding_Unknown};
}

// One preview of a JPEG: a reduced decode of the image, or the thumbnail that its Exif data embeds.
struct JpegPreview {
    uint32_t reduction;     // reduction of the image, or zero for the thumbnail
    PrUVec2 extent;
};

// The maximum number of previews of one JPEG. They are the reductions to a quarter and to an eighth, and the thumbnail.
constexpr uint32_t kMaxPreviews = 3;

// An open JPEG. It holds the decoder, the bytes of the file that the decoder reads, the decoder of the embedded
// thumbnail, and the previews, largest first.
struct JpegReader : PrImageReader {
    uint8_t* bytes;
    PrJpegDecoder* jpeg;
    PrJpegDecoder* PR_NULLABLE thumbnail;
    PrStatus openStatus;    // Ok, or Truncated for a file that ends early
    JpegPreview previews[kMaxPreviews];
};

// The smallest reduction that is a preview. Half the extent saves too little over the image to be worth a decode.
constexpr uint32_t kFirstPreviewReduction = 2;

// Returns the decoder of the thumbnail in `exif`, or null if the image has no thumbnail that can stand for it. A
// thumbnail stands for the image if it decodes whole, has the image's components, is at most half its extent, and has
// its aspect ratio to within one texel. Cameras letterbox some thumbnails, and those do not show the same picture. An
// image with an ICC profile has no thumbnail, because the thumbnail carries no profile of its own.
PrJpegDecoder* PR_NULLABLE thumbnailOpen(PrByteView thumbnail, const PrJpegInfo& image, const PrJpegLimits& limits,
        uint64_t held) {
    if (thumbnail.size == 0 || image.profile.size != 0)
        return nullptr;
    PrJpegDecoder* decoder = nullptr;
    PrStatus status = prJpegDecoderCreate(thumbnail, &limits, held, &decoder);
    if (decoder == nullptr)
        return nullptr;
    const PrJpegInfo& info = *prJpegDecoderInfo(decoder);
    uint64_t across = uint64_t(info.extent.x) * image.extent.y;
    uint64_t down = uint64_t(info.extent.y) * image.extent.x;
    uint64_t skew = across > down ? across - down : down - across;
    bool same = status == PrStatus_Ok && info.components == image.components && info.extent.x != 0 &&
        info.extent.x * 2 <= image.extent.x && info.extent.y * 2 <= image.extent.y &&
        skew <= (image.extent.x > image.extent.y ? image.extent.x : image.extent.y);
    if (!same) {
        prJpegDecoderDestroy(decoder);
        return nullptr;
    }
    return decoder;
}

void close(PrImageReader* reader) {
    JpegReader* jpeg = static_cast<JpegReader*>(reader);
    prJpegDecoderDestroy(jpeg->thumbnail);
    prJpegDecoderDestroy(jpeg->jpeg);
    prMemoryFree(jpeg->bytes);
    prMemoryFree(jpeg);
}

PrStatus frameInfo(PrImageReader*, uint32_t, PrImageFrameInfo* info) {
    info->durationMs = 0;
    return PrStatus_Ok;
}

PrStatus read(PrImageReader* reader, uint32_t, uint32_t, PrPixels output) {
    JpegReader* jpeg = static_cast<JpegReader*>(reader);
    PrStatus status = prJpegDecoderRead(jpeg->jpeg, 0, output);
    return status == PrStatus_Ok ? jpeg->openStatus : status;
}

PrUVec2 previewExtent(const PrImageReader* reader, uint32_t preview) {
    return static_cast<const JpegReader*>(reader)->previews[preview].extent;
}

// The thumbnail is embedded, and the reductions are decodes of the image's data.
bool previewEmbedded(const PrImageReader* reader, uint32_t preview) {
    return static_cast<const JpegReader*>(reader)->previews[preview].reduction == 0;
}

PrStatus readPreview(PrImageReader* reader, uint32_t preview, PrPixels output) {
    JpegReader* jpeg = static_cast<JpegReader*>(reader);
    uint32_t reduction = jpeg->previews[preview].reduction;
    if (reduction == 0)
        return prJpegDecoderRead(jpeg->thumbnail, 0, output);
    PrStatus status = prJpegDecoderRead(jpeg->jpeg, reduction, output);
    return status == PrStatus_Ok ? jpeg->openStatus : status;
}

uint32_t headerBytes(const PrImageCodec*) {
    return kPrJpegSignatureBytes;
}

bool supportsType(const PrImageCodec*, PrStringView type) {
    return type == "image/jpeg";
}

bool recognize(const PrImageCodec*, PrByteView prefix) {
    return prJpegHasSignature(prefix);
}

// Returns the image interface's info of the JPEG of `jpegInfo`, without previews.
PrImageInfo infoOf(const PrJpegInfo& jpegInfo) {
    ImageMetadata exif{};
    if (jpegInfo.exif.size != 0)
        imageReadExif(jpegInfo.exif.data, jpegInfo.exif.size, &exif);
    // The Exif data gives the resolution before the JFIF density does.
    PrVec2 pixelsPerMeter = jpegInfo.pixelsPerMeter;
    if (exif.pixelsPerMeterX != 0)
        pixelsPerMeter = PrVec2{exif.pixelsPerMeterX, exif.pixelsPerMeterY};
    PrColorModel model = PrColorModel_Rgb;
    if (jpegInfo.components == 1)
        model = PrColorModel_Gray;
    else if (jpegInfo.components == 4)
        model = PrColorModel_Cmyk;
    return PrImageInfo{
        .extent = jpegInfo.extent,
        .format = PrImageFormat{
            .model = model,
            .sample = PrImageSample_U8,
            .alpha = PrImageAlpha_None,
            .color = colorOf(jpegInfo),
        },
        .orientation = exif.orientation,
        .pixelsPerMeter = pixelsPerMeter,
        .frameCount = 1,
        .playCount = 1,
        .workingBytes = jpegInfo.workingBytes,
        .exif = jpegInfo.exif,
    };
}

PrStatus append(PrImageReader* reader, PrByteView bytes, bool end, PrImageInfo* info) {
    JpegReader* jpeg = static_cast<JpegReader*>(reader);
    PrStatus status = prJpegDecoderAppend(jpeg->jpeg, bytes, end);
    if (prJpegDecoderInfo(jpeg->jpeg)->extent.x != 0)
        *info = infoOf(*prJpegDecoderInfo(jpeg->jpeg));
    if (status == PrStatus_Ok)
        jpeg->openStatus = PrStatus_Ok;
    return status;
}

uint64_t work(const PrImageReader* reader) {
    return prJpegDecoderWork(static_cast<const JpegReader*>(reader)->jpeg);
}

// A reader of appended bytes reads the image at its full extent, and has no previews.
PrStatus streamOpen(const PrImageCodec*, const PrImageLimits* limits, PrImageReader* PR_NULLABLE* PR_NONNULL out) {
    const PrJpegLimits jpegLimits{
        .pixels = limits->pixels,
        .workingBytes = limits->workingBytes,
        .outputBytes = limits->outputBytes,
        .work = limits->work,
        .metadataBytes = limits->metadataBytes,
    };
    PrJpegDecoder* jpeg = nullptr;
    PrStatus status = prJpegDecoderCreateStream(&jpegLimits, sizeof(JpegReader), &jpeg);
    if (status != PrStatus_Ok)
        return status;
    JpegReader* reader = allocZeroed<JpegReader>();
    reader->closeFn = close;
    reader->frameInfoFn = frameInfo;
    reader->readFn = read;
    reader->appendFn = append;
    reader->workFn = work;
    reader->jpeg = jpeg;
    reader->openStatus = PrStatus_Truncated;
    *out = reader;
    return PrStatus_Ok;
}

PrStatus open(const PrImageCodec*, const PrImageCodecSet*, const PrImageSource* source, const PrImageLimits* limits,
        PrImageReader* PR_NULLABLE* PR_NONNULL out, PrImageInfo* info) {
    *out = nullptr;
    uint8_t* bytes = nullptr;
    size_t size = 0;
    PrStatus status = imageReadSource(source, *limits, sizeof(JpegReader), &bytes, &size);
    if (status != PrStatus_Ok)
        return status;
    const PrJpegLimits jpegLimits{
        .pixels = limits->pixels,
        .workingBytes = limits->workingBytes,
        .outputBytes = limits->outputBytes,
        .work = limits->work,
        .metadataBytes = limits->metadataBytes,
    };
    PrJpegDecoder* jpeg = nullptr;
    status = prJpegDecoderCreate(PrByteView{bytes, size}, &jpegLimits, size + sizeof(JpegReader), &jpeg);
    if (jpeg == nullptr) {
        prMemoryFree(bytes);
        return status;
    }
    const PrJpegInfo& jpegInfo = *prJpegDecoderInfo(jpeg);
    ImageMetadata exif{};
    if (jpegInfo.exif.size != 0)
        imageReadExif(jpegInfo.exif.data, jpegInfo.exif.size, &exif);
    *info = infoOf(jpegInfo);
    JpegReader* reader = allocZeroed<JpegReader>();
    reader->closeFn = close;
    reader->frameInfoFn = frameInfo;
    reader->readFn = read;
    reader->previewExtentFn = previewExtent;
    reader->readPreviewFn = readPreview;
    reader->previewEmbeddedFn = previewEmbedded;
    // Each reduction, starting at a quarter, is a preview if it makes the image smaller than the previous reduction
    // does. A small image reaches its least extent before an eighth.
    PrUVec2 above = prJpegReducedExtent(jpegInfo.extent, kFirstPreviewReduction - 1);
    for (uint32_t reduction = kFirstPreviewReduction; reduction < kPrJpegReductionCount; ++reduction) {
        PrUVec2 extent = prJpegReducedExtent(jpegInfo.extent, reduction);
        if (extent == above)
            continue;
        reader->previews[info->previewCount++] = JpegPreview{reduction, extent};
        above = extent;
    }
    // The thumbnail is inserted by its size, so that the previews stay largest first.
    reader->thumbnail = thumbnailOpen(exif.thumbnail, jpegInfo, jpegLimits, size + sizeof(JpegReader));
    if (reader->thumbnail != nullptr) {
        PrUVec2 extent = prJpegDecoderInfo(reader->thumbnail)->extent;
        uint32_t at = 0;
        while (at < info->previewCount && reader->previews[at].extent.x >= extent.x)
            ++at;
        for (uint32_t i = info->previewCount; i > at; --i)
            reader->previews[i] = reader->previews[i - 1];
        reader->previews[at] = JpegPreview{0, extent};
        ++info->previewCount;
    }
    reader->bytes = bytes;
    reader->jpeg = jpeg;
    reader->openStatus = status;
    *out = reader;
    return status;
}

// The codec has no state, so one serves every call and nothing is destroyed.
PrImageCodec gCodec = {
    .headerBytesFn = headerBytes,
    .supportsTypeFn = supportsType,
    .recognizeFn = recognize,
    .decoderOpenFn = open,
    .streamOpenFn = streamOpen,
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrImageCodec* prImageJpegCreate(void) {
    return &gCodec;
}
