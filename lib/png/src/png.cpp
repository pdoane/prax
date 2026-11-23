// PNG decoding and encoding.

#include "pr/png.h"

#include "pr/alloc.h"
#include "pr/ascii.h"
#include "pr/checksum.h"
#include "pr/count_of.h"
#include "pr/mem.h"
#include "pr/raw_array.h"

namespace pr_impl {
// PNG color types (png 11.2.2).
enum {
    PngColorType_Grayscale = 0,
    PngColorType_Truecolor = 2,
    PngColorType_Indexed = 3,
    PngColorType_GrayscaleAlpha = 4,
    PngColorType_TruecolorAlpha = 6,
};
typedef uint8_t PngColorType;

// The header fields of the image, with the palette and the transparency from the chunks after the header (png 11.2.2,
// 11.2.3, 11.3.2).
struct Png {
    const uint8_t* palette;
    const uint8_t* transparency;
    uint32_t width;
    uint32_t height;
    uint32_t paletteCount;
    uint32_t transparencySize;
    uint8_t depth;
    PngColorType colorType;
    bool interlaced;
    bool forceAlpha;
};

// One chunk with a view into the encoded input.
struct Chunk {
    const uint8_t* data;
    uint32_t size;
};

// PNG 11.3.6.2: disposal and composition operations are independent frame fields.
enum {
    PngDisposal_None,
    PngDisposal_Background,
    PngDisposal_Previous,
};
typedef uint8_t PngDisposal;

enum {
    PngBlend_Source,
    PngBlend_Over,
};
typedef uint8_t PngBlend;

struct PngFrame {
    uint32_t width;
    uint32_t height;
    uint32_t x;
    uint32_t y;
    uint32_t duration;
    uint32_t dataStart;
    uint32_t dataCount;
    PngDisposal disposal;
    PngBlend blend;
};

struct PngAnimation {
    const PrCompressCodecSet* compress;
    Png png;
    RawArray<PngFrame> frames;
    RawArray<Chunk> chunks;
    RawArray<Chunk> cover;
    uint8_t* pixels;
    uint8_t* previous;
    uint8_t* framePixels;
    size_t pixelsSize;
    uint32_t at;
    uint64_t work;                      // the pixels of the frames that reads decoded
};

// The rows of an image whose data inflates as it arrives: the default image, or the frame of an animation that is
// arriving. Each row that completes is unfiltered and placed in the image's pixels, pass by pass.
struct PngRows {
    Png png;                            // the header, at the extent of the image or of the frame
    PrCompressDecoder* PR_NULLABLE inflate;
    RawArray<uint8_t> rows;             // the filtered row being filled, and the unfiltered row above it
    size_t rowFill;
    uint32_t pass;                      // the pass of the row being filled, or kPassCount once the image is whole
    uint32_t row;                       // its row within the pass
    bool done;
};

// The state of a decoder whose bytes are appended (prPngDecoderCreateStream). Each byte is walked once: the chunks as
// they complete, and the image data as it arrives, which inflates row by row into the decoder's pixels. The frames of
// an animation are recorded as their chunks complete, by their offsets in `bytes`, and decode when they are read. The
// data of the frame that is arriving also inflates row by row as it arrives, so that the frame shows as far as it has
// arrived, as a still image does.
struct PngStream {
    RawArray<uint8_t> bytes;            // every byte appended
    PrPngLimits limits;
    uint64_t held;
    size_t walked;                      // the offset of the first chunk that the walk has not passed
    size_t feedAt;                      // the next byte of image data to inflate
    size_t dataLeft;                    // the bytes of the image data chunk at `feedAt` that are not inflated
    bool described;                     // the chunks before the image data have been read into the info
    bool ended;                         // the IEND chunk has arrived
    PrStatus failed;                    // a failure, which the stream keeps
    uint64_t work;

    // The still image, or the default image of an animation: its header, a copy of its palette and transparency, and
    // the rows that inflate into the decoder's scratch pixels.
    Png png;
    uint8_t palette[768];
    uint8_t transparency[256];
    PngRows image;

    // An animation: its frame count and plays from acTL, its frames, the offsets and sizes of their data chunks, and
    // whether the default image is the first frame.
    bool animated;
    uint32_t plays;
    RawArray<PngFrame> frames;
    RawArray<size_t> chunkOffsets;
    RawArray<uint32_t> chunkSizes;
    uint32_t sequence;
    bool defaultIsFrame;                // the default image is the first frame, whose rows are `image`

