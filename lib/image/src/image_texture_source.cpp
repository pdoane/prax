// Images as texture sources. A source decodes the encoded bytes of an image to pixels.

#include "pr/image_texture_source.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/atomic.h"
#include "pr/color_space.h"
#include "pr/float16.h"
#include "pr/mem.h"
#include "pr/memory.h"
#include "pr/mutex.h"
#include "pr/texture_backend.h"

namespace pr_impl {

namespace {

// The duration, in milliseconds, below which a frame is too short to show, and the duration that it shows for instead.
// Many files state no duration, or one too short to show, and are made to play at that rate.
constexpr uint32_t kShortestFrame = 20;
constexpr uint32_t kDefaultFrame = 100;

// What the frames of an animated image share: when each frame ends, the time at which the animation began, and a
// decoder that stays open between reads. A GIF and an animated PNG store a frame as its difference from the frames
// before it, so a decoder that reads the frames in order composes each from the last, and one that starts over composes
// from the first. `mutex` guards the start, the decoder and the bytes that the decoder reads.
struct ImageFrames {
    Atomic<uint32_t> references;
    uint32_t count;
    uint32_t playCount;                     // the number of times that the frames play, or zero for forever
    uint64_t* ends;                         // when each frame ends, in milliseconds from the start of a play
    PrMutex mutex;
    bool started;
    double start;                           // the time of the first ask, in seconds
    PrBlob* PR_NULLABLE bytes;
    PrImageDecoder* PR_NULLABLE decoder;
};

void framesRelease(ImageFrames* PR_NULLABLE frames) {
    if (frames == nullptr || frames->references.fetchSub(1) != 1)
        return;
    prImageDecoderDestroy(frames->decoder);
    prBlobRelease(frames->bytes);
    prMutexReset(&frames->mutex);
    prMemoryFree(frames->ends);
    prMemoryFree(frames);
}

// The state of a source that decodes an image. It holds the image's bytes, the codec, the limits within which the image
// opens, and the image's info as the codec reported it when the source was created.
struct ImageTexture {
    const PrBlobSource* bytes;
    const PrImageCodecSet* codec;
    PrImageLimits limits;
    PrImageInfo image;
    bool converted;             // true if pixels are read as the codec converts them to sRGB, in 8 bits
    bool wide;                  // true if pixels are read as linear extended sRGB in half floats, from `space`
    PrColorSpace space;
    PrTextureChannels channels;
    PrTextureSample sample;
    PrTextureDesc desc;
    uint32_t preview;           // codec preview index plus one, or zero if the source reads the image

