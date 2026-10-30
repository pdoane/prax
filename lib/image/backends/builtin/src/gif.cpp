#include "image/backends/builtin/src/gif.h"

#include "pr/alloc.h"
#include "pr/array.h"
#include "pr/mem.h"

namespace pr_impl {
namespace {

// The maximum decoded pixel count. Larger logical screens are rejected before allocation.
constexpr uint64_t kMaxPixels = 64u * 1024u * 1024u;

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
    if (!gifHasSignature({bytes, size}) || size < 13) {
        return false;
    }
    screen = {};
    screen.loops = 1;
    screen.width = readLeU16(bytes + 6);
    screen.height = readLeU16(bytes + 8);
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
    return screen.width != 0 && screen.height != 0 && uint64_t(screen.width) * uint64_t(screen.height) <= kMaxPixels;
}

// Collects frames from the block chain until reaching `limit` or the trailer. Returns false for a malformed block,
// keeping the frames read before it.
bool readFrames(const uint8_t* bytes, size_t size, Screen& screen, uint32_t limit, Array<Frame>& frames) {
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
                    memEquals(bytes + at + 1, "NETSCAPE2.0", 11) && bytes[at + 12] == 3 && bytes[at + 13] == 1) {
                const uint32_t repeats = readLeU16(bytes + at + 14);
                screen.loops = repeats == 0 ? 0 : repeats + 1;
            }
            if (label == 0xF9 && size - at >= 6 && bytes[at] == 4) {
                const uint8_t packed = bytes[at + 1];
                control.disposal = (packed >> 2) & 7;
                control.transparent = (packed & 1) != 0;
                control.delayMs = delayOf(readLeU16(bytes + at + 2));
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
        frame.left = readLeU16(bytes + at);
        frame.top = readLeU16(bytes + at + 2);
        frame.width = readLeU16(bytes + at + 4);
        frame.height = readLeU16(bytes + at + 6);
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
        if (at >= size || uint64_t(frame.width) * uint64_t(frame.height) > kMaxPixels) {
            return false;
        }
        frame.data = at;
        at += 1;
        if (!skipSubBlocks(bytes, size, at)) {
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
        memZero(out, size_t(columns) * 4);
    }
}

// Indicates that no frame has been composed onto the screen of the animation.
constexpr uint32_t kNoFrame = 0xFFFFFFFFu;

// Returns the pixel count of the largest frame among those that a compose walks.
size_t widestFrame(const Array<Frame>& frames) {
    size_t widest = 0;
    for (uint32_t i = 0; i < frames.count; ++i) {
        const size_t pixels = size_t(frames.data[i].width) * size_t(frames.data[i].height);
        widest = pixels > widest ? pixels : widest;
    }
    return widest;
}

} // namespace

struct GifAnimation {
    const uint8_t* bytes; // encoded GIF, valid until `gifAnimationClose`
    size_t size;
    Screen screen;
    Array<Frame> frames;
    LzwTables tables;
    uint8_t* pixels;   // the composed screen, with frame `at` drawn over the ones before it
    uint8_t* previous; // the snapshot that a restore-to-previous disposal restores; null until needed
    uint8_t* indices;  // one frame's palette indices
    size_t pixelsSize;
    uint32_t at; // the frame that `pixels` holds, or `kNoFrame`
};

bool gifHasSignature(PrByteView bytes) {
    return bytes.size >= 6 && bytes.data[0] == 'G' && bytes.data[1] == 'I' && bytes.data[2] == 'F' &&
        isVersion(bytes.data);
}

GifAnimation* gifAnimationOpen(PrByteView bytes) {
    Screen screen = {};
    if (!readScreen(bytes.data, bytes.size, screen)) {
        return nullptr;
    }
    GifAnimation* animation = allocZeroed<GifAnimation>();
    animation->bytes = bytes.data;
    animation->size = bytes.size;
    animation->screen = screen;
    animation->at = kNoFrame;
    readFrames(bytes.data, bytes.size, animation->screen, 0xFFFFFFFFu, animation->frames);
    if (animation->frames.count == 0) {
        gifAnimationClose(animation);
        return nullptr;
    }
    animation->pixelsSize = size_t(screen.width) * size_t(screen.height) * 4;
    animation->pixels = static_cast<uint8_t*>(prMemoryAlloc(animation->pixelsSize));
    const size_t widest = widestFrame(animation->frames);
    animation->indices = widest == 0 ? nullptr : static_cast<uint8_t*>(prMemoryAlloc(widest));
    animation->tables = lzwTables();
    return animation;
}

void gifAnimationClose(GifAnimation* animation) {
    if (animation != nullptr) {
        animation->frames.reset();
        prMemoryFree(animation->tables.prefix);
        prMemoryFree(animation->tables.suffix);
        prMemoryFree(animation->tables.stack);
        prMemoryFree(animation->pixels);
        prMemoryFree(animation->previous);
        prMemoryFree(animation->indices);
        prMemoryFree(animation);
    }
}

uint32_t gifAnimationDelay(const GifAnimation& animation, uint32_t frame) {
    return frame < animation.frames.count ? animation.frames.data[frame].delayMs : 0;
}

// Composes frames from the current frame of the screen through `frame`, drawing each over the disposal of the one
// before it. A frame at or before the current frame of the screen restarts the walk from an empty screen.
bool gifAnimationFrame(GifAnimation& animation, uint32_t frame, uint8_t* pixels, size_t pixelsSize) {
    if (frame >= animation.frames.count || pixelsSize != animation.pixelsSize) {
        return false;
    }
    if (animation.at == kNoFrame || frame < animation.at) {
        memZero(animation.pixels, animation.pixelsSize);
        animation.at = kNoFrame;
    }
    while (animation.at != frame) {
        const uint32_t next = animation.at == kNoFrame ? 0 : animation.at + 1;
        if (animation.at != kNoFrame) {
            const Frame& done = animation.frames.data[animation.at];
            if (done.disposal == GifDisposal_Background) {
                clearFrame(done, animation.screen, animation.pixels);
            } else if (done.disposal == GifDisposal_Previous) {
                memCopy(animation.pixels, animation.previous, animation.pixelsSize);
            }
        }
        const Frame& current = animation.frames.data[next];
        if (current.disposal == GifDisposal_Previous) {
            if (animation.previous == nullptr) {
                animation.previous = static_cast<uint8_t*>(prMemoryAlloc(animation.pixelsSize));
            }
            memCopy(animation.previous, animation.pixels, animation.pixelsSize);
        }
        size_t written = 0;
        const size_t count = size_t(current.width) * size_t(current.height);
        if (!decodeLzw(animation.bytes, animation.size, current, animation.tables, animation.indices, count, &written)) {
            animation.at = kNoFrame;
            return false;
        }
        drawFrame(current, animation.indices, written, animation.screen, animation.pixels);
        animation.at = next;
    }
    memCopy(pixels, animation.pixels, pixelsSize);
    return true;
}

} // namespace pr_impl
