// Helpers for the tests of the built-in image codec.

#pragma once

#include "pr/alloc.h"
#include "pr/byte_view.h"
#include "pr/compress_builtin.h"
#include "pr/image_builtin.h"

#include <vector>

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Whether a read applies the image's orientation or keeps the stored one.
enum {
    Orientation_FromImage,     // the displayed image
    Orientation_None,          // the stored pixels
};
typedef uint8_t Orientation;

// A decoder kept open across the reads of the frames of an animation.
struct Animation {
    PrImageDecoder* decoder;
    Orientation orientation;
};

// The built-in codec that every test decodes through. It is created on first use and kept for the whole run.
inline PrImageCodec* testCodec() {
    static PrImageCodec* codec = prImageBuiltinCreate(prCompressBuiltinCreate());
    return codec;
}

// Creates a decoder of `codec` and opens `source` with it. Returns the status of the open. `*out` is the decoder if the
// status is Ok or Truncated, and null otherwise.
inline PrStatus testDecoderOpen(PrImageCodec* codec, PrImageSource source, const PrImageLimits* PR_NULLABLE limits,
                                PrImageDecoder* PR_NULLABLE* PR_NONNULL out) {
    PrImageDecoder* decoder = prImageDecoderCreate(codec);
    PrStatus status = prImageDecoderOpen(decoder, source, limits);
    if (status != PrStatus_Ok && status != PrStatus_Truncated) {
        prImageDecoderDestroy(decoder);
        decoder = nullptr;
    }
    *out = decoder;
    return status;
}

// Returns a decoder of the test codec with the image in `bytes` open, or null if the image does not open.
inline PrImageDecoder* PR_NULLABLE testImageOpen(const uint8_t* PR_NULLABLE bytes, size_t size) {
    if (bytes == nullptr && size != 0)
        return nullptr;
    PrImageSource source{};
    source.memory = PrByteView{bytes, size};
    PrImageDecoder* decoder = nullptr;
    testDecoderOpen(testCodec(), source, nullptr, &decoder);
    return decoder;
}

// Encodes straight-alpha Rgba8 `pixels` as a PNG through the test codec, and writes up to `capacity` bytes to `out`.
// Returns the size of the whole encoding, or zero if encoding fails.
inline size_t testEncodePng(PrPixelView pixels, uint8_t* PR_NULLABLE out, size_t capacity) {
    size_t size = 0;
    PrStatus status = prImageCodecEncode(testCodec(), "image/png", pixels, out, capacity, &size);
    return status == PrStatus_Ok ? size : 0;
}

// Decodes `frame` as Rgba8 with `alpha` into `size` bytes of packed `pixels`, in displayed orientation if `oriented`.
// Returns InvalidData if `size` is not the size of the frame.
inline PrStatus testReadRgba8(PrImageDecoder* decoder, uint32_t frame, bool oriented, PrImageAlpha alpha,
                              uint8_t* pixels, size_t size) {
    const PrImageInfo& info = *prImageDecoderInfo(decoder);
    const PrImageOrientation applied = oriented ? info.orientation : PrImageOrientation_Identity;
    const bool swap = prImageOrientationSwapsAxes(applied);
    const uint32_t width = swap ? info.extent.y : info.extent.x;
    const uint32_t height = swap ? info.extent.x : info.extent.y;
    if (size != size_t(width) * height * 4)
        return PrStatus_InvalidData;
    return prImageDecoderReadRgba8(decoder, frame, alpha, applied,
                                   PrPixels{pixels, size_t(width) * 4, {width, height}});
}

// Writes the width and height of the image in `bytes`, swapped if `orientation` applies one that transposes. Returns
// false if the image does not open.
inline bool imageSize(const uint8_t* PR_NULLABLE bytes, size_t size, Orientation orientation,
                      uint32_t* outWidth, uint32_t* outHeight) {
    PrImageDecoder* decoder = testImageOpen(bytes, size);
    if (decoder == nullptr) {
        return false;
    }
    const PrImageInfo& info = *prImageDecoderInfo(decoder);
    const bool swap =
        orientation == Orientation_FromImage && prImageOrientationSwapsAxes(info.orientation);
    *outWidth = swap ? info.extent.y : info.extent.x;
    *outHeight = swap ? info.extent.x : info.extent.y;
    prImageDecoderDestroy(decoder);
    return true;
}