    // An animated image: what its frames share, and the frame that this source reads. `frames` is null for a still
    // image and for a preview.
    ImageFrames* PR_NULLABLE frames;
    uint32_t frame;
};

// A read in progress: the source, and the PrTextureRead that it completes.
struct ImageRead {
    const ImageTexture* image;
    PrTextureRead* read;
};

// The orientation flags of each EXIF orientation, indexed by PrImageOrientation.
constexpr PrTextureOrientationFlags kOrientations[] = {
    0,
    PrTextureOrientationFlag_FlipX,
    PrTextureOrientationFlag_FlipX | PrTextureOrientationFlag_FlipY,
    PrTextureOrientationFlag_FlipY,
    PrTextureOrientationFlag_Transpose,
    PrTextureOrientationFlag_Transpose | PrTextureOrientationFlag_FlipX,
    PrTextureOrientationFlag_Transpose | PrTextureOrientationFlag_FlipX | PrTextureOrientationFlag_FlipY,
    PrTextureOrientationFlag_Transpose | PrTextureOrientationFlag_FlipY,
};

// The texture sample of each image sample, indexed by PrImageSample.
constexpr PrTextureSample kSamples[] = {
    PrTextureSample_U8, PrTextureSample_U16, PrTextureSample_I16, PrTextureSample_F16, PrTextureSample_F32,
    PrTextureSample_U32,
};

// The formats that an image decodes to.
constexpr PrGpuFormat kDecoded[] = {PrGpuFormat_R8, PrGpuFormat_Rgba8, PrGpuFormat_Rgba16F, PrGpuFormat_Rgba32F};

// Returns the larger of `a` and `b`.
uint64_t larger(uint64_t a, uint64_t b) {
    return a > b ? a : b;
}

// Returns the limits within which the image of `inputBytes` bytes of a texture with `limits` opens. A frame has the
// pixels of a square of the extent, of the widest pixels, four 32-bit floats. A decoder holds the input and a few such
// frames, and takes a few steps for each sample. The image library's defaults are the least of each, which covers the
// state of the decoder of a small image. An image whose sides are within the extent therefore opens and reads.
PrImageLimits imageLimits(const PrTextureLimits& limits, uint64_t inputBytes) {
    PrImageLimits image = prImageLimitsDefault();
    const uint64_t pixels = uint64_t(limits.extent) * limits.extent;
    image.inputBytes = larger(image.inputBytes, inputBytes);
    image.pixels = pixels;
    image.workingBytes = larger(image.workingBytes, inputBytes + pixels * 16 * 8);
    image.outputBytes = larger(image.outputBytes, pixels * 16);
    image.work = larger(image.work, pixels * 64);
    return image;
}

// Opens the image in `bytes` with a decoder of `codec`, and stores the decoder at `*out`.
PrStatus imageOpen(const PrImageCodecSet* codec, PrByteView bytes, const PrImageLimits& limits,
        PrImageDecoder* PR_NULLABLE* PR_NONNULL out) {
    PrImageDecoder* decoder = prImageDecoderCreate(codec);
    PrImageSource source{};
    source.memory = bytes;
    PrStatus status = prImageDecoderOpen(decoder, source, &limits);
    if (status == PrStatus_Truncated) {
        // Some frame or level does not read, so the image is not a texture.
        status = PrStatus_InvalidData;
    }
    if (status != PrStatus_Ok) {
        prImageDecoderDestroy(decoder);
        decoder = nullptr;
    }
    *out = decoder;
    return status;
}

// Converts `count` texels of straight half floats in `space` to linear extended sRGB in place.
void toLinearSrgb(PrColorSpace space, uint8_t* texels, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        PrFloat16 texel[4];
        prMemCopy(texel, texels + i * 8, sizeof(texel));
        double channels[3];
        for (uint32_t k = 0; k < 3; ++k)
            channels[k] = double(prFloat16ToFloat(texel[k]));
        prColorConvert(space, PrColorSpace_SrgbLinear, channels, channels);
        for (uint32_t k = 0; k < 3; ++k)
            texel[k] = prFloat16FromFloat(float(channels[k]));
        prMemCopy(texels + i * 8, texel, sizeof(texel));
    }
}

// Decodes level `level` of frame `layer` at `stored`, in the image's format. If the source reads a preview, it decodes
// that preview instead.
PrStatus decodeStored(const ImageTexture& image, PrImageDecoder* decoder, uint32_t level, uint32_t layer, PrPixels stored) {
    if (image.preview != 0)
        return prImageDecoderReadPreview(decoder, image.preview - 1, stored);
    return prImageDecoderReadLevel(decoder, layer, level, stored);
}

// Decodes frame `layer` of a wide image at `pixels`, as linear extended sRGB in straight half floats.
PrStatus decodeWide(const ImageTexture& image, PrImageDecoder* decoder, uint32_t layer, PrPixels pixels) {
    const PrImageInfo& info = *prImageDecoderInfo(decoder);
    PrUVec2 extent = pixels.extent;
    size_t stride = size_t(extent.x) * prImageFormatSize(&info.format);
    uint8_t* stored = static_cast<uint8_t*>(prMemoryAlloc(stride * extent.y + 1));
    PrStatus status = decodeStored(image, decoder, 0, layer, PrPixels{stored, stride, extent});
    // The texture holds four channels, so the alpha of an opaque image converts to one.
    PrImageFormat wide{
        .model = PrColorModel_Rgb,
        .sample = PrImageSample_F16,
        .alpha = PrImageAlpha_Straight,
        .color = info.format.color,
    };
    if (status == PrStatus_Ok)
        status = prImageConvert(PrPixelView{stored, stride, extent}, &info.format, pixels, &wide, 0);
    if (status == PrStatus_Ok)
        toLinearSrgb(image.space, pixels.data, size_t(extent.x) * extent.y);
    prMemoryFree(stored);
    return status;
}

// Decodes level `level` of frame `layer` at `out`, in the source's format.
PrStatus decodeLevel(const ImageTexture& image, PrImageDecoder* decoder, uint32_t level, uint32_t layer, uint8_t* out) {
    PrUVec2 extent = prTextureLevelExtent(image.desc.extent, level);
    uint32_t texelBytes = prGpuFormatBlock(image.desc.format).bytes;
    PrPixels pixels{out, size_t(extent.x) * texelBytes, extent};
    if (image.wide)
        return decodeWide(image, decoder, layer, pixels);
    if (image.converted) {
        // The codec converts the first level to sRGB. The Srgb form of the texture holds those values.
        if (image.preview != 0) {
            return prImageDecoderReadPreviewRgba8(decoder, image.preview - 1, PrImageAlpha_Straight,
                PrImageOrientation_Identity, pixels);
        }
        return prImageDecoderReadRgba8(decoder, layer, PrImageAlpha_Straight, PrImageOrientation_Identity, pixels);
    }
    // Pixels that are stored in the layout of the format decode in place. A Srgb form holds the same bytes as its plain
    // form.
    PrGpuFormat plain = prGpuFormatPlain(image.desc.format);
    if (image.channels == PrTextureChannels_Rgba && image.sample == PrTextureSample_U8 && plain == PrGpuFormat_Rgba8)
        return decodeStored(image, decoder, level, layer, pixels);
    uint32_t pixelBytes = prTexturePixelSize(image.channels, image.sample);
    size_t stride = size_t(extent.x) * pixelBytes;
    uint8_t* stored = static_cast<uint8_t*>(prMemoryAlloc(stride * extent.y));
    PrStatus status = decodeStored(image, decoder, level, layer, PrPixels{stored, stride, extent});
    if (status == PrStatus_Ok)
        prTexturePixelsUnpack(PrPixelView{stored, stride, extent}, image.channels, image.sample, plain, pixels);
    prMemoryFree(stored);
    return status;
}

// Decodes frame `image.frame` of an animated image into `read`, with the decoder that the frames share. `blob` holds
// the image's bytes, which the decoder keeps reading.
PrStatus decodeFrame(const ImageTexture& image, PrBlob* blob, PrTextureRead* read) {
    ImageFrames& frames = *image.frames;
    prMutexLock(&frames.mutex);
    PrStatus status = PrStatus_Ok;
    if (frames.decoder == nullptr) {
        status = imageOpen(image.codec, prBlobBytes(blob), image.limits, &frames.decoder);
        if (status == PrStatus_Ok) {
            prBlobRetain(blob);
            frames.bytes = blob;
        }
    }
    if (status == PrStatus_Ok && prImageDecoderInfo(frames.decoder)->frameCount != image.image.frameCount)
        status = PrStatus_InvalidData;
    if (status == PrStatus_Ok) {
        const PrTextureReadDesc& asked = *prTextureReadInfo(read);
        uint8_t* out = prTextureReadStorage(read) + prTextureReadOffset(&image.desc, &asked, 0, 0);
        status = decodeLevel(image, frames.decoder, 0, image.frame, out);
    }
    prMutexUnlock(&frames.mutex);
    return status == PrStatus_Truncated ? PrStatus_InvalidData : status;
}

// Decodes the levels and layers that `read` asks for from `bytes`. `bytes` is the image's bytes from a second read of
// the blob.
PrStatus decodeRead(const ImageTexture& image, PrByteView bytes, PrTextureRead* read) {
    PrImageDecoder* decoder = nullptr;
    PrStatus status = imageOpen(image.codec, bytes, image.limits, &decoder);
    // The second read must return the image that the source describes.
    if (status == PrStatus_Ok) {
        const PrImageInfo& again = *prImageDecoderInfo(decoder);
        bool same = again.extent == image.image.extent && again.levelCount == image.image.levelCount &&
            again.frameCount == image.image.frameCount && again.format.sample == image.image.format.sample &&
            again.format.model == image.image.format.model && again.format.alpha == image.image.format.alpha;
        status = same ? PrStatus_Ok : PrStatus_InvalidData;
    }
    const PrTextureReadDesc& asked = *prTextureReadInfo(read);
    for (uint32_t level = asked.firstLevel; status == PrStatus_Ok && level < asked.firstLevel + asked.levelCount; ++level) {
        for (uint32_t layer = asked.firstLayer; status == PrStatus_Ok && layer < asked.firstLayer + asked.layerCount; ++layer) {
            uint8_t* out = prTextureReadStorage(read) + prTextureReadOffset(&image.desc, &asked, level, layer);
            status = decodeLevel(image, decoder, level, layer, out);
        }
    }
    prImageDecoderDestroy(decoder);
    return status == PrStatus_Truncated ? PrStatus_InvalidData : status;
}

void sourceRead(void* PR_NULLABLE context, PrTextureRead* read) {
    const ImageTexture* image = static_cast<const ImageTexture*>(context);
    ImageRead* held = static_cast<ImageRead*>(prMemoryAlloc(sizeof(ImageRead)));
    *held = ImageRead{image, read};
    PrBlobReadDesc bytes = prBlobReadDescDefault();
    bytes.done = [](void* PR_NULLABLE context, PrBlobRead* blob) {
        ImageRead held = *static_cast<ImageRead*>(context);
        prMemoryFree(context);
        PrStatus status = prBlobReadStatus(blob);
        // A frame after the first of an animation is read with the decoder that the frames share.
        if (status == PrStatus_Ok && held.image->frames != nullptr && held.image->frame != 0)
            status = decodeFrame(*held.image, prBlobReadBlob(blob), held.read);
        else if (status == PrStatus_Ok)
            status = decodeRead(*held.image, prBlobBytes(prBlobReadBlob(blob)), held.read);
        prBlobReadDestroy(blob);
        prTextureReadComplete(held.read, status);
    };
    bytes.context = held;
    prBlobReadCreate(image->bytes, &bytes);
}

void sourceRelease(void* PR_NULLABLE context) {
    ImageTexture* image = static_cast<ImageTexture*>(context);
    prBlobSourceRelease(image->bytes);
    framesRelease(image->frames);
    prMemoryFree(image);
}

const PrTextureSource* sourceOf(const ImageTexture& image);
const PrTextureSource* sourceWithPreviews(const ImageTexture& image,
    const PrTextureSource* PR_NONNULL const* PR_NULLABLE previews, uint32_t previewCount);

// Returns true if a source of `image` produces `format`: its own format, or for an image whose pixels the codec does
// not convert, the other formats that it decodes to (kDecoded).
bool produces(const ImageTexture& image, PrGpuFormat format) {
    if (format == image.desc.format)
        return true;
    if (image.converted || image.wide)
        return false;
    for (PrGpuFormat decoded : kDecoded) {
        if (decoded == format)
            return true;
    }
    return false;
}

PrStatus sourceReformat(void* PR_NULLABLE context, PrGpuFormat format, const PrTextureSource* PR_NULLABLE* PR_NONNULL out) {
    ImageTexture image = *static_cast<const ImageTexture*>(context);
    if (!produces(image, format))
        return PrStatus_NotSupported;
    image.desc.format = format;
    *out = sourceOf(image);
    return PrStatus_Ok;
}

// Answers a need with the image's own pixels, or with R8 for a need of the red channel of data alone.
PrStatus sourceAnswer(void* PR_NULLABLE context, const PrTextureSource* own, const PrTextureNeed* need,
        const PrTextureSource* PR_NULLABLE* PR_NONNULL out) {
    const ImageTexture& image = *static_cast<const ImageTexture*>(context);
    if (prTextureNeedSingleChannel(need) == 0 && image.desc.format != PrGpuFormat_R8 && produces(image, PrGpuFormat_R8)) {
        ImageTexture red = image;
        red.desc.format = PrGpuFormat_R8;
        red.frames = nullptr;
        *out = sourceOf(red);
        return PrStatus_Ok;
    }
    prTextureSourceRetain(own);
    *out = own;
    return PrStatus_Ok;
}

const PrTextureSource* sourceOf(const ImageTexture& image) {
    return sourceWithPreviews(image, nullptr, 0);
}

// Says which frame of an animated image shows at `time` (PrTextureSourceFrameFn). The animation begins at the time of
// the first ask. Its frames play in turn, as often as the image says, and then the last frame stays.
void sourceFrame(void* PR_NULLABLE context, double time, PrTextureFrame* out) {
    const ImageTexture& image = *static_cast<const ImageTexture*>(context);
    ImageFrames& frames = *image.frames;
    prMutexLock(&frames.mutex);
    if (!frames.started) {
        frames.started = true;
        frames.start = time;
    }
    double start = frames.start;
    prMutexUnlock(&frames.mutex);
    uint64_t total = frames.ends[frames.count - 1];
    double elapsed = time > start ? (time - start) * 1000 : 0;
    uint64_t plays = uint64_t(elapsed / double(total));
    uint32_t index = frames.count - 1;
    bool last = frames.playCount != 0 && plays >= frames.playCount;
    if (!last) {
        double within = elapsed - double(plays) * double(total);
        index = 0;
        while (index + 1 < frames.count && double(frames.ends[index]) <= within)
            ++index;
        out->next = start + (double(plays) * double(total) + double(frames.ends[index])) / 1000;
    }
    out->serial = uint64_t(index) + 1;
    // The source itself is the first frame.
    if (index != 0) {
        ImageTexture frame = image;
        frame.frame = index;
        out->source = sourceOf(frame);
    }
}

// Returns a texture source over a copy of `image`, with `previews`. The copy retains the image's bytes.
const PrTextureSource* sourceWithPreviews(const ImageTexture& image,
        const PrTextureSource* PR_NONNULL const* PR_NULLABLE previews, uint32_t previewCount) {
    ImageTexture* held = allocZeroed<ImageTexture>();
    *held = image;
    prBlobSourceRetain(image.bytes);
    if (image.frames != nullptr)
        image.frames->references.fetchAdd(1);
    // The source of a whole animated image changes over time. A frame and a preview are still.
    bool animated = image.frames != nullptr && image.frame == 0 && image.preview == 0;
    PrTextureSourceDesc desc{
        .texture = image.desc,
        .read = sourceRead,
        .reformat = sourceReformat,
        .answer = sourceAnswer,
        .previews = previews,
        .previewCount = previewCount,
        .frame = animated ? sourceFrame : nullptr,
        .release = sourceRelease,
        .context = held,
    };
    return prTextureSourceCreate(&desc);
}

// Returns a source of `image` with the previews that its codec reads. Only a 2D image of one level has previews. An
// image with levels of its own is a texture that a consumer samples by level.
const PrTextureSource* sourceWithDecoderPreviews(const ImageTexture& image, const PrImageDecoder* decoder) {
    const PrTextureSource* previews[kPrTextureSourceMaxPreviews];
    uint32_t count = 0;
    if (image.desc.kind == PrGpuTextureKind_2d && image.desc.layerCount == 1 && image.desc.levelCount == 1) {
        count = image.image.previewCount;
        if (count > kPrTextureSourceMaxPreviews)
            count = kPrTextureSourceMaxPreviews;
        // The codec lists its previews largest first. A source takes them smallest first.
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t codecPreview = image.image.previewCount - 1 - i;
            ImageTexture preview = image;
            preview.frames = nullptr;
            preview.preview = codecPreview + 1;
            preview.desc.extent = prImageDecoderPreviewExtent(decoder, codecPreview);
            previews[i] = sourceOf(preview);
        }
    }
    const PrTextureSource* source = sourceWithPreviews(image, previews, count);
    for (uint32_t i = 0; i < count; ++i)
        prTextureSourceRelease(previews[i]);
    return source;
}

