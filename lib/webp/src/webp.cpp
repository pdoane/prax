// WebP images.

#include "pr/webp.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/mem.h"
#include "pr/memory.h"
#include "pr/raw_array.h"

#include "webp/src/vp8.h"
#include "webp/src/vp8l.h"

namespace pr_impl {

// One frame of a WebP file, as its chunks give it. A still image has one, which covers the canvas.
struct WebpFrame {
    PrByteView bitstream;   // the payload of the frame's 'VP8 ' or 'VP8L' chunk
    PrByteView alpha;       // the payload of a lossy frame's 'ALPH' chunk, or empty
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t durationMs;
    bool lossless;
    bool hasAlpha;          // the frame's bitstream or alpha chunk says that it has alpha
    bool blend;             // the frame is blended over the canvas, rather than replacing its rectangle
    bool dispose;           // after it shows, the frame's rectangle is cleared
};

// A WebP file's canvas, frames and metadata. The views are of the file's bytes.
struct WebpImage {
    RawArray<WebpFrame> frames;
    PrByteView profile;     // the ICC profile, or empty
    PrByteView exif;        // the Exif metadata, a TIFF structure, or empty
    uint32_t width;
    uint32_t height;
    uint32_t playCount;     // the plays of an animation, or zero for forever
    bool animated;
    bool alpha;             // the canvas has alpha
};

struct WebpAnimation {
    const WebpImage* image;
    uint8_t* canvas;        // the canvas after frame `next` - 1
    uint8_t* disposed;      // that canvas after the frame's disposal, which the next frame starts from
    uint8_t* frame;         // one frame's pixels
    uint64_t budget;
    uint32_t next;          // the frame that composes next, or zero to start over
    bool keyFrame;          // the frame before `next` started from a transparent canvas
};

struct WebpDecoder {
    WebpImage image;
    PrWebpInfo info;
    WebpAnimation* PR_NULLABLE animation;
    uint32_t* PR_NULLABLE words;    // the words that a still lossless frame decodes through
    uint64_t budget;                // the bytes that decoding a frame holds beyond its pixels
    uint32_t channels;
};

namespace {

PrStatus webpImageOpen(PrByteView bytes, const PrWebpLimits& limits, WebpImage* out);
void webpImageClose(WebpImage& image);
PrStatus webpFrameWorkingBytes(const WebpFrame& frame, uint64_t limit, uint64_t* out);
PrStatus webpFrameDecode(const WebpFrame& frame, WebpBudget& budget, uint32_t* scratch, PrPixels output, uint32_t channels);

constexpr size_t kChunkHeaderBytes = 8;
constexpr size_t kExtendedHeaderBytes = 10;
constexpr size_t kAnimationBytes = 6;
constexpr size_t kFrameHeaderBytes = 16;

// The flags of the extended header (rfc 9649 2.7).
constexpr uint8_t kFlagAnimation = 0x02;
constexpr uint8_t kFlagAlpha = 0x10;

// The compression and filtering of an alpha chunk (rfc 9649 2.7.1.2).
enum {
    WebpAlphaFilter_None,
    WebpAlphaFilter_Horizontal,
    WebpAlphaFilter_Vertical,
    WebpAlphaFilter_Gradient,
};
typedef uint8_t WebpAlphaFilter;

bool isFourCc(const uint8_t* bytes, const char* fourCc) {
    return prMemEquals(bytes, fourCc, 4);
}

uint32_t readLe24(const uint8_t* bytes) {
    return uint32_t(bytes[0]) | (uint32_t(bytes[1]) << 8) | (uint32_t(bytes[2]) << 16);
}

// A chunk of a chunk sequence (rfc 9649 2.3): its FourCC and its payload. Its padding byte may be missing at the end of
// the sequence.
struct Chunk {
    const uint8_t* fourCc;
    PrByteView payload;
};

// The outcome of reading a chunk.
enum {
    WebpChunk_Read,
    WebpChunk_End,          // the sequence has no more chunks
    WebpChunk_Incomplete,   // the sequence ends inside a chunk
};
typedef uint8_t WebpChunk;

WebpChunk nextChunk(const uint8_t* data, size_t end, size_t* at, Chunk* out) {
    if (*at >= end)
        return WebpChunk_End;
    if (end - *at < kChunkHeaderBytes)
        return WebpChunk_Incomplete;
    const uint32_t size = prMemReadLeU32(data + *at + 4);
    if (size > end - *at - kChunkHeaderBytes)
        return WebpChunk_Incomplete;
    out->fourCc = data + *at;
    out->payload = PrByteView{data + *at + kChunkHeaderBytes, size};
    const size_t next = *at + kChunkHeaderBytes + size + (size & 1);
    *at = next < end ? next : end;
    return WebpChunk_Read;
}

// Reads the size and alpha of a frame's bitstream, and checks it against the size that the container gives it.
PrStatus readBitstream(const Chunk& chunk, WebpFrame* frame) {
    frame->bitstream = chunk.payload;
    frame->lossless = isFourCc(chunk.fourCc, "VP8L");
    uint32_t width = 0;
    uint32_t height = 0;
    if (frame->lossless) {
        Vp8lHeader header = {};
        if (!vp8lReadHeader(chunk.payload, &header))
            return PrStatus_InvalidData;
        width = header.width;
        height = header.height;
        frame->hasAlpha = header.alpha;
        // A lossless frame's alpha is its own, and an alpha chunk before it is not used.
        frame->alpha = {};
    } else {
        Vp8Header header = {};
        if (!vp8ReadHeader(chunk.payload, &header))
            return PrStatus_InvalidData;
        width = header.width;
        height = header.height;
        frame->hasAlpha = frame->alpha.size != 0;
    }
    if (frame->width == 0) {
        frame->width = width;
        frame->height = height;
    }
    return width == frame->width && height == frame->height ? PrStatus_Ok : PrStatus_InvalidData;
}

bool isBitstream(const Chunk& chunk) {
    return isFourCc(chunk.fourCc, "VP8 ") || isFourCc(chunk.fourCc, "VP8L");
}

// Reads an animation frame's chunk (rfc 9649 2.7.1.1): its rectangle, timing and disposal, then its alpha and its
// bitstream.
PrStatus readAnimationFrame(PrByteView payload, const WebpImage& image, WebpFrame* frame) {
    if (payload.size < kFrameHeaderBytes)
        return PrStatus_InvalidData;
    const uint8_t* p = payload.data;
    *frame = {};
    frame->x = 2 * readLe24(p);
    frame->y = 2 * readLe24(p + 3);
    frame->width = readLe24(p + 6) + 1;
    frame->height = readLe24(p + 9) + 1;
    frame->durationMs = readLe24(p + 12);
    frame->dispose = (p[15] & 1) != 0;
    frame->blend = (p[15] & 2) == 0;
    if (uint64_t(frame->x) + frame->width > image.width || uint64_t(frame->y) + frame->height > image.height)
        return PrStatus_InvalidData;
    size_t at = kFrameHeaderBytes;
    Chunk chunk = {};
    for (;;) {
        const WebpChunk result = nextChunk(p, payload.size, &at, &chunk);
        if (result != WebpChunk_Read)
            return PrStatus_InvalidData;
        if (isFourCc(chunk.fourCc, "ALPH")) {
            if (frame->alpha.size == 0)
                frame->alpha = chunk.payload;
        } else if (isBitstream(chunk)) {
            return readBitstream(chunk, frame);
        }
    }
}

PrStatus readExtended(const uint8_t* data, size_t end, size_t at, const Chunk& header, const PrWebpLimits& limits,
        bool truncated, WebpImage* out) {
    if (header.payload.size < kExtendedHeaderBytes)
        return PrStatus_InvalidData;
    const uint8_t flags = header.payload.data[0];
    out->width = readLe24(header.payload.data + 4) + 1;
    out->height = readLe24(header.payload.data + 7) + 1;
    if (uint64_t(out->width) * out->height > 0xFFFFFFFFu)
        return PrStatus_InvalidData;
    if (uint64_t(out->width) * out->height > limits.pixels)
        return PrStatus_Limit;
    out->animated = (flags & kFlagAnimation) != 0;
    out->alpha = (flags & kFlagAlpha) != 0 || out->animated;
    out->playCount = 1;
    bool sawAnimation = false;
    uint64_t metadata = 0;
    PrByteView alpha = {};
    Chunk chunk = {};
    for (;;) {
        const WebpChunk result = nextChunk(data, end, &at, &chunk);
        if (result == WebpChunk_End)
            break;
        if (result == WebpChunk_Incomplete)
            return truncated ? PrStatus_Truncated : PrStatus_InvalidData;
        const bool before = out->frames.count == 0;
        if (isFourCc(chunk.fourCc, "ICCP") || isFourCc(chunk.fourCc, "EXIF") || isFourCc(chunk.fourCc, "XMP ")) {
            metadata += chunk.payload.size;
            if (metadata > limits.metadataBytes)
                return PrStatus_Limit;
        }
        if (isFourCc(chunk.fourCc, "ICCP")) {
            if (before && out->profile.size == 0)
                out->profile = chunk.payload;
        } else if (isFourCc(chunk.fourCc, "EXIF")) {
            if (out->exif.size == 0) {
                out->exif = chunk.payload;
                // Some writers keep the "Exif\0\0" that begins a JPEG's segment of it.
                if (out->exif.size >= 6 && prMemEquals(out->exif.data, "Exif\0\0", 6))
                    out->exif = PrByteView{out->exif.data + 6, out->exif.size - 6};
            }
        } else if (!out->animated) {
            if (!before)
                continue;
            if (isFourCc(chunk.fourCc, "ALPH")) {
                if (alpha.size == 0)
                    alpha = chunk.payload;
            } else if (isBitstream(chunk)) {
                WebpFrame frame = {};
                frame.width = out->width;
                frame.height = out->height;
                frame.alpha = alpha;
                const PrStatus status = readBitstream(chunk, &frame);
                if (status != PrStatus_Ok)
                    return status;
                out->alpha = out->alpha || frame.hasAlpha;
                out->frames.push(frame);
            }
        } else if (isFourCc(chunk.fourCc, "ANIM")) {
            if (chunk.payload.size < kAnimationBytes)
                return PrStatus_InvalidData;
            if (!sawAnimation)
                out->playCount = prMemReadLeU16(chunk.payload.data + 4);
            sawAnimation = true;
        } else if (isFourCc(chunk.fourCc, "ANMF")) {
            if (!sawAnimation)
                return PrStatus_InvalidData;
            if (out->frames.count >= limits.frames)
                return PrStatus_Limit;
            WebpFrame frame = {};
            const PrStatus status = readAnimationFrame(chunk.payload, *out, &frame);
            if (status != PrStatus_Ok)
                return status;
            out->frames.push(frame);
        }
    }
    if (out->frames.count == 0)
        return truncated ? PrStatus_Truncated : PrStatus_InvalidData;
    return truncated ? PrStatus_Truncated : PrStatus_Ok;
}

// Decodes an alpha chunk's data (rfc 9649 2.7.1.2) into one byte a pixel. Its preprocessing bits tell how the encoder
// reduced the levels, which a decoder may use to smooth them. The reference decoder does not by default, and this one
// does not.
PrStatus decodeAlpha(PrByteView chunk, uint32_t width, uint32_t height, WebpBudget& budget, uint8_t* alpha) {
    if (chunk.size < 1)
        return PrStatus_InvalidData;
    const uint32_t method = chunk.data[0] & 3;
    const uint32_t filter = (chunk.data[0] >> 2) & 3;
    const size_t count = size_t(width) * height;
    const PrByteView data = {chunk.data + 1, chunk.size - 1};
    if (method == 0) {
        if (data.size < count)
            return PrStatus_InvalidData;
        prMemCopy(alpha, data.data, count);
    } else if (method == 1) {
        if (!webpBudgetTake(budget, uint64_t(count) * 4))
            return PrStatus_Limit;
        uint32_t* argb = static_cast<uint32_t*>(prMemoryAlloc(count * 4));
        const PrStatus status = vp8lDecode(data, width, height, budget, argb);
        if (status == PrStatus_Ok) {
            for (size_t i = 0; i < count; ++i)
                alpha[i] = uint8_t(argb[i] >> 8);
        }
        prMemoryFree(argb);
        webpBudgetGive(budget, uint64_t(count) * 4);
        if (status != PrStatus_Ok)
            return status;
    } else {
        return PrStatus_InvalidData;
    }
    if (filter == WebpAlphaFilter_None)
        return PrStatus_Ok;
    // The first row predicts from the left, and the first pixel of each later row from the pixel above it.
    for (uint32_t x = 1; x < width; ++x)
        alpha[x] = uint8_t(alpha[x] + alpha[x - 1]);
    for (uint32_t y = 1; y < height; ++y) {
        uint8_t* row = alpha + size_t(y) * width;
        const uint8_t* above = row - width;
        if (filter == WebpAlphaFilter_Vertical) {
            for (uint32_t x = 0; x < width; ++x)
                row[x] = uint8_t(row[x] + above[x]);
            continue;
        }
        row[0] = uint8_t(row[0] + above[0]);
        for (uint32_t x = 1; x < width; ++x) {
            int32_t predicted = row[x - 1];
            if (filter == WebpAlphaFilter_Gradient) {
                predicted += int32_t(above[x]) - int32_t(above[x - 1]);
                predicted = predicted < 0 ? 0 : predicted > 255 ? 255 : predicted;
            }
            row[x] = uint8_t(row[x] + predicted);
        }
    }
    return PrStatus_Ok;
}

// Blends `source` over `destination` as the reference decoder blends straight alpha, in eight-bit arithmetic: the
// formula of rfc 9649 2.7.1.1 without linearizing.
void blendPixel(uint8_t* source, const uint8_t* destination) {
    const uint32_t sourceAlpha = source[3];
    if (sourceAlpha == 0) {
        prMemCopy(source, destination, 4);
        return;
    }
    const uint32_t destinationAlpha = (uint32_t(destination[3]) * (256 - sourceAlpha)) >> 8;
    const uint32_t alpha = sourceAlpha + destinationAlpha;
    const uint32_t scale = (1u << 24) / alpha;
    for (uint32_t c = 0; c < 3; ++c)
        source[c] = uint8_t(((source[c] * sourceAlpha + destination[c] * destinationAlpha) * scale) >> 24);
    source[3] = uint8_t(alpha);
}

bool covers(const WebpFrame& frame, const WebpImage& image) {
    return frame.width == image.width && frame.height == image.height;
}

bool inside(const WebpFrame& frame, uint32_t x, uint32_t y) {
    return x >= frame.x && x - frame.x < frame.width && y >= frame.y && y - frame.y < frame.height;
}

// Composes the frame `index` over the disposed canvas of the frame before it, as the reference decoder composes it. A
// frame that starts from a transparent canvas, a key frame, is drawn without blending, and one that follows a frame
// disposed to transparent blends only outside that frame's rectangle, whose pixels it keeps as decoded.
PrStatus compose(WebpAnimation& animation, uint32_t index) {
    const WebpImage& image = *animation.image;
    const WebpFrame& frame = image.frames.data[index];
    const size_t canvasBytes = size_t(image.width) * image.height * 4;
    const WebpFrame* previous = index == 0 ? nullptr : &image.frames.data[index - 1];
    bool key = index == 0 || ((!frame.hasAlpha || !frame.blend) && covers(frame, image));
    if (!key)
        key = previous->dispose && (covers(*previous, image) || animation.keyFrame);
    WebpBudget budget = {.limit = animation.budget};
    const PrStatus status = webpFrameDecode(frame, budget, reinterpret_cast<uint32_t*>(animation.frame),
        PrPixels{animation.frame, size_t(frame.width) * 4, {frame.width, frame.height}}, 4);
    if (status != PrStatus_Ok)
        return status;
    if (key)
        prMemZero(animation.canvas, canvasBytes);
    else
        prMemCopy(animation.canvas, animation.disposed, canvasBytes);
    const size_t canvasStride = size_t(image.width) * 4;
    const bool blend = index > 0 && frame.blend && !key;
    for (uint32_t row = 0; row < frame.height; ++row) {
        const uint32_t y = frame.y + row;
        uint8_t* to = animation.canvas + y * canvasStride + size_t(frame.x) * 4;
        prMemCopy(to, animation.frame + size_t(row) * frame.width * 4, size_t(frame.width) * 4);
        if (!blend)
            continue;
        const uint8_t* under = animation.disposed + y * canvasStride + size_t(frame.x) * 4;
        for (uint32_t column = 0; column < frame.width; ++column) {
            if (to[column * 4 + 3] == 0xFF || (previous->dispose && inside(*previous, frame.x + column, y)))
                continue;
            blendPixel(to + column * 4, under + column * 4);
        }
    }
    prMemCopy(animation.disposed, animation.canvas, canvasBytes);
    if (frame.dispose) {
        for (uint32_t row = 0; row < frame.height; ++row)
            prMemZero(animation.disposed + (frame.y + row) * canvasStride + size_t(frame.x) * 4, size_t(frame.width) * 4);
    }
    animation.keyFrame = key;
    return PrStatus_Ok;
}

bool multiplyChecked(uint64_t a, uint64_t b, uint64_t* out) {
    if (a != 0 && b > ~uint64_t(0) / a)
        return false;
    *out = a * b;
    return true;
}

PrStatus describe(WebpDecoder& decoder, PrByteView bytes, const PrWebpLimits& limits, uint64_t held) {
    const WebpImage& image = decoder.image;
    PrWebpInfo* info = &decoder.info;
    *info = PrWebpInfo{
        .extent = {image.width, image.height},
        .frameCount = image.frames.count,
        .playCount = image.animated ? image.playCount : 1,
        .animated = image.animated,
        .alpha = image.alpha,
        .profile = image.profile,
        .exif = image.exif,
    };
    decoder.channels = image.alpha ? 4 : 3;
    // The output and the canvas fit before the frames' codes are read and counted: an animation keeps two canvases.
    const uint64_t pixels = uint64_t(image.width) * image.height;
    const uint64_t canvases = image.animated ? 8 * pixels : 0;
    if (pixels > limits.pixels || pixels * decoder.channels > limits.outputBytes || held > limits.workingBytes ||
            canvases > limits.workingBytes - held)
        return PrStatus_Limit;
    // Decoding a frame works through its pixels, and composing one works through the canvas twice.
    uint64_t work = bytes.size;
    uint64_t largest = 0;
    for (uint32_t i = 0; i < image.frames.count; ++i) {
        const WebpFrame& frame = image.frames.data[i];
        const uint64_t area = uint64_t(frame.width) * frame.height;
        largest = area > largest ? area : largest;
        work += area + (image.animated ? 2 * pixels : 0);
        if (work > limits.work)
            return PrStatus_Limit;
        uint64_t frameBytes = 0;
        const PrStatus measured = webpFrameWorkingBytes(frame, limits.workingBytes, &frameBytes);
        if (measured != PrStatus_Ok)
            return measured;
        decoder.budget = frameBytes > decoder.budget ? frameBytes : decoder.budget;
    }
    // A frame's pixels, or a still lossless frame's words, beside the decode's own and the frames' descriptions.
    uint64_t frameBytes = 0;
    if (!multiplyChecked(largest, 4, &frameBytes))
        return PrStatus_Limit;
    const uint64_t working = held + canvases + frameBytes + decoder.budget + sizeof(WebpDecoder) +
        uint64_t(image.frames.count) * sizeof(WebpFrame);
    if (working > limits.workingBytes)
        return PrStatus_Limit;
    info->workingBytes = working;
    return PrStatus_Ok;
}

PrStatus webpImageOpen(PrByteView bytes, const PrWebpLimits& limits, WebpImage* out) {
    if (!prWebpHasSignature(bytes))
        return PrStatus_NotSupported;
    // The RIFF size counts the bytes after it, from "WEBP" on, which hold at least one chunk's header.
    const uint64_t riff = prMemReadLeU32(bytes.data + 4);
    if (riff < 4 + kChunkHeaderBytes)
        return PrStatus_InvalidData;
    const bool truncated = riff + 8 > bytes.size;
    const size_t end = truncated ? bytes.size : size_t(riff + 8);
    size_t at = kPrWebpSignatureBytes;
    Chunk first = {};
    const WebpChunk result = nextChunk(bytes.data, end, &at, &first);
    if (result != WebpChunk_Read)
        return truncated ? PrStatus_Truncated : PrStatus_InvalidData;
    if (isFourCc(first.fourCc, "VP8X"))
        return readExtended(bytes.data, end, at, first, limits, truncated, out);
    if (!isBitstream(first))
        return PrStatus_InvalidData;
    WebpFrame frame = {};
    const PrStatus status = readBitstream(first, &frame);
    if (status != PrStatus_Ok)
        return status;
    if (uint64_t(frame.width) * frame.height > limits.pixels)
        return PrStatus_Limit;
    out->width = frame.width;
    out->height = frame.height;
    out->alpha = frame.hasAlpha;
    out->playCount = 1;
    out->frames.push(frame);
    return truncated ? PrStatus_Truncated : PrStatus_Ok;
}

void webpImageClose(WebpImage& image) {
    image.frames.reset();
}

PrStatus webpFrameWorkingBytes(const WebpFrame& frame, uint64_t limit, uint64_t* out) {
    WebpBudget budget = {.limit = limit};
    *out = 0;
    if (frame.lossless) {
        const PrByteView stream = {frame.bitstream.data + kVp8lHeaderBytes, frame.bitstream.size - kVp8lHeaderBytes};
        const PrStatus status = vp8lDecode(stream, frame.width, frame.height, budget, nullptr);
        *out = budget.peak;
        return status;
    }
    const uint64_t pixels = uint64_t(frame.width) * frame.height;
    uint64_t bytes = vp8WorkingBytes(frame.width, frame.height);
    if (frame.alpha.size != 0) {
        bytes += pixels;
        if ((frame.alpha.data[0] & 3) == 1) {
            const PrByteView stream = {frame.alpha.data + 1, frame.alpha.size - 1};
            const PrStatus status = vp8lDecode(stream, frame.width, frame.height, budget, nullptr);
            if (status != PrStatus_Ok)
                return status;
            bytes += pixels * 4 + budget.peak;
        }
    }
    *out = bytes;
    return bytes > limit ? PrStatus_Limit : PrStatus_Ok;
}

PrStatus webpFrameDecode(const WebpFrame& frame, WebpBudget& budget, uint32_t* scratch, PrPixels output,
        uint32_t channels) {
    const size_t count = size_t(frame.width) * frame.height;
    if (frame.lossless) {
        // The pixels decode as words of 0xAARRGGBB, which each row then turns into bytes of red, green, blue and alpha.
        // A word is read before its bytes are written, so the two may share memory.
        const PrByteView stream = {frame.bitstream.data + kVp8lHeaderBytes, frame.bitstream.size - kVp8lHeaderBytes};
        const PrStatus status = vp8lDecode(stream, frame.width, frame.height, budget, scratch);
        if (status != PrStatus_Ok)
            return status;
        for (uint32_t y = 0; y < frame.height; ++y) {
            const uint32_t* from = scratch + size_t(y) * frame.width;
            uint8_t* to = output.data + y * output.stride;
            if (channels == 4) {
                for (uint32_t x = 0; x < frame.width; ++x) {
                    const uint32_t pixel = from[x];
                    to[x * 4] = uint8_t(pixel >> 16);
                    to[x * 4 + 1] = uint8_t(pixel >> 8);
                    to[x * 4 + 2] = uint8_t(pixel);
                    to[x * 4 + 3] = uint8_t(pixel >> 24);
                }
            } else {
                for (uint32_t x = 0; x < frame.width; ++x) {
                    const uint32_t pixel = from[x];
                    to[x * 3] = uint8_t(pixel >> 16);
                    to[x * 3 + 1] = uint8_t(pixel >> 8);
                    to[x * 3 + 2] = uint8_t(pixel);
                }
            }
        }
        return PrStatus_Ok;
    }
    Vp8Planes planes = {};
    PrStatus status = vp8Decode(frame.bitstream, budget, &planes);
    if (status == PrStatus_Ok)
        vp8ToRgb(planes, output.data, output.stride, channels);
    vp8PlanesFree(planes, budget);
    if (status != PrStatus_Ok || frame.alpha.size == 0 || channels != 4)
        return status;
    if (!webpBudgetTake(budget, count))
        return PrStatus_Limit;
    uint8_t* alpha = static_cast<uint8_t*>(prMemoryAlloc(count));
    status = decodeAlpha(frame.alpha, frame.width, frame.height, budget, alpha);
    for (uint32_t y = 0; status == PrStatus_Ok && y < frame.height; ++y) {
        const uint8_t* from = alpha + size_t(y) * frame.width;
        uint8_t* to = output.data + y * output.stride;
        for (uint32_t x = 0; x < frame.width; ++x)
            to[x * 4 + 3] = from[x];
    }
    prMemoryFree(alpha);
    webpBudgetGive(budget, count);
    return status;
}

WebpAnimation* webpAnimationOpen(const WebpImage& image, uint64_t budget) {
    WebpAnimation* animation = allocZeroed<WebpAnimation>();
    animation->image = &image;
    animation->budget = budget;
    const size_t canvasBytes = size_t(image.width) * image.height * 4;
    animation->canvas = static_cast<uint8_t*>(prMemoryAlloc(canvasBytes));
    animation->disposed = static_cast<uint8_t*>(prMemoryAlloc(canvasBytes));
    size_t largest = 0;
    for (uint32_t i = 0; i < image.frames.count; ++i) {
        const size_t area = size_t(image.frames.data[i].width) * image.frames.data[i].height;
        largest = area > largest ? area : largest;
    }
    animation->frame = static_cast<uint8_t*>(prMemoryAlloc(largest * 4));
    return animation;
}

void webpAnimationClose(WebpAnimation* PR_NULLABLE animation) {
    if (animation == nullptr)
        return;
    prMemoryFree(animation->canvas);
    prMemoryFree(animation->disposed);
    prMemoryFree(animation->frame);
    prMemoryFree(animation);
}

PrStatus webpAnimationFrame(WebpAnimation& animation, uint32_t frame, PrPixels output) {
    if (frame >= animation.image->frames.count)
        return PrStatus_InvalidData;
    if (frame < animation.next)
        animation.next = 0;
    while (animation.next <= frame) {
        const PrStatus status = compose(animation, animation.next);
        if (status != PrStatus_Ok) {
            animation.next = 0;
            return status;
        }
        animation.next += 1;
    }
    const size_t row = size_t(animation.image->width) * 4;
    for (uint32_t y = 0; y < animation.image->height; ++y)
        prMemCopy(output.data + y * output.stride, animation.canvas + y * row, row);
    return PrStatus_Ok;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrWebpDecoder : WebpDecoder {};

bool prWebpHasSignature(PrByteView bytes) {
    return bytes.size >= kPrWebpSignatureBytes && isFourCc(bytes.data, "RIFF") && isFourCc(bytes.data + 8, "WEBP");
}

PrWebpLimits prWebpLimitsDefault(void) {
    return PrWebpLimits{
        .inputBytes = 256u * 1024u * 1024u,
        .pixels = 64u * 1024u * 1024u,
        .workingBytes = 512u * 1024u * 1024u,
        .outputBytes = 512u * 1024u * 1024u,
        .work = 1000000000u,
        .frames = 4096,
        .metadataBytes = 16u * 1024u * 1024u,
    };
}

PrStatus prWebpDecoderCreate(PrByteView bytes, const PrWebpLimits* PR_NULLABLE limits, uint64_t held,
        PrWebpDecoder* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    const PrWebpLimits bounds = limits != nullptr ? *limits : prWebpLimitsDefault();
    if (bounds.work == 0 || bytes.size > bounds.inputBytes)
        return PrStatus_Limit;
    PrWebpDecoder* decoder = allocZeroed<PrWebpDecoder>();
    const PrStatus status = webpImageOpen(bytes, bounds, &decoder->image);
    PrStatus described = status;
    if (status == PrStatus_Ok || status == PrStatus_Truncated)
        described = describe(*decoder, bytes, bounds, held);
    if (described != PrStatus_Ok) {
        prWebpDecoderDestroy(decoder);
        return described;
    }
    const WebpImage& image = decoder->image;
    if (image.frames.count == 0) {
        // A file that ends before its first frame is whole describes no frame to read.
    } else if (image.animated) {
        decoder->animation = webpAnimationOpen(image, decoder->budget);
    } else if (image.frames.data[0].lossless) {
        decoder->words = static_cast<uint32_t*>(prMemoryAlloc(size_t(image.width) * image.height * 4));
    }
    *out = decoder;
    return status;
}

void prWebpDecoderDestroy(PrWebpDecoder* PR_NULLABLE decoder) {
    if (decoder == nullptr)
        return;
    webpAnimationClose(decoder->animation);
    webpImageClose(decoder->image);
    prMemoryFree(decoder->words);
    prMemoryFree(decoder);
}

const PrWebpInfo* prWebpDecoderInfo(const PrWebpDecoder* decoder) {
    return &decoder->info;
}

uint32_t prWebpDecoderFrameDuration(const PrWebpDecoder* decoder, uint32_t frame) {
    PR_ASSERT(frame < decoder->image.frames.count);
    return decoder->image.animated ? decoder->image.frames.data[frame].durationMs : 0;
}

PrStatus prWebpDecoderRead(PrWebpDecoder* decoder, uint32_t frame, PrPixels output) {
    PR_ASSERT(frame < decoder->image.frames.count);
    PR_ASSERT(output.extent.x == decoder->image.width && output.extent.y == decoder->image.height);
    if (decoder->animation != nullptr)
        return webpAnimationFrame(*decoder->animation, frame, output);
    WebpBudget budget = {.limit = decoder->budget};
    return webpFrameDecode(decoder->image.frames.data[0], budget, decoder->words, output, decoder->channels);
}

PrStatus prWebpDecodePlanes(PrByteView file, const PrWebpLimits* PR_NULLABLE limits, const PrWebpPlanes* planes) {
    const PrByteView bytes = file;
    const PrWebpLimits bounds = limits != nullptr ? *limits : prWebpLimitsDefault();
    const PrPixels luma = planes->luma;
    const PrPixels blueDifference = planes->blueDifference;
    const PrPixels redDifference = planes->redDifference;
    if (bytes.size > bounds.inputBytes)
        return PrStatus_Limit;
    WebpImage image = {};
    PrStatus status = webpImageOpen(bytes, bounds, &image);
    if (status == PrStatus_Ok && (image.animated || image.frames.data[0].lossless))
        status = PrStatus_NotSupported;
    const WebpFrame* frame = status == PrStatus_Ok ? &image.frames.data[0] : nullptr;
    const PrUVec2 chroma = frame == nullptr ? PrUVec2{} : PrUVec2{(frame->width + 1) / 2, (frame->height + 1) / 2};
    if (frame != nullptr && (luma.extent.x != frame->width || luma.extent.y != frame->height ||
            blueDifference.extent.x != chroma.x || blueDifference.extent.y != chroma.y ||
            redDifference.extent.x != chroma.x || redDifference.extent.y != chroma.y))
        status = PrStatus_InvalidData;
    WebpBudget budget = {.limit = bounds.workingBytes};
    Vp8Planes decoded = {};
    if (status == PrStatus_Ok)
        status = vp8Decode(frame->bitstream, budget, &decoded);
    if (status == PrStatus_Ok) {
        for (uint32_t y = 0; y < frame->height; ++y)
            prMemCopy(luma.data + y * luma.stride, decoded.y + size_t(y) * decoded.yStride, frame->width);
        for (uint32_t y = 0; y < chroma.y; ++y) {
            prMemCopy(blueDifference.data + y * blueDifference.stride, decoded.u + size_t(y) * decoded.uvStride, chroma.x);
            prMemCopy(redDifference.data + y * redDifference.stride, decoded.v + size_t(y) * decoded.uvStride, chroma.x);
        }
    }
    vp8PlanesFree(decoded, budget);
    webpImageClose(image);
    return status;
}