// Writes to `*outDensity` the horizontal density of the image in `bytes`, in image pixels per CSS pixel. Returns false
// if the image does not open or has no resolution.
inline bool imageDensity(const uint8_t* PR_NULLABLE bytes, size_t size, float* outDensity) {
    PrImageDecoder* decoder = testImageOpen(bytes, size);
    if (decoder == nullptr) {
        return false;
    }
    const float pixelsPerMeter = prImageDecoderInfo(decoder)->pixelsPerMeter.x;
    prImageDecoderDestroy(decoder);
    if (pixelsPerMeter <= 0) {
        return false;
    }
    *outDensity = pixelsPerMeter / (96.0f / 0.0254f);
    return true;
}

// Decodes `frame` of the image in `bytes` into `pixels` as straight-alpha Rgba8. A 16-bit image that needs no
// reorientation is read in its own format, then narrowed to 8 bits. Returns false unless the read is Ok or Truncated.
inline bool decodeFrame(const uint8_t* PR_NULLABLE bytes, size_t size, Orientation orientation,
                        uint32_t frame, uint8_t* pixels, size_t pixelsSize) {
    PrImageDecoder* decoder = testImageOpen(bytes, size);
    if (decoder == nullptr) {
        return false;
    }
    const PrImageInfo& info = *prImageDecoderInfo(decoder);
    if (info.format.sample == PrImageSample_U16
        && (orientation == Orientation_None || info.orientation == PrImageOrientation_Identity)) {
        const size_t row = size_t(info.extent.x) * prImageFormatSize(&info.format);
        std::vector<uint16_t> native(row * info.extent.y / 2);
        const PrPixels output{reinterpret_cast<uint8_t*>(native.data()), row, info.extent};
        const PrStatus status = row != 0 ? prImageDecoderRead(decoder, frame, output) : PrStatus_InvalidData;
        const size_t pixelCount = size_t(info.extent.x) * info.extent.y;
        const uint32_t colors = info.format.model == PrColorModel_Gray ? 1 : 3;
        const uint32_t channels = colors + (info.format.alpha == PrImageAlpha_None ? 0 : 1);
        const bool usable =
            (status == PrStatus_Ok || status == PrStatus_Truncated) && pixelsSize == pixelCount * 4;
        for (size_t pixel = 0; usable && pixel < pixelCount; ++pixel) {
            const uint16_t* source = native.data() + pixel * channels;
            pixels[pixel * 4] = uint8_t(source[0] >> 8);
            pixels[pixel * 4 + 1] = uint8_t(source[colors == 1 ? 0 : 1] >> 8);
            pixels[pixel * 4 + 2] = uint8_t(source[colors == 1 ? 0 : 2] >> 8);
            pixels[pixel * 4 + 3] =
                info.format.alpha == PrImageAlpha_None ? 255 : uint8_t(source[colors] >> 8);
        }
        prImageDecoderDestroy(decoder);
        return usable;
    }
    const PrImageOrientation applied =
        orientation == Orientation_FromImage ? info.orientation : PrImageOrientation_Identity;
    const bool swap = prImageOrientationSwapsAxes(applied);
    const uint32_t width = swap ? info.extent.y : info.extent.x;
    const uint32_t height = swap ? info.extent.x : info.extent.y;
    if (pixelsSize != size_t(width) * height * 4) {
        prImageDecoderDestroy(decoder);
        return false;
    }
    const PrStatus status = prImageDecoderReadRgba8(decoder, frame, PrImageAlpha_Straight, applied,
                                                    PrPixels{pixels, size_t(width) * 4, {width, height}});
    prImageDecoderDestroy(decoder);
    return status == PrStatus_Ok || status == PrStatus_Truncated;
}

// Decodes the first frame in the same way as decodeFrame.
inline bool decodeImage(const uint8_t* PR_NULLABLE bytes, size_t size, Orientation orientation,
                        uint8_t* pixels, size_t pixelsSize) {
    return decodeFrame(bytes, size, orientation, 0, pixels, pixelsSize);
}

// Returns the number of frames of the image in `bytes`, and writes up to `count` frame durations, as encoded, to
// `outDelays`. Returns zero if the image does not open.
inline uint32_t imageFrames(const uint8_t* PR_NULLABLE bytes, size_t size,
                            uint32_t* PR_NULLABLE outDelays, uint32_t count) {
    PrImageDecoder* decoder = testImageOpen(bytes, size);
    if (decoder == nullptr) {
        return 0;
    }
    const uint32_t frames = prImageDecoderInfo(decoder)->frameCount;
    PrImageFrameInfo first = {};
    if (prImageDecoderFrameInfo(decoder, 0, &first) != PrStatus_Ok) {
        prImageDecoderDestroy(decoder);
        return 0;
    }
    for (uint32_t frame = 0; frame < frames && frame < count; ++frame) {
        PrImageFrameInfo info = {};
        prImageDecoderFrameInfo(decoder, frame, &info);
        outDelays[frame] = info.durationMs;
    }
    prImageDecoderDestroy(decoder);
    return frames;
}