// Returns the number of the image's mip levels, counted from the first, that halve as the levels of a texture do,
// rounding down.
uint32_t halvingLevels(const PrImageDecoder* decoder, const PrImageInfo& info) {
    if (info.levels != PrImageLevels_Mip)
        return 1;
    uint32_t count = 1;
    while (count < info.levelCount && count < uint32_t(kPrTextureMaxLevels) &&
            prImageDecoderLevelExtent(decoder, count) == prTextureLevelExtent(info.extent, count))
        ++count;
    return count;
}

// Returns true if `color` is sRGB. A color that says nothing is taken as sRGB.
bool isSrgb(const PrImageColor& color) {
    return color.encoding == PrImageColorEncoding_Unknown ||
        (color.encoding == PrImageColorEncoding_Named && color.space == PrColorSpace_Srgb);
}

// Creates a source over `bytes`, with its previews. `blob` describes the bytes. `color` says how the source treats the
// image's colors.
PrStatus sourceCreate(const PrBlobSource* bytes, const PrBlob* blob, const PrImageCodecSet* codec,
        const PrTextureLimits& limits, PrImageTextureColor color, const PrTextureSource* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    PrImageLimits imageBounds = imageLimits(limits, prBlobBytes(blob).size);
    PrImageDecoder* decoder = nullptr;
    PrStatus status = imageOpen(codec, prBlobBytes(blob), imageBounds, &decoder);
    if (status != PrStatus_Ok)
        return status;
    const PrImageInfo& info = *prImageDecoderInfo(decoder);
    const PrImageFormat& format = info.format;
    bool gray = format.model == PrColorModel_Gray;
    bool hasAlpha = format.alpha != PrImageAlpha_None;
    ImageTexture image{
        .bytes = bytes,
        .codec = codec,
        .limits = imageBounds,
        .image = info,
        .sample = kSamples[format.sample],
    };
    image.image.format.color.profile = PrByteView{};
    if (gray)
        image.channels = hasAlpha ? PrTextureChannels_GrayAlpha : PrTextureChannels_Gray;
    else
        image.channels = hasAlpha ? PrTextureChannels_Rgba : PrTextureChannels_Rgb;
    image.converted = (!gray && format.model != PrColorModel_Rgb) ||
        prImageFormatSize(&format) != prTexturePixelSize(image.channels, image.sample);
    bool wide = format.sample == PrImageSample_F16 || format.sample == PrImageSample_F32 ||
        format.sample == PrImageSample_U32;
    // For PrImageTextureColor_Srgb, an image in another named RGB space is converted to linear floats. An image of any
    // other color, including one with an ICC profile, is read as the codec converts it.
    bool sized = info.extent.x != 0 && info.extent.y != 0;
    if (color == PrImageTextureColor_Srgb && sized && !wide && !isSrgb(format.color) && !image.converted) {
        if (format.color.encoding == PrImageColorEncoding_Named &&
                prColorSpaceModel(format.color.space) == PrColorModel_Rgb) {
            image.wide = true;
            image.space = format.color.space;
        } else {
            image.converted = true;
        }
    }
    image.desc = PrTextureDesc{
        .kind = PrGpuTextureKind_2d,
        .extent = info.extent,
        .depth = 1,
        .layerCount = 1,
        .levelCount = halvingLevels(decoder, info),
        .format = PrGpuFormat(wide && !image.converted ? PrGpuFormat_Rgba32F : PrGpuFormat_Rgba8),
        .alpha = PrTextureAlpha(hasAlpha ? PrTextureAlpha_Straight : PrTextureAlpha_None),
        .orientation = kOrientations[info.orientation],
    };
    if (format.alpha == PrImageAlpha_Premultiplied)
        image.desc.alpha = PrTextureAlpha_Premultiplied;
    // The frames of a cube are its faces, one layer after another.
    if (info.faceCount == 6 && info.frameCount >= 6) {
        image.desc.kind = info.frameCount > 6 ? PrGpuTextureKind_CubeArray : PrGpuTextureKind_Cube;
        image.desc.layerCount = info.frameCount / 6 * 6;
    }
    // Channels that the codec converts become values of the sRGB transfer function, which are color.
    if (image.converted) {
        image.desc.levelCount = 1;
        image.desc.alpha = PrTextureAlpha_Straight;
        image.desc.format = PrGpuFormat_Rgba8Srgb;
    }
    if (image.wide) {
        image.desc.levelCount = 1;
        image.desc.format = PrGpuFormat_Rgba16F;
        image.desc.layerCount = 1;
        image.desc.kind = PrGpuTextureKind_2d;
    }
    // For PrImageTextureColor_Srgb, 8-bit values are sRGB values.
    if (color == PrImageTextureColor_Srgb && image.desc.format == PrGpuFormat_Rgba8)
        image.desc.format = PrGpuFormat_Rgba8Srgb;
    // A 2D image of several frames is an animation. Its frames show for the durations that the image states.
    if (!prTextureLimitsAdmit(&limits, &image.desc)) {
        prImageDecoderDestroy(decoder);
        return PrStatus_Limit;
    }
    if (image.desc.kind == PrGpuTextureKind_2d && info.faceCount == 1 && info.frameCount > 1) {
        ImageFrames* frames = allocZeroed<ImageFrames>();
        frames->references.storeRelaxed(1);
        prMutexInit(&frames->mutex);
        frames->count = info.frameCount;
        frames->playCount = info.playCount;
        frames->ends = static_cast<uint64_t*>(prMemoryAlloc(info.frameCount * sizeof(uint64_t)));
        uint64_t end = 0;
        for (uint32_t frame = 0; frame < info.frameCount; ++frame) {
            PrImageFrameInfo timing{};
            prImageDecoderFrameInfo(decoder, frame, &timing);
            end += timing.durationMs < kShortestFrame ? kDefaultFrame : timing.durationMs;
            frames->ends[frame] = end;
        }
        image.frames = frames;
    }
    *out = sourceWithDecoderPreviews(image, decoder);
    framesRelease(image.frames);
    prImageDecoderDestroy(decoder);
    return PrStatus_Ok;
}

