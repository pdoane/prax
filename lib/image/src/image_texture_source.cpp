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
#include "pr/raw_array.h"
#include "pr/texture_backend.h"
#include "pr/time.h"

namespace pr_impl {

namespace {

// The duration, in milliseconds, below which a frame is too short to show, and the duration that it shows for instead.
// Many files state no duration, or one too short to show, and are made to play at that rate.
constexpr uint32_t kShortestFrame = 20;
constexpr uint32_t kDefaultFrame = 100;

// The number of decoders that the frames of an animated image keep open between reads.
constexpr uint32_t kFrameDecoders = 4;

// A decoder that the frames of an animated image keep open, the bytes that it reads, and the frame after the last that
// it decoded. A read takes it for itself while it decodes, so no two reads share a decoder at once.
struct FrameDecoder {
    PrImageDecoder* PR_NULLABLE decoder;
    PrBlob* PR_NULLABLE bytes;
    uint64_t fed;                           // the bytes appended to a decoder of arriving bytes
    uint32_t next;
    bool busy;
    uint64_t used;                          // the value of `uses` when it was last taken
};

// What the frames of an animated image share: when each frame ends, and decoders that stay open between reads. A GIF
// and an animated PNG store a frame as its difference from the frames before it, so a decoder that reads the frames in
// order composes each from the last, and one that starts over composes from the first. A read of a frame takes the
// decoder that last decoded the nearest frame before it, so that consumers at different times each keep their own
// decoder moving forward. `mutex` guards the decoders.
struct ImageFrames {
    Atomic<uint32_t> references;
    uint32_t count;
    uint32_t playCount;                     // the number of times that the frames play, or zero for forever
    uint64_t* ends;                         // when each frame ends, in milliseconds from the start of a play
    PrMutex mutex;
    FrameDecoder decoders[kFrameDecoders];
    uint64_t uses;
};

void framesRelease(ImageFrames* PR_NULLABLE frames) {
    if (frames == nullptr || frames->references.fetchSub(1) != 1)
        return;
    for (FrameDecoder& decoder : frames->decoders) {
        prImageDecoderDestroy(decoder.decoder);
        prBlobRelease(decoder.bytes);
    }
    prMutexReset(&frames->mutex);
    prMemoryFree(frames->ends);
    prMemoryFree(frames);
}

// What the sources of an image whose bytes are still arriving share: a decoder of the image as a stream, to which each
// byte is appended once as it arrives (prImageDecoderAppend), and the picture of the last revision. A revision is a
// snapshot of what the decoder holds, taken when a consumer asks what the source shows after the decoder changed, so
// revisions follow the consumer's frames, not the arrival of the bytes. `notifier` is notified once after the decoder
// changes, and again only after a consumer has asked. Once the bytes are complete, the picture is the image of every
// byte, and an animation's frames play from the decoders that `frames` keeps. `mutex` guards the decoder and the
// revision.
struct ImageStream {
    Atomic<uint32_t> references;
    PrMutex mutex;
    const PrBlobSource* bytes;
    PrImageDecoder* decoder;
    PrNotifier* notifier;
    uint64_t observer;
    uint64_t fed;                           // the bytes appended to the decoder
    bool changed;                           // the decoder changed since the last revision
    bool notified;                          // the notifier was notified since a consumer last asked
    uint32_t revision;
    bool complete;                          // the decoder has the whole image, or failed
    bool ended;                             // the bytes are complete, so the source itself reads them without waiting
    PrStatus status;                        // why the bytes failed, or InvalidData for bytes that do not decode
    uint32_t frameCount;                    // the frames that have begun, at least one
    uint32_t playCount;
    RawArray<uint64_t> ends;                    // when each of those frames ends, in milliseconds from the start
    PrImageLimits bounds;                       // what a decoder of the stream's bytes is opened within
    ImageFrames* PR_NULLABLE frames;            // the frames of a complete animation

    // The decoders that read the whole frames of an animation while its bytes arrive. Each is a decoder of the bytes as
    // a stream too, appended what has arrived when a read takes it, so that consumers that play the frames each keep a
    // decoder moving forward, as the decoders of a complete animation do.
    ImageFrames* PR_NULLABLE arriving;
    const PrTextureSource* PR_NULLABLE picture; // the last revision, and its serial
    uint64_t pictureSerial;

