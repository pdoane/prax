// GIF decoding.

#include "pr/gif.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/mem.h"
#include "pr/raw_array.h"

namespace pr_impl {
namespace {

// The number of codes that twelve bits address, at which an LZW dictionary stops growing, and those twelve bits, the
// longest code (gif89a appendix F).
constexpr uint32_t kMaxCodes = 4096;
constexpr uint32_t kCodeMaxBits = 12;

// No code has been decoded since the last clear.
constexpr uint32_t kNoCode = 0xFFFFFFFFu;

// The interlace passes: the row at which each starts, and the number of rows that it steps by (gif89a 20).
constexpr uint32_t kPassCount = 4;
constexpr uint32_t kPassOrigin[kPassCount] = {0, 4, 2, 1};
constexpr uint32_t kPassStep[kPassCount] = {8, 8, 4, 2};

// What the graphic control extension of a frame requests of the screen after the frame is displayed (gif89a 23).
enum {
    GifDisposal_Unspecified,
    GifDisposal_Keep,
    GifDisposal_Background,
    GifDisposal_Previous,
};
typedef uint8_t GifDisposal;

// The logical screen that frames compose onto, and its global colour table.
struct Screen {
    const uint8_t* palette;
    uint32_t paletteCount;
    uint32_t width;
    uint32_t height;
    uint32_t loops; // number of plays; zero means forever
    size_t blocks;  // offset of the first block after the header
};

// The image descriptor of one frame, with the graphic control extension before it. `data` is the offset of the minimum
// code size byte that starts the LZW sub-blocks. `palette` is the local table, or the global one if the frame has none.
struct Frame {
    const uint8_t* palette;
    size_t data;
    uint32_t paletteCount;
    uint32_t left;
    uint32_t top;
    uint32_t width;
    uint32_t height;
    uint32_t delayMs;
    GifDisposal disposal;
    uint8_t transparentIndex;
    bool transparent;
    bool interlaced;
    bool truncated;     // the bytes end inside the frame's data
};

// The LZW code stream, read low bit first out of a chain of sub-blocks.
struct Bits {
    const uint8_t* bytes;
    size_t size;
    size_t at;          // next byte of the file
    uint32_t remaining; // bytes left in the sub-block being read
    uint32_t bits;
    uint32_t count;
};

bool isVersion(const uint8_t* bytes) {
    return (bytes[3] == '8' && (bytes[4] == '7' || bytes[4] == '9') && bytes[5] == 'a');
}

uint32_t delayOf(uint32_t hundredths) {
    return hundredths * 10;
}

// Walks the sub-block chain starting at the length byte at `at`, and leaves `at` just past the terminator. Returns
// false if the chain runs off the end.
bool skipSubBlocks(const uint8_t* bytes, size_t size, size_t& at) {
    while (at < size) {
        const uint32_t length = bytes[at++];
        if (length == 0) {
            return true;
        }
        if (size_t(length) > size - at) {
            return false;
        }
        at += length;
    }
    return false;
}

// Reads the header and the logical screen descriptor, leaving `blocks` at the first block after the global colour
// table.
bool readScreen(const uint8_t* bytes, size_t size, Screen& screen) {
    if (!prGifHasSignature({bytes, size}) || size < 13) {
        return false;
    }
    screen = {};
    screen.loops = 1;
    screen.width = prMemReadLeU16(bytes + 6);
    screen.height = prMemReadLeU16(bytes + 8);
    const uint8_t packed = bytes[10];
    size_t at = 13;
    if ((packed & 0x80) != 0) {
        screen.paletteCount = 2u << (packed & 7);
        if (size_t(screen.paletteCount) * 3 > size - at) {
            return false;
        }
        screen.palette = bytes + at;
        at += size_t(screen.paletteCount) * 3;
    }
    screen.blocks = at;
    return screen.width != 0 && screen.height != 0;
}

// Collects frames from the block chain until reaching `limit` or the trailer. Returns false for a malformed block,
// keeping the frames read before it.
bool readFrames(const uint8_t* bytes, size_t size, Screen& screen, uint32_t limit, RawArray<Frame>& frames) {
    size_t at = screen.blocks;
    Frame control = {};
    control.delayMs = delayOf(0);
    while (at < size && frames.count < limit) {
        const uint8_t block = bytes[at++];
        if (block == 0x3B) {
            return true;
        }
        if (block == 0x21) {
            if (at >= size) {
                return false;
            }
            const uint8_t label = bytes[at++];
            // The Netscape application extension stores the loop count in its own sub-block, after the eleven-byte
            // identifier.
            if (label == 0xFF && size - at >= 19 && bytes[at] == 11 &&
                    prMemEquals(bytes + at + 1, "NETSCAPE2.0", 11) && bytes[at + 12] == 3 && bytes[at + 13] == 1) {
                const uint32_t repeats = prMemReadLeU16(bytes + at + 14);
                screen.loops = repeats == 0 ? 0 : repeats + 1;
            }
            if (label == 0xF9 && size - at >= 6 && bytes[at] == 4) {
                const uint8_t packed = bytes[at + 1];
                control.disposal = (packed >> 2) & 7;
                control.transparent = (packed & 1) != 0;
                control.delayMs = delayOf(prMemReadLeU16(bytes + at + 2));
                control.transparentIndex = bytes[at + 4];
            }
            if (!skipSubBlocks(bytes, size, at)) {
                return false;
            }
            continue;
        }
        if (block != 0x2C || size - at < 9) {
            return false;
        }
        Frame frame = control;
        frame.left = prMemReadLeU16(bytes + at);
        frame.top = prMemReadLeU16(bytes + at + 2);
        frame.width = prMemReadLeU16(bytes + at + 4);
        frame.height = prMemReadLeU16(bytes + at + 6);
        const uint8_t packed = bytes[at + 8];
        frame.interlaced = (packed & 0x40) != 0;
        at += 9;
        if ((packed & 0x80) != 0) {
            frame.paletteCount = 2u << (packed & 7);
            if (size_t(frame.paletteCount) * 3 > size - at) {
                return false;
            }
            frame.palette = bytes + at;
            at += size_t(frame.paletteCount) * 3;
        } else {
            frame.palette = screen.palette;
            frame.paletteCount = screen.paletteCount;
        }
        if (at >= size) {
            return false;
        }
        frame.data = at;
        at += 1;
        if (!skipSubBlocks(bytes, size, at)) {
            // A frame whose data the bytes cut short is decoded as far as the data goes.
            frame.truncated = true;
            frames.push(frame);
            return false;
        }
        frames.push(frame);
        control = {};
        control.delayMs = delayOf(0);
    }
    return true;
}

// Reads the next byte of the code stream, moving from one sub-block to the next. Returns false at the terminator, or if
// the chain runs off the end.
bool nextByte(Bits& in, uint8_t& out) {
    while (in.remaining == 0) {
        if (in.at >= in.size) {
            return false;
        }
        in.remaining = in.bytes[in.at++];
        if (in.remaining == 0 || size_t(in.remaining) > in.size - in.at) {
            return false;
        }
    }
    out = in.bytes[in.at++];
    in.remaining -= 1;
    return true;
}

// Reads the next `width` bits of the code stream. Returns false if the data ends first.
bool readCode(Bits& in, uint32_t width, uint32_t& out) {
    while (in.count < width) {
        uint8_t byte = 0;
        if (!nextByte(in, byte)) {
            return false;
        }
        in.bits |= uint32_t(byte) << in.count;
        in.count += 8;
    }
    out = in.bits & ((1u << width) - 1);
    in.bits >>= width;
    in.count -= width;
    return true;
}

// The dictionary that one LZW expansion builds, and the stack on which it unwinds strings. The tables are allocated
// once and reused by every frame of a decode.
struct LzwTables {
    uint16_t* prefix;
    uint8_t* suffix;
    uint8_t* stack;
};

LzwTables lzwTables() {
    return {
        static_cast<uint16_t*>(prMemoryAlloc(kMaxCodes * sizeof(uint16_t))),
        static_cast<uint8_t*>(prMemoryAlloc(kMaxCodes)),
        static_cast<uint8_t*>(prMemoryAlloc(kMaxCodes)),
    };
}

// Expands the LZW sub-blocks of the frame into one palette index per pixel of its rect, writing at `indices`. Reports
// how many of the `count` pixels the coded data reached. A stream that ends before the whole rect leaves the rest
// undrawn instead of failing. Returns false for an invalid minimum code size, or a code that the dictionary cannot
// hold.
bool decodeLzw(const uint8_t* bytes, size_t size, const Frame& frame, const LzwTables& tables,
        uint8_t* indices, size_t count, size_t* outWritten) {
    *outWritten = 0;
    const uint32_t minCodeSize = bytes[frame.data];
    if (minCodeSize < 1 || minCodeSize + 1 > kCodeMaxBits) {
        return false;
    }
    Bits in = {};
    in.bytes = bytes;
    in.size = size;
    in.at = frame.data + 1;

    uint16_t* prefix = tables.prefix;
    uint8_t* suffix = tables.suffix;
    uint8_t* stack = tables.stack;
    const uint32_t clear = 1u << minCodeSize;
    const uint32_t end = clear + 1;
    uint32_t next = clear + 2;
    uint32_t width = minCodeSize + 1;
    uint32_t previous = kNoCode;
    uint8_t previousFirst = 0;
    size_t written = 0;
    uint32_t code = 0;
    while (readCode(in, width, code)) {
        if (code == clear) {
            next = clear + 2;
            width = minCodeSize + 1;
            previous = kNoCode;
            continue;
        }
        if (code == end) {
            break;
        }
        if (code > next || (code == next && previous == kNoCode)) {
            return false;
        }
        uint32_t top = 0;
        uint32_t walk = code;
        if (code == next) {
            // The dictionary entry that this code names is the one that this step adds. Its string is therefore the
            // previous string followed by its own first byte (gif89a appendix F).
            stack[top++] = previousFirst;
            walk = previous;
        }
        while (walk >= clear + 2) {
            if (top >= kMaxCodes) {
                return false;
            }
            stack[top++] = suffix[walk];
            walk = prefix[walk];
        }
        if (walk >= clear) {
            return false;
        }
        stack[top++] = uint8_t(walk);
        const uint8_t first = uint8_t(walk);
        while (top > 0) {
            top -= 1;
            if (written < count) {
                indices[written++] = stack[top];
            }
        }
        if (written == count) {
            *outWritten = written;
            return true;
        }
        if (previous != kNoCode && next < kMaxCodes) {
            prefix[next] = uint16_t(previous);
            suffix[next] = first;
            next += 1;
            if (next == (1u << width) && width < kCodeMaxBits) {
                width += 1;
            }
        }
        previous = code;
        previousFirst = first;
    }
    *outWritten = written;
    return true;
}

// Draws the pixels of one frame onto the screen. Pixels with the transparent index of the frame, and pixels beyond the
// coded data, are left untouched.
void drawFrame(const Frame& frame, const uint8_t* indices, size_t written, const Screen& screen, uint8_t* pixels) {
    uint32_t pass = 0;
    uint32_t interlacedRow = kPassOrigin[0];
    for (uint32_t row = 0; row < frame.height; ++row) {
        const uint32_t target = frame.interlaced ? interlacedRow : row;
        if (frame.interlaced) {
            interlacedRow += kPassStep[pass];
            while (interlacedRow >= frame.height && pass + 1 < kPassCount) {
                pass += 1;
                interlacedRow = kPassOrigin[pass];
            }
        }
        const uint32_t screenY = frame.top + target;
        const bool visible = screenY < screen.height;
        for (uint32_t column = 0; column < frame.width; ++column) {
            const size_t index = size_t(row) * frame.width + column;
            if (index >= written) {
                return;
            }
            const uint32_t screenX = frame.left + column;
            const uint8_t value = indices[index];
            if (!visible || screenX >= screen.width || (frame.transparent && value == frame.transparentIndex) ||
                    value >= frame.paletteCount) {
                continue;
            }
            const uint8_t* entry = frame.palette + size_t(value) * 3;
            uint8_t* out = pixels + (size_t(screenY) * screen.width + screenX) * 4;
            out[0] = entry[0];
            out[1] = entry[1];
            out[2] = entry[2];
            out[3] = 0xFF;
        }
    }
}

// Clears the rect of the frame to fully transparent, which is the background that a presenter restores to.
void clearFrame(const Frame& frame, const Screen& screen, uint8_t* pixels) {
    if (frame.left >= screen.width) {
        return;
    }
    const uint32_t columns = frame.width < screen.width - frame.left ? frame.width : screen.width - frame.left;
    for (uint32_t row = 0; row < frame.height; ++row) {
        const uint32_t screenY = frame.top + row;
        if (screenY >= screen.height) {
            return;
        }
        uint8_t* out = pixels + (size_t(screenY) * screen.width + frame.left) * 4;
        prMemZero(out, size_t(columns) * 4);
    }
}

// Indicates that no frame has been composed onto the screen of the animation.
constexpr uint32_t kNoFrame = 0xFFFFFFFFu;

// Returns the pixel count of the largest frame among those that a compose walks.
size_t widestFrame(const RawArray<Frame>& frames) {
    size_t widest = 0;
    for (uint32_t i = 0; i < frames.count; ++i) {
        const size_t pixels = size_t(frames.data[i].width) * size_t(frames.data[i].height);
        widest = pixels > widest ? pixels : widest;
    }
    return widest;
}

bool addChecked(uint64_t a, uint64_t b, uint64_t* out) {
    if (a > 0xFFFFFFFFFFFFFFFFull - b)
        return false;
    *out = a + b;
    return true;
}

bool multiplyChecked(uint64_t a, uint64_t b, uint64_t* out) {
    if (a != 0 && b > 0xFFFFFFFFFFFFFFFFull / a)
        return false;
    *out = a * b;
    return true;
}

bool skipBlocks(const uint8_t* bytes, size_t size, size_t* at) {
    while (*at < size) {
        const uint32_t count = bytes[(*at)++];
        if (count == 0)
            return true;
        if (count > size - *at)
            return false;
        *at += count;
    }
    return false;
}

// Walks the blocks of `bytes` within `limits`, and fills `info` with the screen's extent, the frames and the plays, and
// its working bytes with `held`, what the caller holds. Returns Truncated for a file that ends before its trailer.
PrStatus inspect(const uint8_t* bytes, size_t size, const PrGifLimits& limits, uint64_t held, PrGifInfo* info) {
    if (limits.work == 0)
        return PrStatus_Limit;
    if (size < 13)
        return PrStatus_InvalidData;
    info->extent.x = prMemReadLeU16(bytes + 6);
    info->extent.y = prMemReadLeU16(bytes + 8);
    if (info->extent.x == 0 || info->extent.y == 0)
        return PrStatus_InvalidData;
    size_t at = 13;
    const uint8_t packed = bytes[10];
    if ((packed & 0x80) != 0) {
        const size_t paletteBytes = size_t(2u << (packed & 7)) * 3;
        if (paletteBytes > size - at)
            return PrStatus_InvalidData;
        at += paletteBytes;
    }
    uint32_t frames = 0;
    uint32_t playCount = 1;
    bool end = false;
    uint64_t metadata = 0;
    uint64_t work = 0;
    uint64_t maxFrameArea = 0;
    uint64_t decodeWork = 0;
    uint8_t disposal = 0;
    if (!addChecked(size, uint64_t(info->extent.x) * info->extent.y, &decodeWork) || decodeWork > limits.work)
        return PrStatus_Limit;
    while (at < size) {
        if (++work > limits.work)
            return PrStatus_Limit;
        const uint8_t block = bytes[at++];
        if (block == 0x3B) {
            end = true;
            break;
        }
        if (block == 0x21) {
            if (at >= size)
                break;
            const uint8_t label = bytes[at++];
            const size_t start = at;
            if (label == 0xFF && size - at >= 16 && bytes[at] == 11 &&
                    prMemEquals(bytes + at + 1, "NETSCAPE2.0", 11) && bytes[at + 12] == 3 && bytes[at + 13] == 1) {
                const uint32_t repeats = prMemReadLeU16(bytes + at + 14);
                // NETSCAPE2.0 counts the repeats after the first play. Zero means indefinitely.
                playCount = repeats == 0 ? 0 : repeats + 1;
            }
            if (label == 0xF9 && size - at >= 6 && bytes[at] == 4)
                disposal = (bytes[at + 1] >> 2) & 7;
            if (!skipBlocks(bytes, size, &at))
                break;
            if (!addChecked(metadata, at - start, &metadata) || metadata > limits.metadataBytes)
                return PrStatus_Limit;
            continue;
        }
        if (block != 0x2C)
            return PrStatus_InvalidData;
        if (size - at < 9)
            break;
        const uint32_t width = prMemReadLeU16(bytes + at + 4);
        const uint32_t height = prMemReadLeU16(bytes + at + 6);
        const uint8_t imagePacked = bytes[at + 8];
        if (width == 0 || height == 0)
            return PrStatus_InvalidData;
        uint64_t frameWork = 0;
        if (!multiplyChecked(width, height, &frameWork) || frameWork > limits.pixels ||
                !multiplyChecked(frameWork, 2, &frameWork) ||
                !addChecked(decodeWork, frameWork, &decodeWork) || decodeWork > limits.work)
            return PrStatus_Limit;
        if (disposal == 3) {
            uint64_t snapshotWork = 0;
            if (!multiplyChecked(uint64_t(info->extent.x) * info->extent.y, 2, &snapshotWork) ||
                    !addChecked(decodeWork, snapshotWork, &decodeWork) || decodeWork > limits.work)
                return PrStatus_Limit;
        }
        disposal = 0;
        const uint64_t frameArea = uint64_t(width) * height;
        maxFrameArea = frameArea > maxFrameArea ? frameArea : maxFrameArea;
        at += 9;
        if ((imagePacked & 0x80) != 0) {
            const size_t paletteBytes = size_t(2u << (imagePacked & 7)) * 3;
            if (paletteBytes > size - at)
                break;
            at += paletteBytes;
        }
        if (at >= size)
            break;
        ++at;
        // A frame whose data the bytes cut short counts: it decodes as far as its data goes.
        bool whole = skipBlocks(bytes, size, &at);
        if (++frames > limits.frames)
            return PrStatus_Limit;
        if (!whole)
            break;
    }
    info->frameCount = frames;
    info->playCount = playCount;
    uint64_t pixels = 0;
    uint64_t output = 0;
    uint64_t frameBytes = 0;
    uint64_t extra = 0;
    uint64_t working = 0;
    // The screen and its snapshot take 8 bytes a pixel, the frames' records 256 bytes each, a frame's indices two bytes
    // a pixel, and the decoder two megabytes.
    if (!multiplyChecked(info->extent.x, info->extent.y, &pixels) || pixels > limits.pixels ||
            !multiplyChecked(pixels, 4, &output) || output > limits.outputBytes ||
            !multiplyChecked(frames, 256, &frameBytes) || !multiplyChecked(maxFrameArea, 2, &extra) ||
            !addChecked(extra, frameBytes, &extra) || !addChecked(extra, 2u << 20, &extra) ||
            !multiplyChecked(pixels, 8, &working) || !addChecked(working, extra, &working) ||
            !addChecked(working, held, &working) || working > limits.workingBytes)
        return PrStatus_Limit;
    info->workingBytes = working;
    return end && frames != 0 ? PrStatus_Ok : PrStatus_Truncated;
}

// A GIF being decoded. It keeps the composed screen from one frame to the next.
struct GifAnimation {
    const uint8_t* bytes;   // the encoded GIF, valid until the decoder is destroyed
    size_t size;
    Screen screen;
    RawArray<Frame> frames;
    LzwTables tables;
    uint8_t* pixels;        // the composed screen, with frame `at` drawn over the ones before it
    uint8_t* previous;      // the snapshot that a restore-to-previous disposal restores; null until needed
    uint8_t* indices;       // one frame's palette indices
    size_t pixelsSize;
    size_t indicesSize;
    uint32_t at;            // the frame that `pixels` holds, or kNoFrame
    uint64_t work;          // the pixels of the frames that reads composed
};

void closeAnimation(GifAnimation* PR_NULLABLE animation) {
    if (animation == nullptr)
        return;
    animation->frames.reset();
    prMemoryFree(animation->tables.prefix);
    prMemoryFree(animation->tables.suffix);
    prMemoryFree(animation->tables.stack);
    prMemoryFree(animation->pixels);
    prMemoryFree(animation->previous);
    prMemoryFree(animation->indices);
    prMemoryFree(animation);
}

// Opens the GIF in `bytes`. Returns null for invalid data or for dimensions past the decoder's limit.
GifAnimation* PR_NULLABLE openAnimation(PrByteView bytes) {
    Screen screen = {};
    if (!readScreen(bytes.data, bytes.size, screen))
        return nullptr;
    GifAnimation* animation = allocZeroed<GifAnimation>();
    animation->bytes = bytes.data;
    animation->size = bytes.size;
    animation->screen = screen;
    animation->at = kNoFrame;
    readFrames(bytes.data, bytes.size, animation->screen, 0xFFFFFFFFu, animation->frames);
    if (animation->frames.count == 0) {
        closeAnimation(animation);
        return nullptr;
    }
    animation->pixelsSize = size_t(screen.width) * size_t(screen.height) * 4;
    animation->pixels = static_cast<uint8_t*>(prMemoryAlloc(animation->pixelsSize));
    const size_t widest = widestFrame(animation->frames);
    animation->indices = widest == 0 ? nullptr : static_cast<uint8_t*>(prMemoryAlloc(widest));
    animation->indicesSize = widest;
    animation->tables = lzwTables();
    return animation;
}

// Composes frames from the current frame of the screen through `frame`, drawing each over the disposal of the one
// before it. A frame at or before the current frame of the screen restarts the walk from an empty screen.
bool composeFrame(GifAnimation& animation, uint32_t frame) {
    if (frame >= animation.frames.count)
        return false;
    if (animation.at == kNoFrame || frame < animation.at) {
        prMemZero(animation.pixels, animation.pixelsSize);
        animation.at = kNoFrame;
    }
    while (animation.at != frame) {
        const uint32_t next = animation.at == kNoFrame ? 0 : animation.at + 1;
        if (animation.at != kNoFrame) {
            const Frame& done = animation.frames.data[animation.at];
            if (done.disposal == GifDisposal_Background)
                clearFrame(done, animation.screen, animation.pixels);
            else if (done.disposal == GifDisposal_Previous)
                prMemCopy(animation.pixels, animation.previous, animation.pixelsSize);
        }
        const Frame& current = animation.frames.data[next];
        if (current.disposal == GifDisposal_Previous) {
            if (animation.previous == nullptr)
                animation.previous = static_cast<uint8_t*>(prMemoryAlloc(animation.pixelsSize));
            prMemCopy(animation.previous, animation.pixels, animation.pixelsSize);
        }
        size_t written = 0;
        const size_t count = size_t(current.width) * size_t(current.height);
        if (!decodeLzw(animation.bytes, animation.size, current, animation.tables, animation.indices, count, &written)) {
            animation.at = kNoFrame;
            return false;
        }
        drawFrame(current, animation.indices, written, animation.screen, animation.pixels);
        animation.work += count;
        animation.at = next;
    }
    return true;
}

// The state of a GIF whose bytes are appended. Each byte is walked once: the blocks as they complete, and the coded
// data of the frame that is arriving, which expands into palette indices and draws onto the screen as its codes arrive.
// `pixels` is the screen with the frames before the arriving one composed and that frame drawn as far as it has
// arrived. A frame's palette is kept as an offset into `bytes`, which move as they grow, or SIZE_MAX for the global
// one.
struct GifStream {
    RawArray<uint8_t> bytes;
    PrGifLimits limits;
    size_t walked;                      // the offset of the next block that the walk has not passed
    bool screened;                      // the header and the global table have arrived
    bool ended;                         // the trailer has arrived
    PrStatus failed;
    uint64_t work;
    Screen screen;
    size_t screenPalette;               // the offset of the global table
    RawArray<Frame> frames;
    RawArray<size_t> palettes;          // the offset of each frame's table, or SIZE_MAX for the global one
    Frame control;                      // the graphic control of the next frame
    uint8_t* PR_NULLABLE pixels;
    uint8_t* PR_NULLABLE previous;      // the screen that a frame disposed to the previous restores