    // The frame whose fdAT chunks are arriving: its rows, its pixels, tightly packed at its extent, and its index plus
    // one, or zero. `feedFrame` says that the data at `feedAt` is the frame's.
    PngRows frame;
    RawArray<uint8_t> framePixels;
    uint32_t frameAt;
    bool frameFailed;                   // the frame's data does not decode, so it shows as far as it did
    bool feedFrame;
};

struct PngDecoder {
    PrPngInfo info;
    PrByteView bytes;
    const PrCompressCodecSet* compress;
    uint8_t* PR_NULLABLE profile;       // the expanded ICC profile
    uint8_t* PR_NULLABLE exifText;      // the expanded raw profile that holds the Exif data
    PngAnimation* PR_NULLABLE animation;
    uint8_t* PR_NULLABLE scratch;
    size_t rowBytes;
    size_t pixelsSize;
    PngStream* PR_NULLABLE stream;      // the state of a decoder whose bytes are appended
};

namespace {

// The value of PngAnimation::at before the first frame is composed.
constexpr uint32_t kNoFrame = 0xffffffffu;

// The largest number of bytes of expanded profile text that a raw Exif profile is read from, whatever the metadata
// limit.
constexpr size_t kPngProfileTextMaxBytes = 1u << 20;

// PNG 5.2: the eight-byte signature.
constexpr uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};

// The offsets, steps and count of the passes in which an interlaced image is stored (png 8.2).
constexpr uint32_t kPassCount = 7;
constexpr uint32_t kPassOriginX[kPassCount] = {0, 4, 0, 2, 0, 1, 0};
constexpr uint32_t kPassOriginY[kPassCount] = {0, 0, 4, 0, 2, 0, 1};
constexpr uint32_t kPassStepX[kPassCount] = {8, 8, 4, 4, 2, 2, 1};
constexpr uint32_t kPassStepY[kPassCount] = {8, 8, 8, 4, 4, 2, 2};

// Returns the width and height that one interlace pass of an image of `width` by `height` covers. Either is zero for a
// pass that the image is too small to contain (png 8.2).
PrUVec2 passExtent(uint32_t width, uint32_t height, uint32_t pass) {
    const uint32_t originX = kPassOriginX[pass];
    const uint32_t originY = kPassOriginY[pass];
    return PrUVec2{
        width > originX ? (width - originX + kPassStepX[pass] - 1) / kPassStepX[pass] : 0,
        height > originY ? (height - originY + kPassStepY[pass] - 1) / kPassStepY[pass] : 0,
    };
}

bool isType(const uint8_t* bytes, const char* type) {
    return bytes[0] == uint8_t(type[0]) && bytes[1] == uint8_t(type[1]) &&
        bytes[2] == uint8_t(type[2]) && bytes[3] == uint8_t(type[3]);
}

// Returns the number of samples in one pixel of the color type (png 6.1).
uint32_t channelsOf(PngColorType colorType) {
    if (colorType == PngColorType_Truecolor) {
        return 3;
    }
    if (colorType == PngColorType_GrayscaleAlpha) {
        return 2;
    }
    if (colorType == PngColorType_TruecolorAlpha) {
        return 4;
    }
    return 1;
}

uint32_t outputChannelsOf(const Png& png) {
    if (png.colorType == PngColorType_Indexed) {
        return png.transparency != nullptr || png.forceAlpha ? 4 : 3;
    }
    const uint32_t channels = channelsOf(png.colorType);
    const bool keyedAlpha = png.transparency != nullptr &&
        (png.colorType == PngColorType_Grayscale || png.colorType == PngColorType_Truecolor);
    return channels + (keyedAlpha || (png.forceAlpha && (channels & 1) != 0) ? 1 : 0);
}

uint32_t outputSampleBytesOf(const Png& png) {
    return png.depth == 16 && png.colorType != PngColorType_Indexed ? 2 : 1;
}

// Returns true if the color type allows the bit depth (png 11.2.2).
bool depthAllowed(PngColorType colorType, uint8_t depth) {
    if (colorType == PngColorType_Grayscale) {
        return depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16;
    }
    if (colorType == PngColorType_Indexed) {
        return depth == 1 || depth == 2 || depth == 4 || depth == 8;
    }
    if (colorType == PngColorType_Truecolor || colorType == PngColorType_GrayscaleAlpha ||
            colorType == PngColorType_TruecolorAlpha) {
        return depth == 8 || depth == 16;
    }
    return false;
}

// Reads the header chunk. Returns false if it is short or specifies a combination that the format does not define.
bool readHeader(Png& png, const Chunk& chunk) {
    if (chunk.size < 13) {
        return false;
    }
    png.width = prMemReadBeU32(chunk.data);
    png.height = prMemReadBeU32(chunk.data + 4);
    png.depth = chunk.data[8];
    png.colorType = PngColorType(chunk.data[9]);
    png.interlaced = chunk.data[12] == 1;
    const bool compression = chunk.data[10] == 0;
    const bool filter = chunk.data[11] == 0;
    return png.width != 0 && png.height != 0 && depthAllowed(png.colorType, png.depth) &&
        compression && filter && (chunk.data[12] == 0 || chunk.data[12] == 1);
}

// Reads the physical pixel dimensions chunk, which records how many pixels fill one unit of length. The meter is the
// only unit the format defines (png 11.3.6.2).
void readPhys(const Chunk& chunk, PrVec2* out) {
    if (chunk.size < 9 || chunk.data[8] != 1) {
        return;
    }
    out->x = float(prMemReadBeU32(chunk.data));
    out->y = float(prMemReadBeU32(chunk.data + 4));
}

// Walks the chunk sequence, collecting the header, the palette, the transparency and every run of pixel data. Returns
// false for a chunk structure that runs past the bytes or has no header (png 5.3). If `truncated` is not null, bytes
// that end before the image end are the first bytes of a file instead: the data that they hold is collected, up to the
// end of the bytes, and `*truncated` says whether they end early.
bool readChunks(Png& png, RawArray<Chunk>& data, const uint8_t* bytes, size_t size, bool* PR_NULLABLE truncated) {
    size_t at = sizeof(kSignature);
    bool header = false;
    bool ended = false;
    while (at + 8 <= size && !ended) {
        const uint32_t length = prMemReadBeU32(bytes + at);
        const uint8_t* type = bytes + at + 4;
        if (length > 0x7FFFFFFFu) {
            return false;
        }
        if (size_t(length) + 12 > size - at) {
            if (truncated == nullptr || !header) {
                return false;
            }
            // The data of a chunk that the bytes cut short decodes as far as it goes.
            if (isType(type, "IDAT") && size - at > 8)
                data.push(Chunk{bytes + at + 8, uint32_t(size - at - 8 < length ? size - at - 8 : length)});
            break;
        }
        const Chunk chunk = {bytes + at + 8, length};
        at += size_t(length) + 12;
        if (isType(type, "IHDR")) {
            if (header || !readHeader(png, chunk)) {
                return false;
            }
            header = true;
            continue;
        }
        if (!header) {
            return false;
        }
        if (isType(type, "PLTE")) {
            png.palette = chunk.data;
            png.paletteCount = chunk.size / 3;
        } else if (isType(type, "tRNS")) {
            png.transparency = chunk.data;
            png.transparencySize = chunk.size;
        } else if (isType(type, "IDAT")) {
            data.push(chunk);
        } else if (isType(type, "IEND")) {
            ended = true;
        }
    }
    if (truncated != nullptr)
        *truncated = !ended;
    return header && data.count != 0;
}

// Returns the number of bytes in a row of the given width, excluding its filter byte (png 7.2).
uint64_t rowBytesOf(const Png& png, uint64_t width) {
    return (width * channelsOf(png.colorType) * png.depth + 7) / 8;
}

// Writes the width and height that one interlace pass of the image covers (passExtent).
void passExtentOf(const Png& png, uint32_t pass, uint32_t* outWidth, uint32_t* outHeight) {
    PrUVec2 extent = passExtent(png.width, png.height, pass);
    *outWidth = extent.x;
    *outHeight = extent.y;
}

// Returns the size of the filtered bytes for the whole image: every row of every pass, including its filter byte.
uint64_t filteredSizeOf(const Png& png) {
    if (!png.interlaced) {
        return (rowBytesOf(png, png.width) + 1) * png.height;
    }
    uint64_t total = 0;
    for (uint32_t pass = 0; pass < kPassCount; ++pass) {
        uint32_t width = 0;
        uint32_t height = 0;
        passExtentOf(png, pass, &width, &height);
        if (width != 0 && height != 0) {
            total += (rowBytesOf(png, width) + 1) * height;
        }
    }
    return total;
}

// Returns the Paeth predictor (png 9.4) of the pixel to the left, the one above, and the one above left.
uint8_t paeth(uint8_t left, uint8_t above, uint8_t aboveLeft) {
    const int32_t estimate = int32_t(left) + int32_t(above) - int32_t(aboveLeft);
    const int32_t distanceLeft = estimate > int32_t(left) ? estimate - int32_t(left) : int32_t(left) - estimate;
    const int32_t distanceAbove = estimate > int32_t(above) ? estimate - int32_t(above) : int32_t(above) - estimate;
    const int32_t distanceAboveLeft =
        estimate > int32_t(aboveLeft) ? estimate - int32_t(aboveLeft) : int32_t(aboveLeft) - estimate;
    if (distanceLeft <= distanceAbove && distanceLeft <= distanceAboveLeft) {
        return left;
    }
    return distanceAbove <= distanceAboveLeft ? above : aboveLeft;
}

// Turns one filtered row into its pixel bytes, reading the row above. For the first row of a pass, the row above is all
// zeros. Returns false for a filter type that the format does not define (png 9.2).
bool unfilterRow(uint8_t type, uint8_t* row, const uint8_t* above, size_t size, size_t pixelBytes) {
    const size_t lead = pixelBytes < size ? pixelBytes : size;
    switch (type) {
        case 0:
            return true;
        case 1:
            for (size_t at = pixelBytes; at < size; ++at)
                row[at] = uint8_t(row[at] + row[at - pixelBytes]);
            return true;
        case 2:
            if (above != nullptr) {
                for (size_t at = 0; at < size; ++at)
                    row[at] = uint8_t(row[at] + above[at]);
            }
            return true;
        case 3:
            for (size_t at = 0; at < lead; ++at)
                row[at] = uint8_t(row[at] + (above != nullptr ? above[at] : 0) / 2);
            if (above == nullptr) {
                for (size_t at = pixelBytes; at < size; ++at)
                    row[at] = uint8_t(row[at] + row[at - pixelBytes] / 2);
                return true;
            }
            for (size_t at = pixelBytes; at < size; ++at)
                row[at] = uint8_t(row[at] + (uint32_t(row[at - pixelBytes]) + uint32_t(above[at])) / 2);
            return true;
        case 4: {
            // The first pixel has nothing to its left, so the predictor is the byte above. Without a row above, Paeth
            // predicts the byte to the left, the same as Sub.
            for (size_t at = 0; at < lead; ++at)
                row[at] = uint8_t(row[at] + (above != nullptr ? above[at] : 0));
            if (above == nullptr) {
                for (size_t at = pixelBytes; at < size; ++at)
                    row[at] = uint8_t(row[at] + row[at - pixelBytes]);
                return true;
            }
            for (size_t at = pixelBytes; at < size; ++at)
                row[at] = uint8_t(row[at] + paeth(row[at - pixelBytes], above[at], above[at - pixelBytes]));
            return true;
        }
        default:
            return false;
    }
}

// Returns the sample at `index` in a row of `depth`-bit samples, in the range of that depth.
uint32_t sampleOf(const uint8_t* row, uint32_t depth, uint32_t index) {
    if (depth == 16) {
        return prMemReadBeU16(row + size_t(index) * 2);
    }
    if (depth == 8) {
        return row[index];
    }
    const uint32_t perByte = 8 / depth;
    const uint32_t shift = 8 - depth * (index % perByte + 1);
    return (row[index / perByte] >> shift) & ((1u << depth) - 1);
}

// Returns the sample as an eight-bit component. A depth below eight is scaled to fill the range, and a depth of sixteen
// uses the high byte.
uint8_t toEight(uint32_t sample, uint32_t depth) {
    if (depth == 16) {
        return uint8_t(sample >> 8);
    }
    if (depth == 8) {
        return uint8_t(sample);
    }
    return uint8_t(sample * 255 / ((1u << depth) - 1));
}

// Returns true if the samples match the color that the transparency chunk marks as fully transparent. This applies to
// the color types that specify a transparent color instead of an alpha channel (png 11.3.2.1).
bool isTransparentColor(const Png& png, const uint32_t* samples) {
    const uint32_t channels = channelsOf(png.colorType);
    if (png.colorType != PngColorType_Grayscale && png.colorType != PngColorType_Truecolor) {
        return false;
    }
    if (png.transparency == nullptr || png.transparencySize < channels * 2) {
        return false;
    }
    for (uint32_t channel = 0; channel < channels; ++channel) {
        if (samples[channel] != prMemReadBeU16(png.transparency + size_t(channel) * 2)) {
            return false;
        }
    }
    return true;
}

// Writes the pixels of one unfiltered row into the destination, at the row and column stride at which the pass places
// them. Returns false for a palette index outside the palette.
bool emitRow(const Png& png, const uint8_t* row, uint32_t count, uint32_t originX, uint32_t stepX, uint8_t* destination) {
    const uint32_t channels = channelsOf(png.colorType);
    const uint32_t outChannels = outputChannelsOf(png);
    const uint32_t sampleBytes = outputSampleBytesOf(png);
    // Eight-bit samples that gain no alpha are copied as the bytes of the row.
    if (png.depth == 8 && png.colorType != PngColorType_Indexed && outChannels == channels) {
        uint8_t* out = destination + size_t(originX) * channels;
        if (stepX == 1) {
            prMemCopy(out, row, size_t(count) * channels);
            return true;
        }
        for (uint32_t index = 0; index < count; ++index)
            prMemCopy(out + size_t(index) * stepX * channels, row + size_t(index) * channels, channels);
        return true;
    }
    for (uint32_t index = 0; index < count; ++index) {
        uint32_t samples[4] = {};
        for (uint32_t channel = 0; channel < channels; ++channel) {
            samples[channel] = sampleOf(row, png.depth, index * channels + channel);
        }
        uint8_t* out = destination + size_t(originX + index * stepX) * outChannels * sampleBytes;
        if (png.colorType == PngColorType_Indexed) {
            if (samples[0] >= png.paletteCount) {
                return false;
            }
            const uint8_t* entry = png.palette + size_t(samples[0]) * 3;
            out[0] = entry[0];
            out[1] = entry[1];
            out[2] = entry[2];
            if (outChannels == 4) {
                out[3] = samples[0] < png.transparencySize ? png.transparency[samples[0]] : 255;
            }
            continue;
        }
        if (sampleBytes == 2) {
            uint16_t* output = reinterpret_cast<uint16_t*>(out);
            for (uint32_t channel = 0; channel < channels; ++channel) {
                output[channel] = uint16_t(samples[channel]);
            }
            if (outChannels > channels) {
                output[channels] = isTransparentColor(png, samples) ? 0 : 65535;
            }
        } else {
            for (uint32_t channel = 0; channel < channels; ++channel) {
                out[channel] = toEight(samples[channel], png.depth);
            }
            if (outChannels > channels) {
                out[channels] = isTransparentColor(png, samples) ? 0 : 255;
            }
        }
    }
    return true;
}

// Unfilters the rows of the pass and places their pixels, walking the filtered bytes from `at`. Writes the rows that it
// placed to `*placed`. Returns Truncated if the filtered bytes end before the pass does, and InvalidData for a row of
// no filter type.
PrStatus decodePass(const Png& png, uint8_t* filtered, size_t size, size_t* at, uint32_t count,
        uint32_t rows, uint32_t originX, uint32_t originY, uint32_t stepX, uint32_t stepY, uint8_t* pixels, uint32_t* placed) {
    const size_t rowBytes = size_t(rowBytesOf(png, count));
    const size_t pixelBytes = channelsOf(png.colorType) * png.depth / 8;
    const uint8_t* above = nullptr;
    *placed = 0;
    for (uint32_t row = 0; row < rows; ++row) {
        if (*at + rowBytes + 1 > size) {
            return PrStatus_Truncated;
        }
        uint8_t* line = filtered + *at + 1;
        if (!unfilterRow(filtered[*at], line, above, rowBytes, pixelBytes > 0 ? pixelBytes : 1)) {
            return PrStatus_InvalidData;
        }
        *at += rowBytes + 1;
        above = line;
        uint8_t* destination = pixels + size_t(originY + row * stepY) * size_t(png.width) *
            outputChannelsOf(png) * outputSampleBytesOf(png);
        if (!emitRow(png, line, count, originX, stepX, destination)) {
            return PrStatus_InvalidData;
        }
        *placed = row + 1;
    }
    return PrStatus_Ok;
}

// The pass that stores each pixel of an 8 by 8 block of an interlaced image, from one (png 8.2).
constexpr uint8_t kAdam7[8][8] = {
    {1, 6, 4, 6, 2, 6, 4, 6},
    {7, 7, 7, 7, 7, 7, 7, 7},
    {5, 6, 5, 6, 5, 6, 5, 6},
    {7, 7, 7, 7, 7, 7, 7, 7},
    {3, 6, 4, 6, 3, 6, 4, 6},
    {7, 7, 7, 7, 7, 7, 7, 7},
    {5, 6, 5, 6, 5, 6, 5, 6},
    {7, 7, 7, 7, 7, 7, 7, 7},
};

// The block that each pixel of a pass stands for, by the pass from zero: a pixel of the first covers 8 by 8 pixels, and
// one of the sixth 1 by 2.
constexpr uint32_t kBlockWidth[kPassCount] = {8, 4, 4, 2, 2, 1, 1};
constexpr uint32_t kBlockHeight[kPassCount] = {8, 8, 4, 4, 2, 2, 1};

// Returns true if the pixel at (x, y) of an interlaced image is placed, when `passes` passes are placed whole and the
// next has `rows` rows.
bool placedAt(uint32_t x, uint32_t y, uint32_t passes, uint32_t rows) {
    uint32_t pass = kAdam7[y % 8][x % 8] - 1u;
    if (pass < passes)
        return true;
    return pass == passes && (y - kPassOriginY[pass]) / kPassStepY[pass] < rows;
}

// Fills each pixel of an interlaced image that is not placed with the pixel that stands for its block, when `passes`
// passes are placed whole and the next has `rows` rows, so that the picture shows coarsely before it shows whole. The
// pixel that stands for it is the one of the finest pass placed whose block covers it: the pixel at the origin of the
// pass's block (kBlockWidth, kBlockHeight) that holds it, if that pixel is placed. A pixel shows the 8 by 8 block of
// the first pass, then the 4 by 8 block of the second once the second's pixel of it is placed, and so on.
void fillBlocks(const Png& png, uint32_t passes, uint32_t rows, uint8_t* pixels) {
    const size_t pixelBytes = size_t(outputChannelsOf(png)) * outputSampleBytesOf(png);
    const size_t rowBytes = size_t(png.width) * pixelBytes;
    const uint32_t finest = passes < kPassCount ? passes : kPassCount - 1;
    for (uint32_t y = 0; y < png.height; ++y) {
        for (uint32_t x = 0; x < png.width; ++x) {
            if (placedAt(x, y, passes, rows))
                continue;
            for (uint32_t pass = finest + 1; pass-- > 0;) {
                const uint32_t fromX = x / kBlockWidth[pass] * kBlockWidth[pass];
                const uint32_t fromY = y / kBlockHeight[pass] * kBlockHeight[pass];
                if (placedAt(fromX, fromY, passes, rows)) {
                    prMemCopy(pixels + y * rowBytes + x * pixelBytes, pixels + fromY * rowBytes + fromX * pixelBytes,
                        pixelBytes);
                    break;
                }
            }
        }
    }
}

// Places every pixel in the filtered bytes: in one pass for a non-interlaced image, and in seven for an interlaced one.
// Returns Truncated if the bytes end before the image does: the rows before that are placed, and an interlaced image
// fills the rest of its blocks from the passes placed (fillBlocks).
PrStatus decodePixels(const Png& png, uint8_t* filtered, size_t size, uint8_t* pixels) {
    size_t at = 0;
    uint32_t placed = 0;
    if (!png.interlaced) {
        return decodePass(png, filtered, size, &at, png.width, png.height, 0, 0, 1, 1, pixels, &placed);
    }
    for (uint32_t pass = 0; pass < kPassCount; ++pass) {
        uint32_t count = 0;
        uint32_t rows = 0;
        passExtentOf(png, pass, &count, &rows);
        if (count == 0 || rows == 0) {
            continue;
        }
        PrStatus status = decodePass(png, filtered, size, &at, count, rows, kPassOriginX[pass],
            kPassOriginY[pass], kPassStepX[pass], kPassStepY[pass], pixels, &placed);
        if (status == PrStatus_Truncated)
            fillBlocks(png, pass, placed, pixels);
        if (status != PrStatus_Ok)
            return status;
    }
    return PrStatus_Ok;
}

// ImageMagick raw profiles store a description, a decimal byte count, and hex data. Decoding in place keeps the
// expanded text and the binary metadata in one allocation. Writes the Exif data, after its "Exif" prefix, to `*out` if
// the profile holds it.
void readRawExif(uint8_t* text, size_t size, PrByteView* out) {
    if (size == 0 || text[0] != '\n')
        return;
    size_t at = 1;
    while (at < size && text[at] != '\n')
        ++at;
    while (at < size && prAsciiIsSpace(char(text[at])))
        ++at;
    size_t count = 0;
    const size_t start = at;
    while (at < size && prAsciiIsDigit(char(text[at]))) {
        const uint32_t digit = text[at++] - '0';
        if (count > size / 10 || count * 10 + digit > size)
            return;
        count = count * 10 + digit;
    }
    if (at == start || at == size || !prAsciiIsSpace(char(text[at])) || count < 6)
        return;
    for (size_t index = 0; index < count; ++index) {
        while (at < size && prAsciiIsSpace(char(text[at])))
            ++at;
        if (size - at < 2 || !prAsciiIsHexDigit(char(text[at])) || !prAsciiIsHexDigit(char(text[at + 1])))
            return;
        text[index] = uint8_t(prAsciiHexValue(char(text[at])) * 16 + prAsciiHexValue(char(text[at + 1])));
        at += 2;
    }
    while (at < size && prAsciiIsSpace(char(text[at])))
        ++at;
    constexpr uint8_t kExifPrefix[] = {'E', 'x', 'i', 'f', 0, 0};
    if (at == size && prMemEquals(text, kExifPrefix, sizeof(kExifPrefix)))
        *out = PrByteView{text + sizeof(kExifPrefix), count - sizeof(kExifPrefix)};
}

// Expands the raw profile text `profile` into `*text`, which the caller frees with prMemoryFree, and writes the Exif
// data that it holds to `*exif`. `textLimit` bounds the expanded text. Returns Limit if the text expands past it.
PrStatus readProfile(const PrCompressCodecSet* compress, const Chunk& profile, bool compressed, size_t textLimit,
        uint8_t* PR_NULLABLE* PR_NONNULL text, PrByteView* exif) {
    // The PNG working-storage estimate reserves 2 MiB for codec scratch.
    const size_t limit = textLimit < kPngProfileTextMaxBytes ? textLimit : kPngProfileTextMaxBytes;
    const uint8_t* source = profile.data;
    size_t size = profile.size;
    if (compressed && (size == 0 || source[0] != 0))
        return PrStatus_Ok;
    if (!compressed && size > limit)
        return PrStatus_Limit;
    uint8_t* expanded = static_cast<uint8_t*>(prMemoryAlloc(compressed ? limit + 1 : size));
    size_t written = size;
    bool valid = true;
    if (compressed) {
        PrCompressRead read{};
        PrStatus status = prCompressCodecSetDecode(compress, PrCompressFormat_Zlib, PrByteView{source + 1, size - 1},
            expanded, limit + 1, &read);
        written = read.written;
        valid = status == PrStatus_Ok && read.progress != PrCompressProgress_Input;
    } else {
        prMemCopy(expanded, source, size);
    }
    if (valid && written <= limit)
        readRawExif(expanded, written, exif);
    if (exif->data == nullptr) {
        prMemoryFree(expanded);
        expanded = nullptr;
    }
    *text = expanded;
    return written > limit ? PrStatus_Limit : PrStatus_Ok;
}

// Decodes the scanlines in the zlib stream of the chunks, which must hold exactly the filtered bytes of the image, and
// unfilters them into pixels (png 5.6). If `truncated` is set, the chunks are cut short where the bytes end: the rows
// that their data holds are placed, and the result is Truncated.
PrStatus decodeChunks(const Png& png, const PrCompressCodecSet* compress, const Chunk* chunks, uint32_t chunkCount,
        uint8_t* pixels, bool truncated = false) {
    PrCompressDecoder* decoder = nullptr;
    if (chunkCount == 0 || prCompressDecoderCreate(compress, PrCompressFormat_Zlib, &decoder) != PrStatus_Ok)
        return PrStatus_InvalidData;
    size_t filteredSize = size_t(filteredSizeOf(png));
    uint8_t* filtered = static_cast<uint8_t*>(prMemoryAlloc(filteredSize));
    size_t written = 0;
    bool done = false;
    bool ok = true;
    for (uint32_t i = 0; i < chunkCount && ok && !done; ++i) {
        PrByteView input{chunks[i].data, chunks[i].size};
        bool final = i + 1 == chunkCount && !truncated;
        PrCompressRead read{};
        ok = prCompressDecoderRead(decoder, input, final, filtered + written, filteredSize - written, &read) ==
            PrStatus_Ok && read.progress != PrCompressProgress_Output;
        written += read.written;
        done = read.progress == PrCompressProgress_Done;
    }
    prCompressDecoderDestroy(decoder);
    PrStatus status = PrStatus_InvalidData;
    if (ok && done && written == filteredSize)
        status = decodePixels(png, filtered, filteredSize, pixels);
    else if (ok && truncated && !done)
        status = decodePixels(png, filtered, written, pixels);
    // The rows of a stream that the bytes cut short are a part of the image, even where they end on a pass.
    if (status == PrStatus_Ok && truncated && !done)
        status = PrStatus_Truncated;
    prMemoryFree(filtered);
    return status;
}

// The largest number of bytes in one stored Deflate block (RFC 1951 3.2.4).
constexpr size_t kStoredBlockMaxBytes = 65535;

// Counts the full encoding, while keeping only the bytes that fit in the output capacity.
struct Writer {
    uint8_t* out;
    size_t capacity;
    size_t size;
    uint32_t crc;
};

void writeBytes(Writer& writer, PrByteView bytes) {
    writer.crc = prCrc32Update(writer.crc, bytes);
    if (writer.size < writer.capacity) {
        const size_t available = writer.capacity - writer.size;
        const size_t count = bytes.size < available ? bytes.size : available;
        prMemCopy(writer.out + writer.size, bytes.data, count);
    }
    writer.size += bytes.size;
}

void writeByte(Writer& writer, uint8_t value) {
    writeBytes(writer, {&value, 1});
}

void writeBeU32(Writer& writer, uint32_t value) {
    const uint8_t bytes[] = {uint8_t(value >> 24), uint8_t(value >> 16), uint8_t(value >> 8), uint8_t(value)};
    writeBytes(writer, {bytes, sizeof(bytes)});
}

void beginChunk(Writer& writer, uint32_t length, const char* type) {
    writeBeU32(writer, length);
    writer.crc = 0;
    writeBytes(writer, {reinterpret_cast<const uint8_t*>(type), 4});
}

void endChunk(Writer& writer) {
    writeBeU32(writer, writer.crc);
}

// Returns the size of a zlib stream of `size` bytes in stored blocks: its two header bytes, one block header per 65535
// bytes, and the closing Adler-32. An empty input still takes one final empty block.
size_t storedZlibSize(size_t size) {
    const size_t blocks = size == 0 ? 1 : (size + kStoredBlockMaxBytes - 1) / kStoredBlockMaxBytes;
    return 2 + blocks * 5 + size + 4;
}

// Writes `bytes` as a zlib stream of stored blocks (rfc 1950 2.2, rfc 1951 3.2.4).
void writeStoredZlib(Writer& writer, PrByteView bytes) {
    // Deflate over a 32K window, with no preset dictionary and with the check bits that make the two header bytes a
    // multiple of 31.
    writeByte(writer, 0x78);
    writeByte(writer, 0x01);
    size_t written = 0;
    do {
        const size_t remaining = bytes.size - written;
        const uint32_t block = remaining > kStoredBlockMaxBytes ? uint32_t(kStoredBlockMaxBytes) : uint32_t(remaining);
        writeByte(writer, remaining == block ? 1 : 0);
        writeByte(writer, uint8_t(block));
        writeByte(writer, uint8_t(block >> 8));
        writeByte(writer, uint8_t(~block));
        writeByte(writer, uint8_t((~block) >> 8));
        writeBytes(writer, PrByteView{bytes.data + written, block});
        written += block;
    } while (written < bytes.size);
    writeBeU32(writer, prAdler32Update(1, bytes));
}

// Writes each row of `rgba8` to `out` as its filter type and its filtered bytes (png 7.3, 9.2). Each row takes the
// filter whose bytes, as signed values, sum smallest in magnitude, which png 12.7 recommends for truecolor.
void filterRows(PrPixelView rgba8, uint8_t* out) {
    constexpr uint32_t kPixelBytes = 4;
    constexpr uint32_t kFilters = 5;
    const size_t rowBytes = size_t(rgba8.extent.x) * kPixelBytes;
    uint8_t* candidates = static_cast<uint8_t*>(prMemoryAlloc(kFilters * rowBytes + rowBytes));
    uint8_t* zeros = candidates + kFilters * rowBytes;
    prMemZero(zeros, rowBytes);
    for (uint32_t y = 0; y < rgba8.extent.y; ++y) {
        const uint8_t* row = rgba8.data + size_t(y) * rgba8.stride;
        const uint8_t* up = y == 0 ? zeros : rgba8.data + size_t(y - 1) * rgba8.stride;
        uint64_t best = UINT64_MAX;
        uint32_t chosen = 0;
        for (uint32_t filter = 0; filter < kFilters; ++filter) {
            uint8_t* filteredRow = candidates + filter * rowBytes;
            uint64_t cost = 0;
            for (size_t i = 0; i < rowBytes; ++i) {
                const uint8_t left = i >= kPixelBytes ? row[i - kPixelBytes] : 0;
                const uint8_t upLeft = i >= kPixelBytes ? up[i - kPixelBytes] : 0;
                uint8_t predicted = 0;
                switch (filter) {
                    case 1: predicted = left; break;
                    case 2: predicted = up[i]; break;
                    case 3: predicted = uint8_t((uint32_t(left) + up[i]) / 2); break;
                    case 4: predicted = paeth(left, up[i], upLeft); break;
                    default: break;
                }
                const uint8_t value = uint8_t(row[i] - predicted);
                filteredRow[i] = value;
                cost += value < 128 ? value : 256u - value;
            }
            if (cost < best) {
                best = cost;
                chosen = filter;
            }
        }
        uint8_t* target = out + size_t(y) * (rowBytes + 1);
        target[0] = uint8_t(chosen);
        if (rowBytes != 0)
            prMemCopy(target + 1, candidates + chosen * rowBytes, rowBytes);
    }
    prMemoryFree(candidates);
}

struct PngAnimationInfo {
    uint32_t frames;
    uint32_t plays;
    bool cover;
};

// Validates the entire animation before allocating its frame records or canvases. Bytes that are `partial`, which end
// before the file does, hold the frames that have arrived, and are not refused for the frames that have not.
PrStatus scanAnimation(const uint8_t* bytes, size_t size, const PrPngLimits& limits,
        uint32_t width, uint32_t height, PngAnimationInfo& info, PngAnimation* animation, bool partial) {
    uint32_t frames = 0;
    uint32_t sequence = 0;
    bool imageData = false;
    bool frameData = false;
    bool defaultFrame = false;
    uint64_t work = size;
    for (size_t at = 33; at + 12 <= size;) {
        const uint32_t length = prMemReadBeU32(bytes + at);
        if (length > 0x7FFFFFFFu || size_t(length) + 12 > size - at)
            return info.frames == 0 || partial ? PrStatus_Ok : PrStatus_InvalidData;
        const uint8_t* type = bytes + at + 4;
        const uint8_t* data = bytes + at + 8;
        at += size_t(length) + 12;
        if (isType(type, "acTL")) {
            if (length != 8 || info.frames != 0 || imageData)
                return PrStatus_InvalidData;
            info.frames = prMemReadBeU32(data);
            info.plays = prMemReadBeU32(data + 4);
            if (info.frames == 0)
                return PrStatus_InvalidData;
            if (info.frames > limits.frames)
                return PrStatus_Limit;
        } else if (isType(type, "fcTL") && info.frames != 0) {
            if (length != 26 || prMemReadBeU32(data) != sequence++)
                return PrStatus_InvalidData;
            if ((frames != 0 && !frameData) || frames >= info.frames)
                return PrStatus_InvalidData;
            PngFrame frame = {};
            frame.width = prMemReadBeU32(data + 4);
            frame.height = prMemReadBeU32(data + 8);
            frame.x = prMemReadBeU32(data + 12);
            frame.y = prMemReadBeU32(data + 16);
            const uint32_t denominator = prMemReadBeU16(data + 22) == 0 ? 100 : prMemReadBeU16(data + 22);
            frame.duration = (uint32_t(prMemReadBeU16(data + 20)) * 1000 + denominator / 2) / denominator;
            frame.disposal = data[24];
            frame.blend = data[25];
            if (frame.width == 0 || frame.height == 0 || frame.width > width ||
                    frame.height > height || frame.x > width - frame.width ||
                    frame.y > height - frame.height || frame.disposal > PngDisposal_Previous ||
                    frame.blend > PngBlend_Over)
                return PrStatus_InvalidData;
            if (!imageData) {
                if (frames != 0 || frame.width != width || frame.height != height || frame.x != 0 || frame.y != 0)
                    return PrStatus_InvalidData;
                defaultFrame = true;
            }
            const uint64_t cost = uint64_t(width) * height * 8 + uint64_t(frame.width) * frame.height * 16;
            if (work > limits.work || cost > limits.work - work)
                return PrStatus_Limit;
            work += cost;
            if (animation != nullptr) {
                frame.dataStart = animation->chunks.count;
                animation->frames.push(frame);
            }
            ++frames;
            frameData = false;
        } else if (isType(type, "IDAT")) {
            if (info.frames == 0)
                return PrStatus_Ok;
            imageData = true;
            if (frames != 0) {
                if (!defaultFrame || frames != 1)
                    return PrStatus_InvalidData;
                frameData = true;
                if (animation != nullptr) {
                    animation->chunks.push(Chunk{data, length});
                    ++animation->frames.data[frames - 1].dataCount;
                }
            }
        } else if (isType(type, "fdAT") && info.frames != 0) {
            if (length < 4 || !imageData || frames == 0 || (defaultFrame && frames == 1) ||
                    prMemReadBeU32(data) != sequence++)
                return PrStatus_InvalidData;
            frameData = true;
            if (animation != nullptr) {
                animation->chunks.push(Chunk{data + 4, length - 4});
                ++animation->frames.data[frames - 1].dataCount;
            }
        } else if (isType(type, "IEND")) {
            if (info.frames == 0)
                return PrStatus_Ok;
            if (length != 0 || !imageData || !frameData || frames != info.frames)
                return PrStatus_InvalidData;
            info.cover = !defaultFrame;
            return PrStatus_Ok;
        }
    }
    info.cover = info.cover || (partial && !defaultFrame && imageData);
    return info.frames == 0 || partial ? PrStatus_Ok : PrStatus_InvalidData;
}

// Composes the first `rows` rows of `frame`, whose pixels are `framePixels`, onto `canvas`, the pixels of the image of
// `png`.
template <typename Sample>
void composePngRows(const Png& png, uint8_t* canvas, const uint8_t* framePixels, const PngFrame& frame, uint32_t rows) {
    const uint32_t channels = outputChannelsOf(png);
    const uint32_t colors = channels - 1;
    const uint64_t maximum = sizeof(Sample) == 1 ? 255 : 65535;
    for (uint32_t y = 0; y < rows && y < frame.height; ++y) {
        for (uint32_t x = 0; x < frame.width; ++x) {
            const Sample* source = reinterpret_cast<const Sample*>(framePixels) + (size_t(y) * frame.width + x) * channels;
            Sample* destination = reinterpret_cast<Sample*>(canvas) + (size_t(y + frame.y) * png.width + x + frame.x) * channels;
            if (frame.blend == PngBlend_Source) {
                prMemCopy(destination, source, channels * sizeof(Sample));
                continue;
            }
            const uint64_t alpha = source[colors];
            const uint64_t remainder = destination[colors] * (maximum - alpha);
            const uint64_t combined = alpha * maximum + remainder;
            for (uint32_t color = 0; color < colors; ++color) {
                if (combined == 0)
                    destination[color] = 0;
                else
                    destination[color] =
                        Sample((source[color] * alpha * maximum + destination[color] * remainder + combined / 2) / combined);
            }
            destination[colors] = Sample((combined + maximum / 2) / maximum);
        }
    }
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

// Checks the frames, the extent and the pixels of `info` against `limits`, and sets its working bytes to what the
// decoder holds: `inputSize`, the decoded pixels, the `filteredBytes` of scanlines that a read inflates them from, and
// `fixedBytes`. Returns Limit if they exceed the limits.
PrStatus finishInfo(PrPngInfo* info, const PrPngLimits& limits, uint64_t inputSize, uint64_t filteredBytes,
        uint64_t fixedBytes) {
    uint64_t pixels = 0;
    uint64_t rowBytes = 0;
    uint64_t output = 0;
    if (info->frameCount > limits.frames || !multiplyChecked(info->extent.x, info->extent.y, &pixels) ||
            pixels > limits.pixels ||
            !multiplyChecked(info->extent.x, uint64_t(info->channels) * info->sampleBytes, &rowBytes) ||
            !multiplyChecked(rowBytes, info->extent.y, &output) || output > limits.outputBytes)
        return PrStatus_Limit;
    uint64_t working = 0;
    if (!addChecked(inputSize, output, &working) || !addChecked(working, filteredBytes, &working) ||
            !addChecked(working, fixedBytes, &working) || working > limits.workingBytes)
        return PrStatus_Limit;
    info->workingBytes = working;
    return PrStatus_Ok;
}

// The fixed state of a decoder: its zlib decoder's, its own, and the most that its scans of the chunks hold.
constexpr uint64_t kFixedBytes = 2u << 20;

// Returns the bytes of the list of chunks that the decoder keeps of `size` bytes of a file: at most one for each 12
// bytes, a chunk's least.
uint64_t chunkListBytes(uint64_t size) {
    return size / 12 * sizeof(Chunk);
}

// The ancillary chunks before the image data that say what the image is: the profile, the cICP and pHYs chunks, the
// Exif data and a raw profile of the text chunks that may hold it, and how many bytes the metadata takes.
struct PngMetadata {
    Chunk profile;
    Chunk rawExif;
    bool rawExifCompressed;
    bool namedSrgb;
    uint64_t bytes;
    uint64_t workLeft;      // the work that the limits leave after the pixels
};

// Walks the chunks of `bytes` within `limits`, and fills `info` with the header's extent and samples and the chunks'
// frames, alpha, color and metadata. `held` is the bytes that the caller holds, the file among them. Returns Truncated
// for a file that ends before its image data does.
PrStatus inspect(const uint8_t* bytes, size_t size, const PrPngLimits& limits, uint64_t held, PrPngInfo* info,
        PngMetadata* metadata) {
    if (limits.work == 0)
        return PrStatus_Limit;
    if (size < 29)
        return PrStatus_InvalidData;
    if (prMemReadBeU32(bytes + 8) != 13 || !isType(bytes + 12, "IHDR"))
        return PrStatus_InvalidData;
    info->extent.x = prMemReadBeU32(bytes + 16);
    info->extent.y = prMemReadBeU32(bytes + 20);
    const uint8_t depth = bytes[24];
    const PngColorType colorType = bytes[25];
    if (info->extent.x == 0 || info->extent.y == 0)
        return PrStatus_InvalidData;
    if (!depthAllowed(colorType, depth) || bytes[26] != 0 || bytes[27] != 0 || bytes[28] > 1)
        return PrStatus_NotSupported;
    const bool gray = colorType == PngColorType_Grayscale || colorType == PngColorType_GrayscaleAlpha;
    const bool explicitAlpha = colorType == PngColorType_GrayscaleAlpha || colorType == PngColorType_TruecolorAlpha;
    info->channels = (gray ? 1 : 3) + (explicitAlpha ? 1 : 0);
    info->sampleBytes = depth == 16 ? 2 : 1;
    info->alpha = explicitAlpha;
    info->frameCount = 1;
    info->playCount = 1;
    const Png header{
        .width = info->extent.x,
        .height = info->extent.y,
        .depth = depth,
        .colorType = colorType,
        .interlaced = bytes[28] == 1,
    };
    const uint64_t filtered = filteredSizeOf(header);
    if (size < 33) {
        const PrStatus bounded = finishInfo(info, limits, held + chunkListBytes(size), filtered, kFixedBytes);
        return bounded == PrStatus_Ok ? PrStatus_Truncated : bounded;
    }
    bool palette = false;
    bool transparency = false;
    bool data = false;
    bool end = false;
    bool hasIcc = false;
    bool hasExif = false;
    uint64_t work = 1;
    size_t at = 33;
    while (at < size) {
        if (++work > limits.work)
            return PrStatus_Limit;
        if (size - at < 12)
            break;
        const uint32_t length = prMemReadBeU32(bytes + at);
        if (length > 0x7FFFFFFFu || size_t(length) + 12 > size - at)
            break;
        const uint8_t* type = bytes + at + 4;
        const Chunk chunk = {bytes + at + 8, length};
        if (isType(type, "IHDR"))
            return PrStatus_InvalidData;
        if (isType(type, "PLTE")) {
            palette = length >= 3 && length <= 768 && length % 3 == 0;
            if (!palette)
                return PrStatus_InvalidData;
        } else if (isType(type, "tRNS")) {
            transparency = length != 0;
        } else if (isType(type, "IDAT")) {
            data = true;
        } else if (isType(type, "fdAT")) {
            // Animation pixels count against input and decode work, not metadata.
        } else if (isType(type, "IEND")) {
            end = length == 0;
            if (!end)
                return PrStatus_InvalidData;
            at += size_t(length) + 12;
            break;
        } else {
            if (!addChecked(metadata->bytes, length, &metadata->bytes) || metadata->bytes > limits.metadataBytes)
                return PrStatus_Limit;
            // The chunks that describe the image precede the first IDAT (png-3/#5ChunkOrdering).
            if (!data) {
                if (isType(type, "iCCP")) {
                    hasIcc = true;
                    metadata->profile = chunk;
                } else if (isType(type, "sRGB")) {
                    metadata->namedSrgb = true;
                } else if (isType(type, "cICP") && length == 4) {
                    info->hasCicp = true;
                    info->cicp = PrPngCicp{chunk.data[0], chunk.data[1], chunk.data[2], chunk.data[3]};
                } else if (isType(type, "pHYs")) {
                    readPhys(chunk, &info->pixelsPerMeter);
                } else if (isType(type, "eXIf")) {
                    hasExif = true;
                    info->exif = PrByteView{chunk.data, chunk.size};
                } else if (metadata->rawExif.data == nullptr && (isType(type, "tEXt") || isType(type, "zTXt"))) {
                    constexpr char kKeyword[] = "Raw profile type exif";
                    if (chunk.size > sizeof(kKeyword) && prMemEquals(chunk.data, kKeyword, sizeof(kKeyword))) {
                        metadata->rawExif = {chunk.data + sizeof(kKeyword), uint32_t(chunk.size - sizeof(kKeyword))};
                        metadata->rawExifCompressed = isType(type, "zTXt");
                    }
                }
            }
        }
        at += size_t(length) + 12;
    }
    if (colorType == PngColorType_Indexed && !palette && end)
        return PrStatus_InvalidData;
    if (hasExif)
        metadata->rawExif = Chunk{};
    if (transparency && !explicitAlpha) {
        info->alpha = true;
        ++info->channels;
    }
    info->srgb = metadata->namedSrgb && !hasIcc;
    uint64_t pixelWork = 0;
    if (!multiplyChecked(uint64_t(info->extent.x) * info->extent.y, info->channels, &pixelWork) ||
            !addChecked(pixelWork, size, &pixelWork) || pixelWork > limits.work)
        return PrStatus_Limit;
    metadata->workLeft = limits.work - pixelWork;
    const PrStatus bounded = finishInfo(info, limits, held + chunkListBytes(size), filtered, kFixedBytes);
    if (bounded != PrStatus_Ok)
        return bounded;
    return end && data ? PrStatus_Ok : PrStatus_Truncated;
}

// Expands the iCCP chunk `profile` into at most `limit` bytes. Writes the profile, which the caller frees with
// prMemoryFree, to `*out` and its size to `*size`. Returns false if the chunk is malformed or does not decompress.
bool expandProfile(const PrCompressCodecSet* compress, const Chunk& profile, size_t limit,
        uint8_t* PR_NULLABLE* PR_NONNULL out, size_t* size) {
    *out = nullptr;
    *size = 0;
    if (profile.data == nullptr || limit == 0)
        return false;
    // iCCP holds a name of up to 79 bytes, its terminator, a compression method of zero, then the zlib stream.
    size_t name = 0;
    while (name < profile.size && name < 80 && profile.data[name] != 0)
        ++name;
    if (name + 2 >= profile.size || profile.data[name + 1] != 0)
        return false;
    uint8_t* expanded = static_cast<uint8_t*>(prMemoryAlloc(limit));
    PrCompressRead read{};
    PrStatus status = prCompressCodecSetDecode(compress, PrCompressFormat_Zlib,
        PrByteView{profile.data + name + 2, profile.size - name - 2}, expanded, limit, &read);
    if (status != PrStatus_Ok) {
        prMemoryFree(expanded);
        return false;
    }
    *out = expanded;
    *size = read.written;
    return true;
}

// Decodes the still image of `bytes`, its IDAT chunks, into `pixels` as tightly packed samples of the decoded format.
// Decodes a still image into `pixels`. Bytes that end early decode the rows that they hold, and give Truncated. The
// pixels that they do not reach are zero.
PrStatus decodeStill(const PrCompressCodecSet* compress, PrByteView bytes, uint8_t* pixels, size_t pixelsSize) {
    Png png = {};
    RawArray<Chunk> data;
    bool truncated = false;
    bool ok = readChunks(png, data, bytes.data, bytes.size, &truncated);
    ok = ok && uint64_t(png.width) * uint64_t(png.height) * outputChannelsOf(png) * outputSampleBytesOf(png) ==
        uint64_t(pixelsSize);
    ok = ok && (png.colorType != PngColorType_Indexed || png.paletteCount != 0);
    if (ok && truncated)
        prMemZero(pixels, pixelsSize);
    PrStatus status = ok ? decodeChunks(png, compress, data.data, data.count, pixels, truncated) : PrStatus_InvalidData;
    data.reset();
    return status;
}

void closeAnimation(PngAnimation* PR_NULLABLE animation) {
    if (animation == nullptr)
        return;
    animation->frames.reset();
    animation->chunks.reset();
    animation->cover.reset();
    prMemoryFree(animation->pixels);
    prMemoryFree(animation->previous);
    prMemoryFree(animation->framePixels);
    prMemoryFree(animation);
}

// Scans the animation chunks of `bytes` within `limits`, updates `info` with the animation that they describe, and
// opens the animation. For a PNG that is not animated, returns Ok and sets `*out` to null.
PrStatus openAnimation(const PrCompressCodecSet* compress, PrByteView bytes, const PrPngLimits& limits, PrPngInfo* info,
        bool partial, PngAnimation* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    PngAnimationInfo scanned = {};
    const PrStatus status =
        scanAnimation(bytes.data, bytes.size, limits, info->extent.x, info->extent.y, scanned, nullptr, partial);
    if (status != PrStatus_Ok || scanned.frames == 0)
        return status;
    // The canvas, the frame that composes over it, and the canvas that a frame disposed to the previous restores, each
    // of 8-bit or 16-bit RGBA, and the frames.
    const uint64_t canvas = uint64_t(info->extent.x) * info->extent.y * 4 * info->sampleBytes;
    const uint64_t extra = 3 * canvas + uint64_t(scanned.frames) * sizeof(PngFrame) + sizeof(PngAnimation);
    if (info->workingBytes > limits.workingBytes || extra > limits.workingBytes - info->workingBytes)
        return PrStatus_Limit;
    info->workingBytes += extra;
    info->frameCount = scanned.frames;
    info->playCount = scanned.plays;
    info->hasCover = scanned.cover;
    // The frames compose over a transparent canvas, so an animation always has alpha.
    if (!info->alpha) {
        info->alpha = true;
        ++info->channels;
    }
    const uint64_t pixelsBytes = uint64_t(info->extent.x) * info->extent.y * info->channels * info->sampleBytes;
    if (pixelsBytes == 0 || pixelsBytes > limits.outputBytes)
        return PrStatus_Limit;
    const size_t pixelsSize = size_t(pixelsBytes);
    PngAnimation* animation = allocZeroed<PngAnimation>();
    animation->compress = compress;
    animation->at = kNoFrame;
    animation->pixelsSize = pixelsSize;
    scanned = {};
    bool cut = false;
    const bool parsed = readChunks(animation->png, animation->cover, bytes.data, bytes.size, partial ? &cut : nullptr) &&
        scanAnimation(bytes.data, bytes.size, limits, info->extent.x, info->extent.y, scanned, animation, partial) ==
        PrStatus_Ok;
    if (!parsed) {
        closeAnimation(animation);
        return PrStatus_InvalidData;
    }
    if (!info->hasCover)
        animation->cover.reset();
    animation->png.forceAlpha = true;
    animation->pixels = static_cast<uint8_t*>(prMemoryAlloc(pixelsSize));
    animation->framePixels = static_cast<uint8_t*>(prMemoryAlloc(pixelsSize));
    *out = animation;
    return PrStatus_Ok;
}

// Composes the first `rows` rows of `frame` onto `canvas` in the samples of `png`.
void composeRows(const Png& png, uint8_t* canvas, const uint8_t* framePixels, const PngFrame& frame, uint32_t rows) {
    if (outputSampleBytesOf(png) == 2)
        composePngRows<uint16_t>(png, canvas, framePixels, frame, rows);
    else
        composePngRows<uint8_t>(png, canvas, framePixels, frame, rows);
}

// Disposes `done`, the frame that `canvas` was last composed with, as its fcTL says: clears its region to transparent
// black, or restores the canvas from before it, which the animation saved.
void disposeFrame(const PngAnimation& animation, const PngFrame& done, uint8_t* canvas) {
    const uint32_t pixelBytes = outputChannelsOf(animation.png) * outputSampleBytesOf(animation.png);
    if (done.disposal == PngDisposal_Background) {
        for (uint32_t y = 0; y < done.height; ++y) {
            size_t pixel = size_t(y + done.y) * animation.png.width + done.x;
            prMemZero(canvas + pixel * pixelBytes, size_t(done.width) * pixelBytes);
        }
    } else if (done.disposal == PngDisposal_Previous && animation.previous != nullptr) {
        prMemCopy(canvas, animation.previous, animation.pixelsSize);
    }
}

// Decodes `frame`, composed over the frames before it, into `pixels` as tightly packed, straight-alpha samples of the
// decoded format. Decoding forward continues the composition, and going back restarts it.
bool readAnimationFrame(PngAnimation& animation, uint32_t frame, uint8_t* pixels, size_t pixelsSize) {
    if (pixelsSize != animation.pixelsSize)
        return false;
    if (frame == kPrPngFrameCover && animation.cover.count != 0)
        return decodeChunks(animation.png, animation.compress, animation.cover.data, animation.cover.count, pixels) ==
            PrStatus_Ok;
    if (frame >= animation.frames.count)
        return false;
    if (animation.at == kNoFrame || frame < animation.at) {
        prMemZero(animation.pixels, pixelsSize);
        animation.at = kNoFrame;
    }
    while (animation.at != frame) {
        const uint32_t next = animation.at == kNoFrame ? 0 : animation.at + 1;
        if (animation.at != kNoFrame)
            disposeFrame(animation, animation.frames.data[animation.at], animation.pixels);
        const PngFrame& current = animation.frames.data[next];
        if (current.disposal == PngDisposal_Previous) {
            if (animation.previous == nullptr)
                animation.previous = static_cast<uint8_t*>(prMemoryAlloc(pixelsSize));
            prMemCopy(animation.previous, animation.pixels, pixelsSize);
        }
        Png png = animation.png;
        png.width = current.width;
        png.height = current.height;
        if (decodeChunks(png, animation.compress, animation.chunks.data + current.dataStart, current.dataCount,
                animation.framePixels) != PrStatus_Ok) {
            animation.at = kNoFrame;
            return false;
        }
        composeRows(animation.png, animation.pixels, animation.framePixels, current, current.height);
        animation.work += uint64_t(current.width) * current.height;
        animation.at = next;
    }
    prMemCopy(pixels, animation.pixels, pixelsSize);
    return true;
}

// Returns the extent of pass `pass` of the rows' image, or the whole image if it is not interlaced.
PrUVec2 rowsPassExtent(const PngRows& r, uint32_t pass) {
    return r.png.interlaced ? passExtent(r.png.width, r.png.height, pass) : PrUVec2{r.png.width, r.png.height};
}

// Moves the rows to the next row that has pixels, past passes that have none. Sets the pass to kPassCount once the
// image is whole.
void nextRow(PngRows& r) {
    const uint32_t passes = r.png.interlaced ? kPassCount : 1;
    PrUVec2 extent = rowsPassExtent(r, r.pass);
    if (++r.row < extent.y && extent.x != 0)
        return;
    r.row = 0;
    for (++r.pass; r.pass < passes; ++r.pass) {
        extent = rowsPassExtent(r, r.pass);
        if (extent.x != 0 && extent.y != 0)
            return;
    }
    r.pass = kPassCount;
    r.done = true;
}

// Readies `r` for the data of an image of `png`, from its first row. Returns false if `compress` inflates no zlib.
bool rowsBegin(PngRows& r, const Png& png, const PrCompressCodecSet* compress) {
    prCompressDecoderDestroy(r.inflate);
    r.inflate = nullptr;
    r.png = png;
    if (prCompressDecoderCreate(compress, PrCompressFormat_Zlib, &r.inflate) != PrStatus_Ok)
        return false;
    const size_t widest = size_t(rowBytesOf(png, png.width)) + 1;
    r.rows.resize(uint32_t(widest * 2));
    r.rowFill = 0;
    r.pass = 0;
    r.row = 0;
    r.done = false;
    if (rowsPassExtent(r, 0).x == 0 || rowsPassExtent(r, 0).y == 0) {
        r.row = ~0u;
        nextRow(r);
    }
    return true;
}

void rowsReset(PngRows& r) {
    prCompressDecoderDestroy(r.inflate);
    r.inflate = nullptr;
    r.rows.reset();
}

// Returns the rows of the rows' image that show: every row once it is whole, the rows of its one pass, or the blocks of
// the first pass, which fillBlocks fills from the rows placed.
uint32_t rowsShown(const PngRows& r) {
    if (r.done)
        return r.png.height;
    if (!r.png.interlaced)
        return r.row;
    if (r.pass != 0)
        return r.png.height;
    return r.row * kPassStepY[0] < r.png.height ? r.row * kPassStepY[0] : r.png.height;
}

// Inflates `input`, image data of `r`, and places each row that completes in `pixels`, adding what it did to `work`.
// Returns InvalidData for data that does not decode, and Ok otherwise.
PrStatus streamRows(PngRows& r, PrByteView input, uint8_t* pixels, uint64_t& work) {
    const size_t pixelRowBytes = size_t(r.png.width) * outputChannelsOf(r.png) * outputSampleBytesOf(r.png);
    const size_t pixelBytes = channelsOf(r.png.colorType) * r.png.depth / 8;
    while (!r.done) {
        const PrUVec2 extent = rowsPassExtent(r, r.pass);
        const size_t rowSize = size_t(rowBytesOf(r.png, extent.x)) + 1;
        uint8_t* filling = r.rows.data;
        PrCompressRead read{};
        if (prCompressDecoderRead(r.inflate, input, false, filling + r.rowFill, rowSize - r.rowFill, &read) !=
                PrStatus_Ok)
            return PrStatus_InvalidData;
        input = PrByteView{input.data + read.consumed, input.size - read.consumed};
        r.rowFill += read.written;
        work += read.consumed + read.written;
        if (r.rowFill < rowSize) {
            if (read.progress == PrCompressProgress_Done)
                return PrStatus_InvalidData;
            return PrStatus_Ok;
        }
        // The row above is the last row of the same pass, or none for a pass's first row.
        uint8_t* above = r.rows.data + rowSize;
        if (!unfilterRow(filling[0], filling + 1, r.row == 0 ? nullptr : above, rowSize - 1, pixelBytes > 0 ? pixelBytes : 1))
            return PrStatus_InvalidData;
        const uint32_t originX = r.png.interlaced ? kPassOriginX[r.pass] : 0;
        const uint32_t originY = r.png.interlaced ? kPassOriginY[r.pass] : 0;
        const uint32_t stepX = r.png.interlaced ? kPassStepX[r.pass] : 1;
        const uint32_t stepY = r.png.interlaced ? kPassStepY[r.pass] : 1;
        if (!emitRow(r.png, filling + 1, extent.x, originX, stepX,
                pixels + size_t(originY + r.row * stepY) * pixelRowBytes))
            return PrStatus_InvalidData;
        prMemCopy(above, filling + 1, rowSize - 1);
        work += rowSize;
        r.rowFill = 0;
        nextRow(r);
    }
    return PrStatus_Ok;
}

// Returns the number of the stream's frames that are whole: each frame whose next frame has begun, and every frame once
// the IEND chunk has arrived.
uint32_t streamWholeFrames(const PngStream& s) {
    uint32_t count = s.frames.count;
    return s.ended || count == 0 ? count : count - 1;
}

// Reads the chunks of the stream before its image data into the decoder's info, as prPngDecoderCreate reads a file, and
// readies the rows that the image data inflates into. `prefix` is the offset of the first chunk of image data. The
// metadata is copied out of the stream's bytes, which move as they grow.
PrStatus describeStream(PngDecoder& d, size_t prefix) {
    PngStream& s = *d.stream;
    PngMetadata metadata{};
    PrStatus status = inspect(s.bytes.data, prefix, s.limits, s.held, &d.info, &metadata);
    if (status != PrStatus_Ok && status != PrStatus_Truncated)
        return status;
    if (metadata.rawExif.data != nullptr) {
        const uint64_t metadataLeft = s.limits.metadataBytes - metadata.bytes;
        const uint64_t textLimit = metadataLeft < metadata.workLeft ? metadataLeft : metadata.workLeft;
        PrStatus read = readProfile(d.compress, metadata.rawExif, metadata.rawExifCompressed, size_t(textLimit),
            &d.exifText, &d.info.exif);
        if (read != PrStatus_Ok)
            return read;
    } else if (d.info.exif.size != 0) {
        // The eXIf chunk's data is a view of the stream's bytes, so it is copied.
        d.exifText = static_cast<uint8_t*>(prMemoryAlloc(d.info.exif.size));
        prMemCopy(d.exifText, d.info.exif.data, d.info.exif.size);
        d.info.exif = PrByteView{d.exifText, d.info.exif.size};
    }
    size_t profileSize = 0;
    if (expandProfile(d.compress, metadata.profile, s.limits.metadataBytes, &d.profile, &profileSize))
        d.info.profile = PrByteView{d.profile, profileSize};
    // The header, palette and transparency, from the chunks before the image data.
    RawArray<Chunk> none;
    Png png{};
    readChunks(png, none, s.bytes.data, prefix, nullptr);
    none.reset();
    s.png = png;
    if (png.palette != nullptr) {
        prMemCopy(s.palette, png.palette, size_t(png.paletteCount) * 3);
        s.png.palette = s.palette;
    }
    if (png.transparency != nullptr) {
        const uint32_t size = png.transparencySize < sizeof(s.transparency) ? png.transparencySize : 256;
        prMemCopy(s.transparency, png.transparency, size);
        s.png.transparency = s.transparency;
        s.png.transparencySize = size;
    }
    if (s.png.colorType == PngColorType_Indexed && s.png.paletteCount == 0)
        return PrStatus_InvalidData;
    if (s.animated) {
        // The frames compose over a transparent canvas, so an animation always has alpha.
        s.png.forceAlpha = true;
        if (!d.info.alpha) {
            d.info.alpha = true;
            ++d.info.channels;
        }
        d.info.playCount = s.plays;
    }
    d.rowBytes = size_t(d.info.extent.x) * d.info.channels * d.info.sampleBytes;
    d.pixelsSize = d.rowBytes * d.info.extent.y;
    if (d.pixelsSize == 0 || d.pixelsSize > s.limits.outputBytes)
        return PrStatus_Limit;
    d.scratch = static_cast<uint8_t*>(prMemoryAllocZeroed(d.pixelsSize));
    if (!rowsBegin(s.image, s.png, d.compress))
        return PrStatus_NotSupported;
    s.described = true;
    return PrStatus_Ok;
}

// Walks the stream's bytes from where the last walk stopped: inflates the image data that has arrived, and passes each
// chunk that has arrived whole. Each byte is walked once.
PrStatus walkStream(PngDecoder& d) {
    PngStream& s = *d.stream;
    const size_t size = s.bytes.count;
    if (s.walked == 0) {
        if (size < sizeof(kSignature))
            return PrStatus_Ok;
        if (!prMemEquals(s.bytes.data, kSignature, sizeof(kSignature)))
            return PrStatus_NotSupported;
        s.walked = sizeof(kSignature);
        s.work += sizeof(kSignature);
    }
    while (!s.ended) {
        // The data of the image data chunk that is arriving inflates as it arrives.
        if (s.dataLeft != 0) {
            size_t available = size - s.feedAt < s.dataLeft ? size - s.feedAt : s.dataLeft;
            if (available == 0)
                return PrStatus_Ok;
            const PrByteView input{s.bytes.data + s.feedAt, available};
            if (!s.feedFrame) {
                PrStatus status = streamRows(s.image, input, d.scratch, s.work);
                if (status != PrStatus_Ok)
                    return status;
            } else if (!s.frameFailed) {
                // A frame whose data does not decode fails when it is read whole. Until then it shows what decoded.
                s.frameFailed = streamRows(s.frame, input, s.framePixels.data, s.work) != PrStatus_Ok;
            }
            s.feedAt += available;
            s.dataLeft -= available;
            continue;
        }
        if (size - s.walked < 12)
            return PrStatus_Ok;
        const uint32_t length = prMemReadBeU32(s.bytes.data + s.walked);
        if (length > 0x7FFFFFFFu)
            return PrStatus_InvalidData;
        const uint8_t* type = s.bytes.data + s.walked + 4;
        const bool imageData = isType(type, "IDAT");
        const bool frameData = isType(type, "fdAT");
        if ((imageData || frameData || isType(type, "IEND")) && !s.described) {
            PrStatus status = describeStream(d, s.walked);
            if (status != PrStatus_Ok)
                return status;
        }
        // The image data streams into the rows: only its chunk's header and its CRC wait for whole bytes.
        if (imageData && s.feedAt <= s.walked) {
            s.feedAt = s.walked + 8;
            s.dataLeft = length;
            s.feedFrame = false;
            s.work += 8;
            if (s.animated && s.frames.count != 0) {
                s.defaultIsFrame = true;
                s.chunkOffsets.push(s.walked + 8);
                s.chunkSizes.push(length);
                ++s.frames.data[s.frames.count - 1].dataCount;
            }
            if (length != 0)
                continue;
        }
        // A frame's data streams into the frame's rows as the default image's does, after its sequence number.
        if (frameData && s.animated && s.feedAt <= s.walked) {
            if (size - s.walked < 12)
                return PrStatus_Ok;
            if (length < 4 || s.frames.count == 0 || prMemReadBeU32(s.bytes.data + s.walked + 8) != s.sequence++)
                return PrStatus_InvalidData;
            if (s.frameAt != s.frames.count) {
                const PngFrame& arriving = s.frames.data[s.frames.count - 1];
                Png png = s.png;
                png.width = arriving.width;
                png.height = arriving.height;
                if (!rowsBegin(s.frame, png, d.compress))
                    return PrStatus_NotSupported;
                const size_t bytes = size_t(png.width) * png.height * outputChannelsOf(png) * outputSampleBytesOf(png);
                s.framePixels.resize(uint32_t(bytes));
                prMemZero(s.framePixels.data, bytes);
                s.frameAt = s.frames.count;
                s.frameFailed = false;
            }
            s.chunkOffsets.push(s.walked + 12);
            s.chunkSizes.push(length - 4);
            ++s.frames.data[s.frames.count - 1].dataCount;
            s.feedAt = s.walked + 12;
            s.dataLeft = length - 4;
            s.feedFrame = true;
            s.work += 12;
            if (s.dataLeft != 0)
                continue;
        }
        if (size - s.walked < size_t(length) + 12)
            return PrStatus_Ok;
        const uint8_t* data = s.bytes.data + s.walked + 8;
        s.work += 12 + (imageData || frameData ? 0 : length);
        if (isType(type, "acTL") && length == 8 && !s.described) {
            s.animated = true;
            s.plays = prMemReadBeU32(data + 4);
            if (prMemReadBeU32(data) > s.limits.frames)
                return PrStatus_Limit;
        } else if (isType(type, "fcTL") && s.animated) {
            if (length != 26 || prMemReadBeU32(data) != s.sequence++)
                return PrStatus_InvalidData;
            PngFrame frame = {};
            frame.width = prMemReadBeU32(data + 4);
            frame.height = prMemReadBeU32(data + 8);
            frame.x = prMemReadBeU32(data + 12);
            frame.y = prMemReadBeU32(data + 16);
            const uint32_t denominator = prMemReadBeU16(data + 22) == 0 ? 100 : prMemReadBeU16(data + 22);
            frame.duration = (uint32_t(prMemReadBeU16(data + 20)) * 1000 + denominator / 2) / denominator;
            frame.disposal = data[24];
            frame.blend = data[25];
            const PrUVec2 extent{prMemReadBeU32(s.bytes.data + 16), prMemReadBeU32(s.bytes.data + 20)};
            if (frame.width == 0 || frame.height == 0 || frame.width > extent.x || frame.height > extent.y ||
                    frame.x > extent.x - frame.width || frame.y > extent.y - frame.height ||
                    frame.disposal > PngDisposal_Previous || frame.blend > PngBlend_Over ||
                    s.frames.count >= s.limits.frames)
                return PrStatus_InvalidData;
            frame.dataStart = s.chunkOffsets.count;
            s.frames.push(frame);
        } else if (isType(type, "IEND")) {
            s.ended = true;
        }
        s.walked += size_t(length) + 12;
        s.feedAt = s.walked;
    }
    return PrStatus_Ok;
}

// Returns the animation that reads the whole frames of a stream, made at its first use, with the frames and chunks of
// the stream as they are now. The frames' chunks view the stream's bytes, which move as they grow. The animation keeps
// what it composed, so a read of a later frame continues from the last one read.
PngAnimation& streamAnimation(PngDecoder& d, uint32_t whole) {
    PngStream& s = *d.stream;
    if (d.animation == nullptr) {
        d.animation = allocZeroed<PngAnimation>();
        d.animation->compress = d.compress;
        d.animation->at = kNoFrame;
        d.animation->pixelsSize = d.pixelsSize;
        d.animation->png = s.png;
        d.animation->pixels = static_cast<uint8_t*>(prMemoryAlloc(d.pixelsSize));
        d.animation->framePixels = static_cast<uint8_t*>(prMemoryAlloc(d.pixelsSize));
    }
    PngAnimation& animation = *d.animation;
    animation.frames.clear();
    animation.frames.append(s.frames.data, whole);
    animation.chunks.clear();
    for (uint32_t i = 0; i < s.chunkOffsets.count; ++i)
        animation.chunks.push(Chunk{s.bytes.data + s.chunkOffsets.data[i], s.chunkSizes.data[i]});
    return animation;
}

// Copies tightly packed pixels of the decoder's image to `output`.
void copyOut(const PngDecoder& d, const uint8_t* pixels, PrPixels output) {
    for (uint32_t row = 0; row < output.extent.y; ++row)
        prMemCopy(output.data + size_t(row) * output.stride, pixels + size_t(row) * d.rowBytes, d.rowBytes);
}

// Reads `frame` of a stream. A whole frame of an animation decodes from its chunks. The frame that is arriving shows
// the frames before it composed and disposed, with its rows composed over them as far as they have arrived. The still
// image, or a default image that is the first frame while it arrives, is the rows that have arrived. An interlaced
// image that is not whole fills each block of the passes that have arrived (fillBlocks).
PrStatus readStream(PngDecoder& d, uint32_t frame, PrPixels output) {
    PngStream& s = *d.stream;
    if (!s.described)
        return PrStatus_InvalidData;
    const uint32_t whole = streamWholeFrames(s);
    if (s.animated && frame < whole) {
        PngAnimation& animation = streamAnimation(d, whole);
        uint8_t* pixels = static_cast<uint8_t*>(prMemoryAlloc(d.pixelsSize));
        bool read = readAnimationFrame(animation, frame, pixels, d.pixelsSize);
        if (read)
            copyOut(d, pixels, output);
        prMemoryFree(pixels);
        if (!read)
            return PrStatus_InvalidData;
        return s.ended ? PrStatus_Ok : PrStatus_Truncated;
    }
    const uint32_t arriving = s.frames.count != 0 ? s.frames.count - 1 : 0;
    if (s.animated && s.frames.count != 0 && !(arriving == 0 && s.defaultIsFrame)) {
        uint8_t* canvas = static_cast<uint8_t*>(prMemoryAllocZeroed(d.pixelsSize));
        if (arriving != 0) {
            PngAnimation& animation = streamAnimation(d, whole);
            if (!readAnimationFrame(animation, arriving - 1, canvas, d.pixelsSize)) {
                prMemoryFree(canvas);
                return PrStatus_InvalidData;
            }
            disposeFrame(animation, s.frames.data[arriving - 1], canvas);
        }
        // The rows of the frame that have arrived, if its data has begun.
        if (s.frameAt == s.frames.count) {
            if (s.frame.png.interlaced && !s.frame.done)
                fillBlocks(s.frame.png, s.frame.pass, s.frame.row, s.framePixels.data);
            composeRows(s.png, canvas, s.framePixels.data, s.frames.data[arriving], rowsShown(s.frame));
        }
        copyOut(d, canvas, output);
        prMemoryFree(canvas);
        return PrStatus_Truncated;
    }
    if (s.png.interlaced && !s.image.done)
        fillBlocks(s.png, s.image.pass, s.image.row, d.scratch);
    copyOut(d, d.scratch, output);
    return s.image.done && s.ended ? PrStatus_Ok : PrStatus_Truncated;
}

} // namespace

// A PNG image open for reading. The scratch pixels hold a decoded frame, tightly packed, before it is copied out.

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrPngDecoder : PngDecoder {};

bool prPngHasSignature(PrByteView bytes) {
    return bytes.size >= sizeof(kSignature) && prMemEquals(bytes.data, kSignature, sizeof(kSignature));
}

PrPngLimits prPngLimitsDefault(void) {
    return PrPngLimits{
        .pixels = 64u * 1024u * 1024u,
        .workingBytes = 512u * 1024u * 1024u,
        .outputBytes = 512u * 1024u * 1024u,
        .work = 1000000000u,
        .frames = 4096,
        .metadataBytes = 16u * 1024u * 1024u,
    };
}

PrStatus prPngDecoderCreate(PrByteView bytes, const PrCompressCodecSet* compress, const PrPngLimits* PR_NULLABLE limits,
        uint64_t held, PrPngDecoder* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    PR_ASSERT(bytes.data != nullptr || bytes.size == 0);
    PR_ASSERT(held >= bytes.size);
    const PrPngLimits budget = limits != nullptr ? *limits : prPngLimitsDefault();
    if (!prPngHasSignature(bytes))
        return PrStatus_NotSupported;
    if (sizeof(PrPngDecoder) > budget.workingBytes || held > budget.workingBytes - sizeof(PrPngDecoder))
        return PrStatus_Limit;
    held += sizeof(PrPngDecoder);
    PrPngDecoder* decoder = allocZeroed<PrPngDecoder>();
    decoder->bytes = bytes;
    decoder->compress = compress;
    PngMetadata metadata{};
    PrStatus status = inspect(bytes.data, bytes.size, budget, held, &decoder->info, &metadata);
    if (status == PrStatus_Ok || status == PrStatus_Truncated) {
        // The raw profile is the Exif data's only source if the file has no eXIf chunk.
        if (metadata.rawExif.data != nullptr) {
            const uint64_t metadataLeft = budget.metadataBytes - metadata.bytes;
            const uint64_t textLimit = metadataLeft < metadata.workLeft ? metadataLeft : metadata.workLeft;
            PrStatus read = readProfile(compress, metadata.rawExif, metadata.rawExifCompressed, size_t(textLimit),
                &decoder->exifText, &decoder->info.exif);
            if (read != PrStatus_Ok)
                status = read;
        }
        size_t profileSize = 0;
        if (status != PrStatus_Limit &&
                expandProfile(compress, metadata.profile, budget.metadataBytes, &decoder->profile, &profileSize))
            decoder->info.profile = PrByteView{decoder->profile, profileSize};
    }
    if (status == PrStatus_Ok || status == PrStatus_Truncated) {
        PrStatus animated = openAnimation(compress, bytes, budget, &decoder->info, status == PrStatus_Truncated,
            &decoder->animation);
        if (animated != PrStatus_Ok)
            status = animated;
    }
    if (status != PrStatus_Ok && status != PrStatus_Truncated) {
        prPngDecoderDestroy(decoder);
        return status;
    }
    PrPngInfo& info = decoder->info;
    decoder->rowBytes = size_t(info.extent.x) * info.channels * info.sampleBytes;
    decoder->pixelsSize = decoder->rowBytes * info.extent.y;
    decoder->scratch = static_cast<uint8_t*>(prMemoryAlloc(decoder->pixelsSize));
    *out = decoder;
    return status;
}

PrStatus prPngDecoderCreateStream(const PrCompressCodecSet* compress, const PrPngLimits* PR_NULLABLE limits,
        uint64_t held, PrPngDecoder* PR_NULLABLE* PR_NONNULL out) {
    const PrPngLimits budget = limits != nullptr ? *limits : prPngLimitsDefault();
    if (sizeof(PrPngDecoder) + sizeof(PngStream) > budget.workingBytes)
        return PrStatus_Limit;
    PrPngDecoder* decoder = allocZeroed<PrPngDecoder>();
    decoder->compress = compress;
    PngStream* stream = allocZeroed<PngStream>();
    stream->limits = budget;
    stream->held = held + sizeof(PrPngDecoder) + sizeof(PngStream);
    decoder->stream = stream;
    *out = decoder;
    return PrStatus_Ok;
}

PrStatus prPngDecoderAppend(PrPngDecoder* decoder, PrByteView bytes) {
    PngStream& s = *decoder->stream;
    if (s.failed != PrStatus_Ok)
        return s.failed;
    if (s.ended)
        return PrStatus_Ok;
    if (bytes.size != 0) {
        if (s.bytes.count + bytes.size > s.limits.workingBytes) {
            s.failed = PrStatus_Limit;
            return s.failed;
        }
        s.bytes.append(bytes.data, uint32_t(bytes.size));
        s.work += bytes.size;
    }
    s.failed = walkStream(*decoder);
    // An animation counts the frames that have begun, and the default image while none has.
    if (s.described && s.animated)
        decoder->info.frameCount = s.frames.count != 0 ? s.frames.count : 1;
    if (s.failed != PrStatus_Ok)
        return s.failed;
    return s.ended ? PrStatus_Ok : PrStatus_Truncated;
}

uint64_t prPngDecoderWork(const PrPngDecoder* decoder) {
    uint64_t work = decoder->stream != nullptr ? decoder->stream->work : 0;
    return work + (decoder->animation != nullptr ? decoder->animation->work : 0);
}

void prPngDecoderDestroy(PrPngDecoder* PR_NULLABLE decoder) {
    if (decoder == nullptr)
        return;
    if (PngStream* stream = decoder->stream) {
        rowsReset(stream->image);
        rowsReset(stream->frame);
        stream->framePixels.reset();
        stream->bytes.reset();
        stream->frames.reset();
        stream->chunkOffsets.reset();
        stream->chunkSizes.reset();
        // The animation's chunks view the stream's bytes, which go with it.
        prMemoryFree(stream);
    }
    closeAnimation(decoder->animation);
    prMemoryFree(decoder->profile);
    prMemoryFree(decoder->exifText);
    prMemoryFree(decoder->scratch);
    prMemoryFree(decoder);
}

const PrPngInfo* prPngDecoderInfo(const PrPngDecoder* decoder) {
    return &decoder->info;
}

uint32_t prPngDecoderFrameDuration(const PrPngDecoder* decoder, uint32_t frame) {
    if (const PngStream* stream = decoder->stream)
        return frame < stream->frames.count ? stream->frames.data[frame].duration : 0;
    const PngAnimation* animation = decoder->animation;
    return animation != nullptr && frame < animation->frames.count ? animation->frames.data[frame].duration : 0;
}

PrStatus prPngDecoderRead(PrPngDecoder* decoder, uint32_t frame, PrPixels output) {
    const PrPngInfo& info = decoder->info;
    PR_ASSERT(frame < info.frameCount || (frame == kPrPngFrameCover && info.hasCover));
    PR_ASSERT(output.extent.x == info.extent.x && output.extent.y == info.extent.y);
    PR_ASSERT(output.stride >= decoder->rowBytes);
    if (decoder->stream != nullptr)
        return readStream(*decoder, frame, output);
    PrStatus status = PrStatus_InvalidData;
    if (decoder->animation != nullptr) {
        if (readAnimationFrame(*decoder->animation, frame, decoder->scratch, decoder->pixelsSize))
            status = PrStatus_Ok;
    } else {
        status = decodeStill(decoder->compress, decoder->bytes, decoder->scratch, decoder->pixelsSize);
    }
    if (status != PrStatus_Ok && status != PrStatus_Truncated)
        return status;
    for (uint32_t row = 0; row < output.extent.y; ++row) {
        prMemCopy(output.data + size_t(row) * output.stride, decoder->scratch + size_t(row) * decoder->rowBytes,
            decoder->rowBytes);
    }
    return status;
}

size_t prPngEncode(PrPixelView rgba8, PrPngEncodeFlags flags, const PrCompressCodecSet* compress,
        uint8_t* PR_NULLABLE out, size_t capacity) {
    PR_ASSERT(rgba8.extent.x != 0 && rgba8.extent.y != 0 && size_t(rgba8.extent.x) * 4 <= rgba8.stride);
    PR_ASSERT(out != nullptr || capacity == 0);
    const uint32_t width = rgba8.extent.x;
    const uint32_t height = rgba8.extent.y;
    Writer writer = {out, out != nullptr ? capacity : 0, 0, 0};

    for (size_t index = 0; index < countOf(kSignature); ++index)
        writeByte(writer, kSignature[index]);

    // png 11.2.2: the dimensions, then 8 bits per sample, color type 6 for truecolor with alpha, the single compression
    // and filter method the format defines, and no interlacing or Adam7.
    const bool interlaced = (flags & PrPngEncodeFlag_Interlaced) != 0;
    beginChunk(writer, 13, "IHDR");
    writeBeU32(writer, width);
    writeBeU32(writer, height);
    writeByte(writer, 8);
    writeByte(writer, 6);
    writeByte(writer, 0);
    writeByte(writer, 0);
    writeByte(writer, interlaced ? 1 : 0);
    endChunk(writer);

    // Each pass of an interlaced image is a reduced image of its own, filtered on its own (png 8.2).
    size_t filteredSize = 0;
    const uint32_t passes = interlaced ? kPassCount : 1;
    for (uint32_t pass = 0; pass < passes; ++pass) {
        PrUVec2 extent = interlaced ? passExtent(width, height, pass) : PrUVec2{width, height};
        if (extent.x != 0 && extent.y != 0)
            filteredSize += size_t(extent.y) * (size_t(extent.x) * 4 + 1);
    }
    uint8_t* filtered = static_cast<uint8_t*>(prMemoryAlloc(filteredSize + 1));
    if (!interlaced) {
        filterRows(rgba8, filtered);
    } else {
        uint8_t* reduced = static_cast<uint8_t*>(prMemoryAlloc(size_t(width) * height * 4));
        size_t at = 0;
        for (uint32_t pass = 0; pass < kPassCount; ++pass) {
            PrUVec2 extent = passExtent(width, height, pass);
            if (extent.x == 0 || extent.y == 0)
                continue;
            for (uint32_t y = 0; y < extent.y; ++y) {
                const uint8_t* row = rgba8.data + size_t(kPassOriginY[pass] + y * kPassStepY[pass]) * rgba8.stride;
                for (uint32_t x = 0; x < extent.x; ++x) {
                    const uint8_t* pixel = row + size_t(kPassOriginX[pass] + x * kPassStepX[pass]) * 4;
                    prMemCopy(reduced + (size_t(y) * extent.x + x) * 4, pixel, 4);
                }
            }
            filterRows(PrPixelView{reduced, size_t(extent.x) * 4, extent}, filtered + at);
            at += size_t(extent.y) * (size_t(extent.x) * 4 + 1);
        }
        prMemoryFree(reduced);
    }
    const PrByteView rows{filtered, filteredSize};

    // The rows go in one zlib stream, compressed by the set, or stored if the set encodes no zlib.
    const size_t bound = prCompressCodecSetEncodeBound(compress, PrCompressFormat_Zlib, filteredSize);
    uint8_t* stream = bound != 0 ? static_cast<uint8_t*>(prMemoryAlloc(bound)) : nullptr;
    size_t streamSize = 0;
    if (stream != nullptr && prCompressCodecSetEncode(compress, PrCompressFormat_Zlib, PrCompressLevel_Default, rows, stream,
            &streamSize) == PrStatus_Ok) {
        beginChunk(writer, uint32_t(streamSize), "IDAT");
        writeBytes(writer, PrByteView{stream, streamSize});
        endChunk(writer);
    } else {
        beginChunk(writer, uint32_t(storedZlibSize(filteredSize)), "IDAT");
        writeStoredZlib(writer, rows);
        endChunk(writer);
    }
    prMemoryFree(stream);
    prMemoryFree(filtered);

    beginChunk(writer, 0, "IEND");
    endChunk(writer);
    return writer.size;
}