    // The image of every byte of a still image, once they have ended: a source of the bytes as a whole file opens them,
    // with the previews that such an open has, the decodes of the image's data among them. `color` is how the sources
    // treat the image's color.
    const PrTextureSource* PR_NULLABLE whole;
    PrImageTextureColor color;
};

void streamRelease(ImageStream* PR_NULLABLE stream) {
    if (stream == nullptr || stream->references.fetchSub(1) != 1)
        return;
    prNotifierUnobserve(prBlobSourceNotifier(stream->bytes), stream->observer);
    prImageDecoderDestroy(stream->decoder);
    prTextureSourceRelease(stream->picture);
    prTextureSourceRelease(stream->whole);
    prBlobSourceRelease(stream->bytes);
    prNotifierRelease(stream->notifier);
    framesRelease(stream->frames);
    framesRelease(stream->arriving);
    stream->ends.reset();
    prMutexReset(&stream->mutex);
    prMemoryFree(stream);
}

// The state of a source that decodes an image. It holds the image's bytes, the codec, the limits of the texture, and
// the image's info as the codec reported it when the source was created.
struct ImageTexture {
    const PrBlobSource* bytes;
    const PrImageCodecSet* codec;
    PrTextureLimits limits;
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

    // An image whose bytes were arriving when it opened: what its sources share. `arrivingFrame` is set for a source of
    // a whole frame of the animation while its bytes arrive, which reads with the stream's decoders of its frames.
    ImageStream* PR_NULLABLE stream;
    bool arrivingFrame;
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

// Opens the image in `bytes` with a decoder of `codec`, within the limits of a texture with `limits`, and stores the
// decoder at `*out`. If `partial` is set, the bytes are the first of an image, and an image that opens from incomplete
// input opens.
PrStatus imageOpen(const PrImageCodecSet* codec, PrByteView bytes, const PrTextureLimits& limits, bool partial,
        PrImageDecoder* PR_NULLABLE* PR_NONNULL out) {
    PrImageDecoder* decoder = prImageDecoderCreate(codec);
    PrImageSource source{};
    source.memory = bytes;
    PrImageLimits bounds = imageLimits(limits, bytes.size);
    PrStatus status = prImageDecoderOpen(decoder, source, &bounds);
    if (status == PrStatus_Truncated) {
        // Of complete bytes, some frame or level does not read, so the image is not a texture.
        status = partial ? PrStatus_Ok : PrStatus_InvalidData;
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
    PrStatus decoded = status;
    if (status == PrStatus_Ok || status == PrStatus_Truncated)
        status = prImageConvert(PrPixelView{stored, stride, extent}, &info.format, pixels, &wide, 0);
    if (status == PrStatus_Ok) {
        toLinearSrgb(image.space, pixels.data, size_t(extent.x) * extent.y);
        status = decoded;
    }
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
    // Pixels that the bytes cut short are unpacked as far as the codec decoded them.
    if (status == PrStatus_Ok || status == PrStatus_Truncated)
        prTexturePixelsUnpack(PrPixelView{stored, stride, extent}, image.channels, image.sample, plain, pixels);
    prMemoryFree(stored);
    return status;
}

// Takes a decoder of the frames for a read of frame `frame`: the idle open decoder whose next frame is the latest at or
// before `frame`, then an empty one, then the idle one used longest ago, which starts over. Returns null if every
// decoder is busy. Called with the frames' mutex held.
FrameDecoder* PR_NULLABLE takeDecoder(ImageFrames& frames, uint32_t frame) {
    FrameDecoder* best = nullptr;
    for (FrameDecoder& decoder : frames.decoders) {
        bool behind = !decoder.busy && decoder.decoder != nullptr && decoder.next <= frame;
        if (behind && (best == nullptr || decoder.next > best->next))
            best = &decoder;
    }
    for (uint32_t i = 0; best == nullptr && i < kFrameDecoders; ++i) {
        if (!frames.decoders[i].busy && frames.decoders[i].decoder == nullptr)
            best = &frames.decoders[i];
    }
    for (FrameDecoder& decoder : frames.decoders) {
        bool older = best == nullptr || (best->decoder != nullptr && best->next > frame && decoder.used < best->used);
        if (!decoder.busy && older)
            best = &decoder;
    }
    if (best != nullptr) {
        best->busy = true;
        best->used = ++frames.uses;
    }
    return best;
}

// Decodes frame `image.frame` of an animated image into `read`, with a decoder that the frames keep open. `blob` holds
// the image's bytes, which the decoder keeps reading.
PrStatus decodeFrame(const ImageTexture& image, PrBlob* blob, PrTextureRead* read) {
    ImageFrames& frames = *image.frames;
    prMutexLock(&frames.mutex);
    FrameDecoder* kept = takeDecoder(frames, image.frame);
    prMutexUnlock(&frames.mutex);
    // A read that finds every decoder busy decodes with one of its own.
    FrameDecoder own{};
    FrameDecoder& taken = kept != nullptr ? *kept : own;
    PrStatus status = PrStatus_Ok;
    if (taken.decoder == nullptr) {
        status = imageOpen(image.codec, prBlobBytes(blob), image.limits, false, &taken.decoder);
        if (status == PrStatus_Ok) {
            prBlobRetain(blob);
            taken.bytes = blob;
            taken.next = 0;
        }
    }
    if (status == PrStatus_Ok && prImageDecoderInfo(taken.decoder)->frameCount != image.image.frameCount)
        status = PrStatus_InvalidData;
    if (status == PrStatus_Ok) {
        const PrTextureReadDesc& asked = *prTextureReadInfo(read);
        uint8_t* out = prTextureReadStorage(read) + prTextureReadOffset(&image.desc, &asked, 0, 0);
        status = decodeLevel(image, taken.decoder, 0, image.frame, out);
    }
    if (kept == nullptr || status != PrStatus_Ok) {
        // A decoder that failed starts over at its next read.
        prImageDecoderDestroy(taken.decoder);
        prBlobRelease(taken.bytes);
        taken.decoder = nullptr;
        taken.bytes = nullptr;
    }
    taken.next = image.frame + 1;
    if (kept != nullptr) {
        prMutexLock(&frames.mutex);
        kept->busy = false;
        prMutexUnlock(&frames.mutex);
    }
    return status == PrStatus_Truncated ? PrStatus_InvalidData : status;
}

// Decodes the levels and layers that `read` asks for from `bytes`. `bytes` is the image's bytes from a second read of
// the blob.
PrStatus decodeRead(const ImageTexture& image, PrByteView bytes, PrTextureRead* read) {
    PrImageDecoder* decoder = nullptr;
    PrStatus status = imageOpen(image.codec, bytes, image.limits, false, &decoder);
    // The second read must return the image that the source describes. The frames of an image whose bytes arrive grow
    // as they do.
    if (status == PrStatus_Ok) {
        const PrImageInfo& again = *prImageDecoderInfo(decoder);
        bool frames = image.stream != nullptr || again.frameCount == image.image.frameCount;
        bool same = again.extent == image.image.extent && again.levelCount == image.image.levelCount && frames &&
            again.format.sample == image.image.format.sample && again.format.model == image.image.format.model &&
            again.format.alpha == image.image.format.alpha;
        status = same ? PrStatus_Ok : PrStatus_InvalidData;
    }
    const PrTextureReadDesc& asked = *prTextureReadInfo(read);
    for (uint32_t level = asked.firstLevel; status == PrStatus_Ok && level < asked.firstLevel + asked.levelCount; ++level) {
        for (uint32_t layer = asked.firstLayer; status == PrStatus_Ok && layer < asked.firstLayer + asked.layerCount; ++layer) {
            uint8_t* out = prTextureReadStorage(read) + prTextureReadOffset(&image.desc, &asked, level, layer);
            status = decodeLevel(image, decoder, level, image.frame + layer, out);
        }
    }
    prImageDecoderDestroy(decoder);
    return status == PrStatus_Truncated ? PrStatus_InvalidData : status;
}

// Decodes whole frame `image.frame` of an animation whose bytes are arriving into `read`, with a decoder of the bytes
// that the stream keeps (ImageStream::arriving). The decoder takes the bytes that have arrived since it last read, so
// it reads no byte twice, and the frame is whole in them. Nothing waits for bytes.
PrStatus decodeArrivingFrame(const ImageTexture& image, PrTextureRead* read) {
    ImageStream& stream = *image.stream;
    ImageFrames& frames = *stream.arriving;
    prMutexLock(&frames.mutex);
    FrameDecoder* kept = takeDecoder(frames, image.frame);
    prMutexUnlock(&frames.mutex);
    FrameDecoder own{};
    FrameDecoder& taken = kept != nullptr ? *kept : own;
    PrStatus status = PrStatus_Ok;
    if (taken.decoder == nullptr) {
        taken.decoder = prImageDecoderCreate(image.codec);
        status = prImageDecoderOpenStream(taken.decoder, &stream.bounds);
        taken.fed = 0;
        taken.next = 0;
    }
    const PrBlobProgress progress = prBlobSourceProgress(stream.bytes);
    if (status == PrStatus_Ok && progress.available > taken.fed) {
        PrBlob* blob = nullptr;
        status = prBlobSourceRead(stream.bytes, PrBlobRange{taken.fed, progress.available - taken.fed}, &blob);
        if (status == PrStatus_Ok) {
            const bool end = progress.complete && progress.status == PrStatus_Ok;
            status = prImageDecoderAppend(taken.decoder, prBlobBytes(blob), end);
            taken.fed = progress.available;
        }
        prBlobRelease(blob);
    }
    if (status == PrStatus_Truncated)
        status = PrStatus_Ok;
    if (status == PrStatus_Ok && prImageDecoderInfo(taken.decoder)->frameCount <= image.frame)
        status = PrStatus_InvalidData;
    if (status == PrStatus_Ok) {
        const PrTextureReadDesc& asked = *prTextureReadInfo(read);
        uint8_t* out = prTextureReadStorage(read) + prTextureReadOffset(&image.desc, &asked, 0, 0);
        status = decodeLevel(image, taken.decoder, 0, image.frame, out);
        // The frame is whole, though the bytes after it have not all arrived.
        if (status == PrStatus_Truncated)
            status = PrStatus_Ok;
    }
    if (kept == nullptr || status != PrStatus_Ok) {
        prImageDecoderDestroy(taken.decoder);
        taken.decoder = nullptr;
        taken.fed = 0;
    }
    taken.next = image.frame + 1;
    if (kept != nullptr) {
        prMutexLock(&frames.mutex);
        kept->busy = false;
        prMutexUnlock(&frames.mutex);
    }
    return status;
}

// Decodes an embedded preview of an image whose bytes are arriving into `read`, from the bytes that have arrived, which
// hold it. Nothing waits for bytes.
PrStatus decodeArrivedPreview(const ImageTexture& image, PrTextureRead* read) {
    const PrBlobProgress progress = prBlobSourceProgress(image.bytes);
    PrBlob* blob = nullptr;
    PrStatus status = prBlobSourceRead(image.bytes, PrBlobRange{0, progress.available}, &blob);
    PrImageDecoder* decoder = nullptr;
    if (status == PrStatus_Ok)
        status = imageOpen(image.codec, prBlobBytes(blob), image.limits, !progress.complete, &decoder);
    if (status == PrStatus_Ok && prImageDecoderInfo(decoder)->previewCount < image.preview)
        status = PrStatus_InvalidData;
    if (status == PrStatus_Ok) {
        const PrTextureReadDesc& asked = *prTextureReadInfo(read);
        uint8_t* out = prTextureReadStorage(read) + prTextureReadOffset(&image.desc, &asked, 0, 0);
        status = decodeLevel(image, decoder, 0, 0, out);
    }
    prImageDecoderDestroy(decoder);
    prBlobRelease(blob);
    return status == PrStatus_Truncated ? PrStatus_InvalidData : status;
}

void sourceRead(void* PR_NULLABLE context, PrTextureRead* read) {
    const ImageTexture* image = static_cast<const ImageTexture*>(context);
    if (image->arrivingFrame) {
        prTextureReadComplete(read, decodeArrivingFrame(*image, read));
        return;
    }
    if (image->stream != nullptr && image->preview != 0) {
        prTextureReadComplete(read, decodeArrivedPreview(*image, read));
        return;
    }
    ImageRead* held = static_cast<ImageRead*>(prMemoryAlloc(sizeof(ImageRead)));
    *held = ImageRead{image, read};
    // A source of an image whose bytes are arriving reads every byte, which waits for them: a consumer shows its
    // revisions meanwhile (prTextureSourceShow).
    PrBlobReadDesc bytes = prBlobReadDescDefault();
    bytes.done = [](void* PR_NULLABLE context, PrBlobRead* blob) {
        ImageRead held = *static_cast<ImageRead*>(context);
        prMemoryFree(context);
        PrStatus status = prBlobReadStatus(blob);
        // A frame after the first of an animation is read with a decoder that the frames keep.
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
    streamRelease(image->stream);
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
        // A whole frame of an arriving animation reads with the stream's decoders.
        if (!image.arrivingFrame)
            red.stream = nullptr;
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

uint32_t frameAt(const uint64_t* ends, uint32_t count, uint32_t plays, bool complete, double elapsed, double* next);

// Says which frame of an animated image shows at `time` on a timeline. The animation starts at time zero. Its frames
// play in turn, as often as the image says, and then the last frame stays. The answer depends on the time alone.
void animationShow(const ImageTexture& image, double time, PrTextureShow* out) {
    const ImageFrames& frames = *image.frames;
    double elapsed = time > 0 ? time * 1000 : 0;
    uint32_t index = frameAt(frames.ends, frames.count, frames.playCount, true, elapsed, &out->next);
    out->serial = uint64_t(index) << 32 | 0xffffffffu;
    // The source itself is the first frame.
    if (index != 0) {
        ImageTexture frame = image;
        frame.frame = index;
        out->picture = sourceOf(frame);
    }
}

// Reads when each frame of the decoder that has begun ends into the stream. Called with the stream's mutex held.
void readStreamFrames(ImageStream& s) {
    PrImageDecoder* decoder = s.decoder;
    const PrImageInfo& info = *prImageDecoderInfo(decoder);
    const uint32_t count = info.frameCount != 0 ? info.frameCount : 1;
    uint64_t end = s.ends.count != 0 ? s.ends.data[s.ends.count - 1] : 0;
    for (uint32_t frame = s.ends.count; frame < count; ++frame) {
        PrImageFrameInfo timing{};
        prImageDecoderFrameInfo(decoder, frame, &timing);
        end += timing.durationMs < kShortestFrame ? kDefaultFrame : timing.durationMs;
        s.ends.push(end);
    }
    s.frameCount = count;
    s.playCount = info.playCount;
}

// Appends the bytes that have arrived since the last append to the stream's decoder, each once, and ends the stream
// when the bytes are complete or fail. Sets `changed` if the decoder or the stream changed. Called with the mutex held.
void streamAppend(ImageStream& s) {
    const PrBlobProgress progress = prBlobSourceProgress(s.bytes);
    if (progress.complete && !s.ended) {
        s.ended = true;
        s.changed = true;
    }
    if (s.complete)
        return;
    const bool end = progress.complete && progress.status == PrStatus_Ok;
    if (progress.available > s.fed || end) {
        PrBlob* blob = nullptr;
        PrByteView appended{};
        if (progress.available > s.fed) {
            if (prBlobSourceRead(s.bytes, PrBlobRange{s.fed, progress.available - s.fed}, &blob) != PrStatus_Ok)
                return;
            appended = prBlobBytes(blob);
        }
        PrStatus status = prImageDecoderAppend(s.decoder, appended, end);
        prBlobRelease(blob);
        s.fed = progress.available;
        s.changed = true;
        if (prImageDecoderInfo(s.decoder)->extent.x != 0)
            readStreamFrames(s);
        if (status == PrStatus_Ok) {
            s.complete = true;
        } else if (status != PrStatus_Truncated || end) {
            // Complete bytes that do not decode keep the last revision, as bytes that fail do.
            s.complete = true;
            s.status = status == PrStatus_Truncated ? PrStatus_InvalidData : status;
        }
        if (s.complete && s.status == PrStatus_Ok && s.frameCount > 1) {
            ImageFrames* frames = allocZeroed<ImageFrames>();
            frames->references.storeRelaxed(1);
            prMutexInit(&frames->mutex);
            frames->count = s.frameCount;
            frames->playCount = s.playCount;
            frames->ends = static_cast<uint64_t*>(prMemoryAlloc(s.frameCount * sizeof(uint64_t)));
            prMemCopy(frames->ends, s.ends.data, s.frameCount * sizeof(uint64_t));
            s.frames = frames;
        }
    }
    if (progress.status != PrStatus_Ok && !s.complete) {
        s.complete = true;
        s.status = progress.status;
        s.changed = true;
    }
}

// Observes the bytes of a stream: appends what arrived, and notifies the sources' notifier once after a change, until a
// consumer asks what the source shows.
void streamChanged(void* PR_NULLABLE context) {
    ImageStream& stream = *static_cast<ImageStream*>(context);
    prMutexLock(&stream.mutex);
    streamAppend(stream);
    bool notify = stream.changed && !stream.notified;
    stream.notified = stream.notified || notify;
    prMutexUnlock(&stream.mutex);
    if (notify)
        prNotifierNotify(stream.notifier);
}

// Returns the frame of `count`, whose ends are `ends`, that shows `elapsed` milliseconds into the animation, and writes
// when it ends to `*next`, in seconds, or kPrF64Infinity. A complete animation plays `plays` times, or forever if it is
// zero, and then its last frame stays. The frames of an incomplete one play once, and its last frame waits for more.
uint32_t frameAt(const uint64_t* ends, uint32_t count, uint32_t plays, bool complete, double elapsed, double* next) {
    *next = kPrF64Infinity;
    uint64_t total = ends[count - 1];
    uint64_t played = complete ? uint64_t(elapsed / double(total)) : 0;
    if (complete && plays != 0 && played >= plays)
        return count - 1;
    double within = elapsed - double(played) * double(total);
    uint32_t index = 0;
    while (index + 1 < count && double(ends[index]) <= within)
        ++index;
    if (complete || index + 1 < count)
        *next = (double(played) * double(total) + double(ends[index])) / 1000;
    return index;
}

// Returns a still source of what the stream's decoder holds of frame `frame`: a snapshot of its pixels in the source's
// format, with the source's description. Called with the stream's mutex held.
const PrTextureSource* snapshotOf(const ImageTexture& image, uint32_t frame) {
    PrTextureDesc desc = image.desc;
    desc.kind = PrGpuTextureKind_2d;
    desc.levelCount = 1;
    desc.layerCount = 1;
    const size_t size = prTextureLevelSize(&desc, 0);
    uint8_t* pixels = static_cast<uint8_t*>(prMemoryAllocZeroed(size));
    // A decoder that has not reached the image's pixels yet shows nothing.
    PrStatus status = PrStatus_Truncated;
    if (prImageDecoderInfo(image.stream->decoder)->extent.x != 0)
        status = decodeLevel(image, image.stream->decoder, 0, frame, pixels);
    // What does not decode yet shows as transparent black.
    if (status != PrStatus_Ok && status != PrStatus_Truncated)
        prMemZero(pixels, size);
    const size_t stride = size_t(desc.extent.x) * prGpuFormatBlock(desc.format).bytes;
    const PrTextureSource* picture = prTextureSourceCreatePixels(&desc, PrPixelView{pixels, stride, desc.extent});
    prMemoryFree(pixels);
    return picture;
}

PrStatus sourceCreate(const PrBlobSource* bytes, const PrBlob* blob, const PrImageCodecSet* codec,
    const PrTextureLimits& limits, PrImageTextureColor color, bool partial,
    const PrTextureSource* PR_NULLABLE* PR_NONNULL out);

// Returns the image of every byte of the stream of a still image, made at its first use, or null if the bytes do not
// open whole. Called with the stream's mutex held.
const PrTextureSource* PR_NULLABLE wholeOf(const ImageTexture& image) {
    ImageStream& stream = *image.stream;
    if (stream.whole != nullptr)
        return stream.whole;
    PrBlob* blob = nullptr;
    if (prBlobSourceRead(stream.bytes, prBlobRangeWhole(), &blob) == PrStatus_Ok) {
        const PrTextureSource* made = nullptr;
        if (sourceCreate(stream.bytes, blob, image.codec, image.limits, stream.color, false, &made) == PrStatus_Ok) {
            // The whole file describes the texture that the revisions showed.
            const PrTextureDesc& desc = *prTextureSourceDesc(made);
            const bool same = desc.extent == image.desc.extent && desc.format == image.desc.format &&
                desc.alpha == image.desc.alpha && desc.orientation == image.desc.orientation &&
                desc.levelCount == image.desc.levelCount;
            if (same)
                stream.whole = made;
            else
                prTextureSourceRelease(made);
        }
    }
    prBlobRelease(blob);
    return stream.whole;
}

// Says what an image whose bytes were arriving when it opened shows at `time` (PrTextureSourceShowFn): a revision of
// what has arrived, and once the bytes are complete, the image of every byte. The frames of an animation that have
// begun play, and a frame shows complete once a frame after it begins. A picture that is not complete changes when the
// stream notifies.
void streamShow(const ImageTexture& image, double time, PrTextureShow* out) {
    ImageStream& stream = *image.stream;
    prMutexLock(&stream.mutex);
    // The image of every byte shows once the bytes have ended, and the decoder's whole image before that.
    const bool decoded = stream.complete && stream.status == PrStatus_Ok;
    const bool whole = decoded && stream.ended;
    const uint32_t frames = stream.frameCount != 0 ? stream.frameCount : 1;
    double elapsed = time > 0 ? time * 1000 : 0;
    double next = kPrF64Infinity;
    uint32_t index = 0;
    if (frames > 1)
        index = frameAt(stream.ends.data, frames, stream.playCount, decoded, elapsed, &next);
    const bool frameWhole = decoded || index + 1 < frames;
    if (stream.changed && !frameWhole) {
        ++stream.revision;
        stream.changed = false;
    }
    // A complete frame keeps one serial whatever the revision that showed it, so the cache keeps its picture. The whole
    // image before the bytes end has a serial of its own, so that the image of every byte replaces it.
    uint32_t low = frameWhole ? 0xffffffffu : stream.revision;
    if (decoded && !whole && index == 0)
        low = 0xfffffffeu;
    out->serial = uint64_t(index) << 32 | low;
    out->next = next;
    out->notifies = !stream.complete || !stream.ended;
    out->complete = frameWhole || stream.complete;
    out->status = stream.status;
    if (whole && index != 0) {
        ImageTexture picture = image;
        picture.frame = index;
        picture.image.frameCount = frames;
        picture.frames = stream.frames;
        out->picture = sourceOf(picture);
    } else if (whole && frames == 1 && wholeOf(image) != nullptr) {
        // A still image of every byte is the image as a whole file opens it, so that it has the same previews. The
        // source's own list of previews has only those that arrived before the image's data.
        prTextureSourceRetain(stream.whole);
        out->picture = stream.whole;
    } else if (!whole && frames > 1 && frameWhole && !decoded) {
        // A whole frame of an animation whose bytes are arriving reads with the stream's decoders of its frames, which
        // continue from the frame that each read last, so playing the frames in order decodes each once.
        if (stream.arriving == nullptr) {
            ImageFrames* arriving = allocZeroed<ImageFrames>();
            arriving->references.storeRelaxed(1);
            prMutexInit(&arriving->mutex);
            stream.arriving = arriving;
        }
        ImageTexture picture = image;
        picture.frame = index;
        picture.arrivingFrame = true;
        out->picture = sourceOf(picture);
    } else if (!whole) {
        // The image of every byte is the source itself. A revision is the snapshot of its serial. A complete image's
        // snapshot has the serial of the image of every byte, which replaces it when the bytes end.
        if (stream.picture == nullptr || stream.pictureSerial != out->serial) {
            prTextureSourceRelease(stream.picture);
            stream.picture = snapshotOf(image, index);
            stream.pictureSerial = out->serial;
        }
        prTextureSourceRetain(stream.picture);
        out->picture = stream.picture;
    }
    stream.notified = false;
    prMutexUnlock(&stream.mutex);
}

// Says what the source shows at `time` (PrTextureSourceShowFn): an animation's frame, or the picture of an image whose
// bytes are arriving.
void sourceShow(void* PR_NULLABLE context, double time, PrTextureShow* out) {
    const ImageTexture& image = *static_cast<const ImageTexture*>(context);
    if (image.stream != nullptr) {
        streamShow(image, time, out);
        return;
    }
    animationShow(image, time, out);
}

// Returns a texture source over a copy of `image`, with `previews`. The copy retains the image's bytes.
const PrTextureSource* sourceWithPreviews(const ImageTexture& image,
        const PrTextureSource* PR_NONNULL const* PR_NULLABLE previews, uint32_t previewCount) {
    ImageTexture* held = allocZeroed<ImageTexture>();
    *held = image;
    prBlobSourceRetain(image.bytes);
    if (image.frames != nullptr)
        image.frames->references.fetchAdd(1);
    if (image.stream != nullptr)
        image.stream->references.fetchAdd(1);
    // The source of a whole animated image changes over time, and the source of an image whose bytes were arriving
    // changes as they arrive. A frame, a revision and a preview are still.
    bool whole = image.frame == 0 && image.preview == 0 && !image.arrivingFrame;
    bool changes = whole && (image.frames != nullptr || image.stream != nullptr);
    PrTextureSourceDesc desc{
        .texture = image.desc,
        .read = sourceRead,
        .reformat = sourceReformat,
        .answer = sourceAnswer,
        .previews = previews,
        .previewCount = previewCount,
        .show = changes ? sourceShow : nullptr,
        .notifier = changes && image.stream != nullptr ? image.stream->notifier : nullptr,
        .release = sourceRelease,
        .context = held,
    };
    return prTextureSourceCreate(&desc);
}

// Returns a source of `image` with the previews that its codec reads. Only a 2D image of one level has previews. An
// image with levels of its own is a texture that a consumer samples by level.
//
// An image whose bytes are arriving has the previews that its file embeds before its data, which have arrived by the
// time that the image opens, and each reads from the bytes that have arrived, without waiting for the rest. It has no
// preview that the image's data reduces to, as a JPEG's decodes at a quarter and an eighth are: a preview is still, and
// such a decode would change as the data arrives. The revisions of the image show it meanwhile.
const PrTextureSource* sourceWithDecoderPreviews(const ImageTexture& image, const PrImageDecoder* decoder) {
    const PrTextureSource* previews[kPrTextureSourceMaxPreviews];
    uint32_t count = 0;
    if (image.desc.kind == PrGpuTextureKind_2d && image.desc.layerCount == 1 && image.desc.levelCount == 1) {
        // The codec lists its previews largest first. A source takes them smallest first.
        for (uint32_t i = image.image.previewCount; i-- > 0 && count < kPrTextureSourceMaxPreviews;) {
            if (image.stream != nullptr && !prImageDecoderPreviewEmbedded(decoder, i))
                continue;
            ImageTexture preview = image;
            preview.frames = nullptr;
            preview.preview = i + 1;
            preview.desc.extent = prImageDecoderPreviewExtent(decoder, i);
            previews[count++] = sourceOf(preview);
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

// Creates a source over `bytes`, with its previews. `blob` describes the bytes, and holds only the first of them if
// `partial` is set. `color` says how the source treats the image's colors.
PrStatus sourceCreate(const PrBlobSource* bytes, const PrBlob* blob, const PrImageCodecSet* codec,
        const PrTextureLimits& limits, PrImageTextureColor color, bool partial,
        const PrTextureSource* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    PrImageDecoder* decoder = nullptr;
    PrStatus status = imageOpen(codec, prBlobBytes(blob), limits, partial, &decoder);
    if (status != PrStatus_Ok)
        return status;
    const PrImageInfo& info = *prImageDecoderInfo(decoder);
    // An image whose bytes are arriving opens once they give its extent, as a 2D image of one level. Any other waits
    // for every byte.
    bool shown = info.extent.x != 0 && info.extent.y != 0 && info.faceCount == 1 && info.levelCount <= 1;
    if (partial && !shown) {
        prImageDecoderDestroy(decoder);
        return PrStatus_Truncated;
    }
    const PrImageFormat& format = info.format;
    bool gray = format.model == PrColorModel_Gray;
    bool hasAlpha = format.alpha != PrImageAlpha_None;
    ImageTexture image{
        .bytes = bytes,
        .codec = codec,
        .limits = limits,
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
    if (partial) {
        // The stream's decoder takes the bytes that have arrived now, and then each piece as it arrives. Its input is
        // bounded by the pixels of the texture, as a whole file's is by its size.
        ImageStream* stream = allocZeroed<ImageStream>();
        stream->references.storeRelaxed(1);
        prMutexInit(&stream->mutex);
        prBlobSourceRetain(bytes);
        stream->bytes = bytes;
        stream->notifier = prNotifierCreate();
        stream->color = color;
        stream->decoder = prImageDecoderCreate(codec);
        const uint64_t pixels = uint64_t(limits.extent) * limits.extent;
        stream->bounds = imageLimits(limits, pixels * 16);
        prImageDecoderOpenStream(stream->decoder, &stream->bounds);
        stream->observer = prNotifierObserve(prBlobSourceNotifier(bytes), streamChanged, stream);
        streamChanged(stream);
        image.stream = stream;
    } else if (image.desc.kind == PrGpuTextureKind_2d && info.faceCount == 1 && info.frameCount > 1) {
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
    streamRelease(image.stream);
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
        const PrTextureLimits* limits, bool partial, const PrTextureSource* PR_NULLABLE* PR_NONNULL out) {
    const ImageTextureFormat* image = static_cast<const ImageTextureFormat*>(format);
    return sourceCreate(bytes, blob, image->codec, *limits, image->color, partial, out);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrStatus prImageTextureSourceOpenBytes(PrByteView bytes, const PrImageCodecSet* codec,
        const PrTextureLimits* PR_NULLABLE limits,
        PrImageTextureColor color, const PrTextureSource* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    PrBlob* blob = prBlobCopy(bytes);
    PrBlobSource* source = prBlobSourceCreateBlob(blob);
    PrTextureLimits bounds = limits != nullptr ? *limits : prTextureLimitsDefault();
    PrStatus status = sourceCreate(source, blob, codec, bounds, color, false, out);
    prBlobRelease(blob);
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