// The image texture format, with the codec that decodes its images and the color treatment of its sources.
struct ImageTextureFormat : PrTextureFormat {
    const PrImageCodecSet* codec;
    PrImageTextureColor color;
};

bool formatSupportsType(const PrTextureFormat* format, PrStringView type) {
    return prImageCodecSetSupportsType(static_cast<const ImageTextureFormat*>(format)->codec, type);
}

PrStatus formatCreate(const PrTextureFormat* format, const PrBlobSource* bytes, const PrBlob* blob,
        const PrTextureLimits* limits, const PrTextureSource* PR_NULLABLE* PR_NONNULL out) {
    const ImageTextureFormat* image = static_cast<const ImageTextureFormat*>(format);
    return sourceCreate(bytes, blob, image->codec, *limits, image->color, out);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrStatus prImageTextureSourceOpen(const PrBlobSource* bytes, const PrImageCodecSet* codec,
        const PrTextureLimits* PR_NULLABLE limits, PrImageTextureColor color,
        const PrTextureSource* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    PrBlob* blob = nullptr;
    PrStatus status = prBlobSourceRead(bytes, prBlobRangeWhole(), &blob);
    if (status != PrStatus_Ok)
        return status;
    PrTextureLimits bounds = limits != nullptr ? *limits : prTextureLimitsDefault();
    status = sourceCreate(bytes, blob, codec, bounds, color, out);
    prBlobRelease(blob);
    return status;
}

PrStatus prImageTextureSourceOpenBytes(PrByteView bytes, const PrImageCodecSet* codec,
        const PrTextureLimits* PR_NULLABLE limits,
        PrImageTextureColor color, const PrTextureSource* PR_NULLABLE* PR_NONNULL out) {
    PrBlobSource* source = prBlobSourceCreateBlob(prBlobCopy(bytes));
    PrStatus status = prImageTextureSourceOpen(source, codec, limits, color, out);
    prBlobSourceRelease(source);
    return status;
}

PrTextureFormat* prImageTextureFormatCreate(const PrImageCodecSet* codec, PrImageTextureColor color) {
    ImageTextureFormat* format = allocZeroed<ImageTextureFormat>();
    format->destroyFn = [](PrTextureFormat* made) { prMemoryFree(static_cast<ImageTextureFormat*>(made)); };
    format->supportsTypeFn = formatSupportsType;
    format->createFn = formatCreate;
    format->codec = codec;
    format->color = color;
    return format;
}