    // The frame that is arriving: its codes as far as they have arrived, the dictionary, and where its next pixel goes.
    bool coding;                        // the frame's coded data is arriving
    Bits in;
    LzwTables tables;
    uint32_t minCodeSize;
    uint32_t next;
    uint32_t width;
    uint32_t previousCode;
    uint8_t previousFirst;
    size_t written;
    uint32_t row;
    uint32_t pass;
    uint32_t target;                    // the row of the frame that `row` stands for, in an interlaced frame
};

// Draws `value`, the next index of the arriving frame, onto the screen, and moves to the pixel after it.
void streamPixel(GifStream& s, uint8_t value) {
    const Frame& frame = s.frames.data[s.frames.count - 1];
    const uint32_t column = uint32_t(s.written % frame.width);
    const uint32_t screenX = frame.left + column;
    const uint32_t screenY = frame.top + s.target;
    if (screenY < s.screen.height && screenX < s.screen.width && !(frame.transparent && value ==
            frame.transparentIndex) && value < frame.paletteCount) {
        const size_t paletteAt = s.palettes.data[s.frames.count - 1];
        const uint8_t* palette = s.bytes.data + (paletteAt == SIZE_MAX ? s.screenPalette : paletteAt);
        const uint8_t* entry = palette + size_t(value) * 3;
        uint8_t* out = s.pixels + (size_t(screenY) * s.screen.width + screenX) * 4;
        out[0] = entry[0];
        out[1] = entry[1];
        out[2] = entry[2];
        out[3] = 0xFF;
    }
    ++s.written;
    if (column + 1 < frame.width)
        return;
    ++s.row;
    if (!frame.interlaced) {
        s.target = s.row;
        return;
    }
    s.target += kPassStep[s.pass];
    while (s.target >= frame.height && s.pass + 1 < kPassCount) {
        s.pass += 1;
        s.target = kPassOrigin[s.pass];
    }
}

// What a read of the arriving frame's coded data found.
enum : uint8_t {
    StreamRead_Got,         // the bits that it asked for
    StreamRead_Waiting,     // the bytes end before them; the bits read so far stay in the reader
    StreamRead_End,         // the terminator of the frame's data
};
typedef uint8_t StreamRead;

// Reads the next byte of the arriving frame's coded data, moving from one sub-block to the next as they arrive.
StreamRead streamByte(Bits& in, uint8_t* out) {
    while (in.remaining == 0) {
        if (in.at >= in.size)
            return StreamRead_Waiting;
        in.remaining = in.bytes[in.at++];
        if (in.remaining == 0)
            return StreamRead_End;
    }
    if (in.at >= in.size)
        return StreamRead_Waiting;
    *out = in.bytes[in.at++];
    in.remaining -= 1;
    return StreamRead_Got;
}

// Reads the next `width` bits of the arriving frame's coded data into `*out`.
StreamRead streamCode(Bits& in, uint32_t width, uint32_t* out) {
    while (in.count < width) {
        uint8_t byte = 0;
        StreamRead read = streamByte(in, &byte);
        if (read != StreamRead_Got)
            return read;
        in.bits |= uint32_t(byte) << in.count;
        in.count += 8;
    }
    *out = in.bits & ((1u << width) - 1);
    in.bits >>= width;
    in.count -= width;
    return StreamRead_Got;
}

// Expands the codes of the arriving frame that have arrived, and draws their pixels. Returns InvalidData for a code
// that the dictionary cannot hold. Stops at the terminator of the frame's data, or where the bytes end, from where the
// next append continues.
PrStatus streamCodes(GifStream& s) {
    const Frame& frame = s.frames.data[s.frames.count - 1];
    const size_t count = size_t(frame.width) * frame.height;
    s.in.bytes = s.bytes.data;
    s.in.size = s.bytes.count;
    const uint32_t clear = 1u << s.minCodeSize;
    const uint32_t end = clear + 1;
    for (;;) {
        const size_t before = s.in.at;
        uint32_t code = 0;
        StreamRead read = streamCode(s.in, s.width, &code);
        s.work += s.in.at - before;
        if (read == StreamRead_End) {
            s.coding = false;
            return PrStatus_Ok;
        }
        if (read == StreamRead_Waiting)
            return PrStatus_Ok;
        if (code == clear) {
            s.next = clear + 2;
            s.width = s.minCodeSize + 1;
            s.previousCode = kNoCode;
            continue;
        }
        if (code == end || s.written >= count) {
            // The rest of the frame's sub-blocks are skipped as they arrive (skipRest).
            s.width = 0;
            return PrStatus_Ok;
        }
        if (code > s.next || (code == s.next && s.previousCode == kNoCode))
            return PrStatus_InvalidData;
        uint32_t top = 0;
        uint32_t walk = code;
        if (code == s.next) {
            s.tables.stack[top++] = s.previousFirst;
            walk = s.previousCode;
        }
        while (walk >= clear + 2) {
            if (top >= kMaxCodes)
                return PrStatus_InvalidData;
            s.tables.stack[top++] = s.tables.suffix[walk];
            walk = s.tables.prefix[walk];
        }
        if (walk >= clear)
            return PrStatus_InvalidData;
        s.tables.stack[top++] = uint8_t(walk);
        const uint8_t first = uint8_t(walk);
        while (top > 0 && s.written < count)
            streamPixel(s, s.tables.stack[--top]);
        s.work += 1;
        if (s.previousCode != kNoCode && s.next < kMaxCodes) {
            s.tables.prefix[s.next] = uint16_t(s.previousCode);
            s.tables.suffix[s.next] = first;
            s.next += 1;
            if (s.next == (1u << s.width) && s.width < kCodeMaxBits)
                s.width += 1;
        }
        s.previousCode = code;
        s.previousFirst = first;
    }
}

// Skips the sub-blocks of the arriving frame after its codes end, as they arrive.
bool skipRest(GifStream& s) {
    while (s.in.at < s.bytes.count) {
        if (s.in.remaining != 0) {
            const size_t step = s.bytes.count - s.in.at < s.in.remaining ? s.bytes.count - s.in.at : s.in.remaining;
            s.in.at += step;
            s.in.remaining -= uint32_t(step);
            continue;
        }
        const uint32_t length = s.bytes.data[s.in.at++];
        if (length == 0) {
            s.coding = false;
            return true;
        }
        s.in.remaining = length;
    }
    return false;
}

// Starts the frame whose image descriptor the stream has just read: disposes of the frame before it as that frame asks,
// keeps the screen if this one is disposed to the previous, and readies the dictionary.
void startFrame(GifStream& s, size_t data) {
    if (s.frames.count > 1) {
        const Frame& done = s.frames.data[s.frames.count - 2];
        if (done.disposal == GifDisposal_Background)
            clearFrame(done, s.screen, s.pixels);
        else if (done.disposal == GifDisposal_Previous && s.previous != nullptr)
            prMemCopy(s.pixels, s.previous, size_t(s.screen.width) * s.screen.height * 4);
    }
    const Frame& frame = s.frames.data[s.frames.count - 1];
    if (frame.disposal == GifDisposal_Previous) {
        const size_t size = size_t(s.screen.width) * s.screen.height * 4;
        if (s.previous == nullptr)
            s.previous = static_cast<uint8_t*>(prMemoryAlloc(size));
        prMemCopy(s.previous, s.pixels, size);
    }
    s.coding = true;
    s.minCodeSize = s.bytes.data[data];
    s.in = Bits{.at = data + 1};
    s.next = (1u << s.minCodeSize) + 2;
    s.width = s.minCodeSize + 1;
    s.previousCode = kNoCode;
    s.written = 0;
    s.row = 0;
    s.pass = 0;
    s.target = 0;
}

// Walks the stream's bytes from where the last walk stopped. Each byte is walked once.
PrStatus walkGif(GifStream& s) {
    const uint8_t* bytes = s.bytes.data;
    const size_t size = s.bytes.count;
    if (!s.screened) {
        if (size < 13)
            return PrStatus_Ok;
        if (!prGifHasSignature({bytes, size}))
            return PrStatus_NotSupported;
        const uint8_t packed = bytes[10];
        const size_t tableBytes = (packed & 0x80) != 0 ? size_t(2u << (packed & 7)) * 3 : 0;
        if (size < 13 + tableBytes)
            return PrStatus_Ok;
        if (!readScreen(bytes, size, s.screen))
            return PrStatus_InvalidData;
        const uint64_t area = uint64_t(s.screen.width) * s.screen.height;
        if (area > s.limits.pixels || area * 4 > s.limits.outputBytes)
            return PrStatus_Limit;
        s.screenPalette = 13;
        s.walked = s.screen.blocks;
        s.pixels = static_cast<uint8_t*>(prMemoryAllocZeroed(size_t(area) * 4));
        s.tables = lzwTables();
        s.control = Frame{};
        s.control.delayMs = delayOf(0);
        s.screened = true;
        s.work += s.walked;
    }
    while (!s.ended) {
        if (s.coding) {
            PrStatus status = s.width != 0 ? streamCodes(s) : PrStatus_Ok;
            if (status != PrStatus_Ok)
                return status;
            if (s.coding && s.width == 0 && !skipRest(s))
                return PrStatus_Ok;
            if (s.coding)
                return PrStatus_Ok;
            s.frames.data[s.frames.count - 1].truncated = false;
            s.walked = s.in.at;
            continue;
        }
        if (s.walked >= size)
            return PrStatus_Ok;
        const uint8_t block = bytes[s.walked];
        if (block == 0x3B) {
            s.ended = true;
            s.walked += 1;
            return PrStatus_Ok;
        }
        if (block == 0x21) {
            // An extension is passed once all of its sub-blocks have arrived.
            size_t at = s.walked + 2;
            if (at > size || !skipSubBlocks(bytes, size, at))
                return PrStatus_Ok;
            const uint8_t label = bytes[s.walked + 1];
            const uint8_t* data = bytes + s.walked + 2;
            const size_t length = at - s.walked - 2;
            if (label == 0xFF && length >= 16 && data[0] == 11 && prMemEquals(data + 1, "NETSCAPE2.0", 11) &&
                    data[12] == 3 && data[13] == 1) {
                const uint32_t repeats = prMemReadLeU16(data + 14);
                s.screen.loops = repeats == 0 ? 0 : repeats + 1;
            }
            if (label == 0xF9 && length >= 6 && data[0] == 4) {
                s.control.disposal = (data[1] >> 2) & 7;
                s.control.transparent = (data[1] & 1) != 0;
                s.control.delayMs = delayOf(prMemReadLeU16(data + 2));
                s.control.transparentIndex = data[4];
            }
            s.work += at - s.walked;
            s.walked = at;
            continue;
        }
        if (block != 0x2C)
            return PrStatus_InvalidData;
        // An image descriptor is passed once it, its local table and its code size have arrived.
        if (size - s.walked < 10)
            return PrStatus_Ok;
        const uint8_t packed = bytes[s.walked + 9];
        const size_t tableBytes = (packed & 0x80) != 0 ? size_t(2u << (packed & 7)) * 3 : 0;
        if (size - s.walked < 11 + tableBytes)
            return PrStatus_Ok;
        Frame frame = s.control;
        frame.left = prMemReadLeU16(bytes + s.walked + 1);
        frame.top = prMemReadLeU16(bytes + s.walked + 3);
        frame.width = prMemReadLeU16(bytes + s.walked + 5);
        frame.height = prMemReadLeU16(bytes + s.walked + 7);
        frame.interlaced = (packed & 0x40) != 0;
        frame.paletteCount = tableBytes != 0 ? 2u << (packed & 7) : s.screen.paletteCount;
        frame.data = s.walked + 10 + tableBytes;
        frame.truncated = true;
        if (frame.width == 0 || frame.height == 0 || s.frames.count >= s.limits.frames)
            return PrStatus_InvalidData;
        if (bytes[frame.data] < 1 || bytes[frame.data] + 1u > kCodeMaxBits)
            return PrStatus_InvalidData;
        s.frames.push(frame);
        s.palettes.push(tableBytes != 0 ? s.walked + 10 : SIZE_MAX);
        s.work += 11 + tableBytes;
        startFrame(s, frame.data);
        s.control = Frame{};
        s.control.delayMs = delayOf(0);
    }
    return PrStatus_Ok;
}

void streamClose(GifStream* PR_NULLABLE s) {
    if (s == nullptr)
        return;
    s->bytes.reset();
    s->frames.reset();
    s->palettes.reset();
    prMemoryFree(s->tables.prefix);
    prMemoryFree(s->tables.suffix);
    prMemoryFree(s->tables.stack);
    prMemoryFree(s->pixels);
    prMemoryFree(s->previous);
    prMemoryFree(s);
}

} // namespace

// A GIF image open for reading. The animation is null for a file that holds no whole frame. A decoder of appended bytes
// has a stream, and an animation over the stream's bytes for the frames before the arriving one.
struct GifDecoder {
    PrGifInfo info;
    GifAnimation* PR_NULLABLE animation;
    GifStream* PR_NULLABLE stream;
};

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrGifDecoder : GifDecoder {};

bool prGifHasSignature(PrByteView bytes) {
    return bytes.size >= 6 && bytes.data[0] == 'G' && bytes.data[1] == 'I' && bytes.data[2] == 'F' &&
        isVersion(bytes.data);
}

PrGifLimits prGifLimitsDefault(void) {
    return PrGifLimits{
        .pixels = 64u * 1024u * 1024u,
        .workingBytes = 512u * 1024u * 1024u,
        .outputBytes = 512u * 1024u * 1024u,
        .work = 1000000000u,
        .frames = 4096,
        .metadataBytes = 16u * 1024u * 1024u,
    };
}

PrStatus prGifDecoderCreate(PrByteView bytes, const PrGifLimits* PR_NULLABLE limits, uint64_t held,
        PrGifDecoder* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    PR_ASSERT(bytes.data != nullptr || bytes.size == 0);
    PR_ASSERT(held >= bytes.size);
    const PrGifLimits budget = limits != nullptr ? *limits : prGifLimitsDefault();
    if (!prGifHasSignature(bytes))
        return PrStatus_NotSupported;
    PrGifInfo info{};
    PrStatus status = inspect(bytes.data, bytes.size, budget, held + sizeof(PrGifDecoder), &info);
    if (status != PrStatus_Ok && status != PrStatus_Truncated)
        return status;
    GifAnimation* animation = openAnimation(bytes);
    if (animation == nullptr && status != PrStatus_Truncated)
        return PrStatus_InvalidData;
    PrGifDecoder* decoder = allocZeroed<PrGifDecoder>();
    decoder->info = info;
    decoder->animation = animation;
    *out = decoder;
    return status;
}

PrStatus prGifDecoderCreateStream(const PrGifLimits* PR_NULLABLE limits, uint64_t held,
        PrGifDecoder* PR_NULLABLE* PR_NONNULL out) {
    const PrGifLimits budget = limits != nullptr ? *limits : prGifLimitsDefault();
    PrGifDecoder* decoder = allocZeroed<PrGifDecoder>();
    GifStream* stream = allocZeroed<GifStream>();
    stream->limits = budget;
    decoder->stream = stream;
    decoder->info.workingBytes = held + sizeof(PrGifDecoder) + sizeof(GifStream);
    *out = decoder;
    return PrStatus_Ok;
}

PrStatus prGifDecoderAppend(PrGifDecoder* decoder, PrByteView bytes) {
    GifStream& s = *decoder->stream;
    if (s.failed != PrStatus_Ok)
        return s.failed;
    if (s.ended)
        return PrStatus_Ok;
    if (s.bytes.count + bytes.size > s.limits.workingBytes) {
        s.failed = PrStatus_Limit;
        return s.failed;
    }
    if (bytes.size != 0)
        s.bytes.append(bytes.data, uint32_t(bytes.size));
    s.work += bytes.size;
    s.failed = walkGif(s);
    if (s.failed != PrStatus_Ok)
        return s.failed;
    if (s.screened) {
        decoder->info.extent = PrUVec2{s.screen.width, s.screen.height};
        decoder->info.frameCount = s.frames.count;
        decoder->info.playCount = s.screen.loops;
    }
    return s.ended ? PrStatus_Ok : PrStatus_Truncated;
}

uint64_t prGifDecoderWork(const PrGifDecoder* decoder) {
    uint64_t work = decoder->stream != nullptr ? decoder->stream->work : 0;
    return work + (decoder->animation != nullptr ? decoder->animation->work : 0);
}

void prGifDecoderDestroy(PrGifDecoder* PR_NULLABLE decoder) {
    if (decoder == nullptr)
        return;
    streamClose(decoder->stream);
    closeAnimation(decoder->animation);
    prMemoryFree(decoder);
}

const PrGifInfo* prGifDecoderInfo(const PrGifDecoder* decoder) {
    return &decoder->info;
}

uint32_t prGifDecoderFrameDuration(const PrGifDecoder* decoder, uint32_t frame) {
    if (const GifStream* stream = decoder->stream)
        return frame < stream->frames.count ? stream->frames.data[frame].delayMs : 0;
    const GifAnimation* animation = decoder->animation;
    return animation != nullptr && frame < animation->frames.count ? animation->frames.data[frame].delayMs : 0;
}

PrStatus prGifDecoderRead(PrGifDecoder* decoder, uint32_t frame, PrPixels output) {
    PR_ASSERT(frame < decoder->info.frameCount);
    PR_ASSERT(output.extent.x == decoder->info.extent.x && output.extent.y == decoder->info.extent.y);
    if (GifStream* s = decoder->stream) {
        const size_t rowBytes = size_t(output.extent.x) * 4;
        // The arriving frame is the stream's screen. A frame before it composes from the stream's bytes as they are.
        if (frame + 1 == s->frames.count) {
            for (uint32_t row = 0; row < output.extent.y; ++row)
                prMemCopy(output.data + size_t(row) * output.stride, s->pixels + size_t(row) * rowBytes, rowBytes);
            return s->frames.data[frame].truncated || !s->ended ? PrStatus_Truncated : PrStatus_Ok;
        }
        if (decoder->animation == nullptr) {
            GifAnimation* animation = allocZeroed<GifAnimation>();
            animation->screen = s->screen;
            animation->at = kNoFrame;
            animation->pixelsSize = size_t(s->screen.width) * s->screen.height * 4;
            animation->pixels = static_cast<uint8_t*>(prMemoryAlloc(animation->pixelsSize));
            animation->tables = lzwTables();
            decoder->animation = animation;
        }
        // The frames view the stream's bytes as they are now, which move as they grow. The screen keeps what it
        // composed, so a read of a later frame continues from the last one read.
        GifAnimation& animation = *decoder->animation;
        animation.bytes = s->bytes.data;
        animation.size = s->bytes.count;
        animation.screen.palette = s->bytes.data + s->screenPalette;
        animation.frames.clear();
        for (uint32_t i = 0; i + 1 < s->frames.count; ++i) {
            Frame whole = s->frames.data[i];
            whole.palette = s->bytes.data + (s->palettes.data[i] == SIZE_MAX ? s->screenPalette : s->palettes.data[i]);
            animation.frames.push(whole);
        }
        const size_t widest = widestFrame(animation.frames);
        if (widest > animation.indicesSize) {
            prMemoryFree(animation.indices);
            animation.indices = static_cast<uint8_t*>(prMemoryAlloc(widest));
            animation.indicesSize = widest;
        }
        if (!composeFrame(animation, frame))
            return PrStatus_InvalidData;
        for (uint32_t row = 0; row < output.extent.y; ++row)
            prMemCopy(output.data + size_t(row) * output.stride, animation.pixels + size_t(row) * rowBytes, rowBytes);
        return s->ended ? PrStatus_Ok : PrStatus_Truncated;
    }
    GifAnimation* animation = decoder->animation;
    if (animation == nullptr || !composeFrame(*animation, frame))
        return PrStatus_InvalidData;
    const size_t rowBytes = size_t(output.extent.x) * 4;
    PR_ASSERT(output.stride >= rowBytes);
    for (uint32_t row = 0; row < output.extent.y; ++row)
        prMemCopy(output.data + size_t(row) * output.stride, animation->pixels + size_t(row) * rowBytes, rowBytes);
    return animation->frames.data[frame].truncated ? PrStatus_Truncated : PrStatus_Ok;
}