// Opens the image in `bytes` as an animation, read in `orientation`. Returns null if the image does not open.
inline Animation* PR_NULLABLE animationOpen(const uint8_t* PR_NULLABLE bytes, size_t size,
                                            Orientation orientation) {
    PrImageDecoder* decoder = testImageOpen(bytes, size);
    if (decoder == nullptr) {
        return nullptr;
    }
    Animation* animation = alloc<Animation>();
    animation->decoder = decoder;
    animation->orientation = orientation;
    return animation;
}

// Closes the animation and frees it.
inline void animationClose(Animation* PR_NULLABLE animation) {
    if (animation != nullptr) {
        prImageDecoderDestroy(animation->decoder);
        prMemoryFree(animation);
    }
}

// Returns the number of frames of `animation`.
inline uint32_t animationFrames(const Animation* animation) {
    return prImageDecoderInfo(animation->decoder)->frameCount;
}

// Returns the duration of `frame` in milliseconds, as encoded, or zero for a frame that the animation does not have.
inline uint32_t animationDelay(const Animation* animation, uint32_t frame) {
    PrImageFrameInfo info = {};
    return prImageDecoderFrameInfo(animation->decoder, frame, &info) == PrStatus_Ok
               ? info.durationMs
               : 0;
}

// Returns the number of times the frames play, or zero for forever.
inline uint32_t animationLoops(const Animation* animation) {
    return prImageDecoderInfo(animation->decoder)->playCount;
}

// Returns the duration of one play, in milliseconds.
inline uint64_t animationDuration(const Animation* animation) {
    uint64_t duration = 0;
    for (uint32_t frame = 0; frame < animationFrames(animation); ++frame) {
        duration += animationDelay(animation, frame);
    }
    return duration;
}

// Returns the frame displayed at `milliseconds` after the animation started.
inline uint32_t animationFrameAt(const Animation* animation, double milliseconds) {
    const uint32_t frames = animationFrames(animation);
    const uint64_t duration = animationDuration(animation);
    if (frames < 2 || duration == 0 || !(milliseconds > 0) || milliseconds >= 1e15) {
        return 0;
    }
    uint64_t at = uint64_t(milliseconds);
    const uint32_t plays = animationLoops(animation);
    if (plays != 0 && at / duration >= plays) {
        return frames - 1;
    }
    at %= duration;
    for (uint32_t frame = 0; frame + 1 < frames; ++frame) {
        const uint32_t delay = animationDelay(animation, frame);
        if (at < delay) {
            return frame;
        }
        at -= delay;
    }
    return frames - 1;
}

// Returns true if no frame change follows `milliseconds`.
inline bool animationEnded(const Animation* animation, double milliseconds) {
    const uint64_t duration = animationDuration(animation);
    if (animationFrames(animation) < 2 || duration == 0) {
        return true;
    }
    const uint32_t plays = animationLoops(animation);
    return plays != 0 && milliseconds > 0
           && (milliseconds >= 1e15 || uint64_t(milliseconds) / duration >= plays);
}

// Decodes `frame` of `animation` into `pixels` as straight-alpha Rgba8. Returns false unless the read is Ok or
// Truncated.
inline bool animationFrame(Animation* animation, uint32_t frame, uint8_t* pixels,
                           size_t pixelsSize) {
    const PrImageInfo& info = *prImageDecoderInfo(animation->decoder);
    const PrImageOrientation applied =
        animation->orientation == Orientation_FromImage ? info.orientation : PrImageOrientation_Identity;
    const bool swap = prImageOrientationSwapsAxes(applied);
    const uint32_t width = swap ? info.extent.y : info.extent.x;
    const uint32_t height = swap ? info.extent.x : info.extent.y;
    if (pixelsSize != size_t(width) * height * 4)
        return false;
    const PrStatus status = prImageDecoderReadRgba8(animation->decoder, frame, PrImageAlpha_Straight, applied,
                                                    PrPixels{pixels, size_t(width) * 4, {width, height}});
    return status == PrStatus_Ok || status == PrStatus_Truncated;
}

} // namespace pr_impl

PR_ASSUME_NONNULL_END
