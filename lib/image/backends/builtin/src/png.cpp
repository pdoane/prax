#include "image/backends/builtin/src/png.h"

#include "pr/alloc.h"
#include "pr/array.h"
#include "pr/ascii.h"
#include "pr/checksum.h"
#include "pr/compress.h"
#include "pr/count_of.h"
#include "pr/mem.h"

#include "image/backends/builtin/src/metadata.h"

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
    PrCompressCodec* codec;
    Png png;
    Array<PngFrame> frames;
    Array<Chunk> chunks;
    Array<Chunk> cover;
    uint8_t* pixels;
    uint8_t* previous;
    uint8_t* framePixels;
    size_t pixelsSize;
    uint32_t at;
};

namespace {

// The value of PngAnimation::at before the first frame is composed.
constexpr uint32_t kNoFrame = 0xffffffffu;

// The maximum decoded pixel count. Larger images are rejected before allocation.
constexpr uint64_t kMaxPixels = 64u * 1024u * 1024u;

// PNG 5.2: the eight-byte signature.
constexpr uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};

// The offsets, steps and count of the passes in which an interlaced image is stored (png 8.2).
constexpr uint32_t kPassCount = 7;
constexpr uint32_t kPassOriginX[kPassCount] = {0, 4, 0, 2, 0, 1, 0};
constexpr uint32_t kPassOriginY[kPassCount] = {0, 0, 4, 0, 2, 0, 1};
constexpr uint32_t kPassStepX[kPassCount] = {8, 8, 4, 4, 2, 2, 1};
constexpr uint32_t kPassStepY[kPassCount] = {8, 8, 8, 4, 4, 2, 2};

bool isType(const uint8_t* bytes, const char* type) {
    return bytes[0] == uint8_t(type[0]) && bytes[1] == uint8_t(type[1])
           && bytes[2] == uint8_t(type[2]) && bytes[3] == uint8_t(type[3]);
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
    const bool keyedAlpha =
        png.transparency != nullptr
        && (png.colorType == PngColorType_Grayscale || png.colorType == PngColorType_Truecolor);
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
    if (colorType == PngColorType_Truecolor || colorType == PngColorType_GrayscaleAlpha
        || colorType == PngColorType_TruecolorAlpha) {
        return depth == 8 || depth == 16;
    }
    return false;
}

// Reads the header chunk. Returns false if it is short or specifies a combination that the format does not define.
bool readHeader(Png& png, const Chunk& chunk) {
    if (chunk.size < 13) {
        return false;
    }
    png.width = readBeU32(chunk.data);
    png.height = readBeU32(chunk.data + 4);
    png.depth = chunk.data[8];
    png.colorType = PngColorType(chunk.data[9]);
    png.interlaced = chunk.data[12] == 1;
    const bool compression = chunk.data[10] == 0;
    const bool filter = chunk.data[11] == 0;
    return png.width != 0 && png.height != 0 && depthAllowed(png.colorType, png.depth)
           && compression && filter && (chunk.data[12] == 0 || chunk.data[12] == 1)
           && uint64_t(png.width) * uint64_t(png.height) <= kMaxPixels;
}

// Reads the physical pixel dimensions chunk, which records how many pixels fill one unit of length. The meter is the
// only unit the format defines (png 11.3.6.2).
void readPhys(const Chunk& chunk, ImageMetadata* out) {
    if (chunk.size < 9 || chunk.data[8] != 1) {
        return;
    }
    out->pixelsPerMeterX = float(readBeU32(chunk.data));
    out->pixelsPerMeterY = float(readBeU32(chunk.data + 4));
}

// Walks the chunk sequence, collecting the header, the palette, the transparency and every run of pixel data. Returns
// false for a chunk structure that runs past the bytes or has no header (png 5.3).
bool readChunks(Png& png, Array<Chunk>& data, const uint8_t* bytes, size_t size) {
    size_t at = sizeof(kSignature);
    bool header = false;
    while (at + 8 <= size) {
        const uint32_t length = readBeU32(bytes + at);
        const uint8_t* type = bytes + at + 4;
        if (length > 0x7FFFFFFFu || size_t(length) + 12 > size - at) {
            return false;
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
            break;
        }
    }
    return header && data.count != 0;
}

// Returns the number of bytes in a row of the given width, excluding its filter byte (png 7.2).
uint64_t rowBytesOf(const Png& png, uint64_t width) {
    return (width * channelsOf(png.colorType) * png.depth + 7) / 8;
}

// Writes the width and height that one interlace pass covers. Either is zero for a pass that the image is too small to
// contain (png 8.2).
void passExtentOf(const Png& png, uint32_t pass, uint32_t* outWidth, uint32_t* outHeight) {
    const uint32_t originX = kPassOriginX[pass];
    const uint32_t originY = kPassOriginY[pass];
    *outWidth =
        png.width > originX ? (png.width - originX + kPassStepX[pass] - 1) / kPassStepX[pass] : 0;
    *outHeight =
        png.height > originY ? (png.height - originY + kPassStepY[pass] - 1) / kPassStepY[pass] : 0;
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
    const int32_t distanceLeft =
        estimate > int32_t(left) ? estimate - int32_t(left) : int32_t(left) - estimate;
    const int32_t distanceAbove =
        estimate > int32_t(above) ? estimate - int32_t(above) : int32_t(above) - estimate;
    const int32_t distanceAboveLeft = estimate > int32_t(aboveLeft) ? estimate - int32_t(aboveLeft)
                                                                    : int32_t(aboveLeft) - estimate;
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
        return readBeU16(row + size_t(index) * 2);
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
        if (samples[channel] != readBeU16(png.transparency + size_t(channel) * 2)) {
            return false;
        }
    }
    return true;
}

// Writes the pixels of one unfiltered row into the destination, at the row and column stride at which the pass places
// them. Returns false for a palette index outside the palette.
bool emitRow(const Png& png, const uint8_t* row, uint32_t count, uint32_t originX, uint32_t stepX,
             uint8_t* destination) {
    const uint32_t channels = channelsOf(png.colorType);
    const uint32_t outChannels = outputChannelsOf(png);
    const uint32_t sampleBytes = outputSampleBytesOf(png);
    // Eight-bit samples that gain no alpha are copied as the bytes of the row.
    if (png.depth == 8 && png.colorType != PngColorType_Indexed && outChannels == channels) {
        uint8_t* out = destination + size_t(originX) * channels;
        if (stepX == 1) {
            memCopy(out, row, size_t(count) * channels);
            return true;
        }
        for (uint32_t index = 0; index < count; ++index)
            memCopy(out + size_t(index) * stepX * channels, row + size_t(index) * channels, channels);
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

// Unfilters the rows of the pass and places their pixels, walking the filtered bytes from `at`.
bool decodePass(const Png& png, uint8_t* filtered, size_t size, size_t* at, uint32_t count,
                uint32_t rows, uint32_t originX, uint32_t originY, uint32_t stepX, uint32_t stepY,
                uint8_t* pixels) {
    const size_t rowBytes = size_t(rowBytesOf(png, count));
    const size_t pixelBytes = channelsOf(png.colorType) * png.depth / 8;
    const uint8_t* above = nullptr;
    for (uint32_t row = 0; row < rows; ++row) {
        if (*at + rowBytes + 1 > size) {
            return false;
        }
        uint8_t* line = filtered + *at + 1;
        if (!unfilterRow(filtered[*at], line, above, rowBytes, pixelBytes > 0 ? pixelBytes : 1)) {
            return false;
        }
        *at += rowBytes + 1;
        above = line;
        uint8_t* destination = pixels
                               + size_t(originY + row * stepY) * size_t(png.width)
                                     * outputChannelsOf(png) * outputSampleBytesOf(png);
        if (!emitRow(png, line, count, originX, stepX, destination)) {
            return false;
        }
    }
    return true;
}

// Places every pixel in the filtered bytes: in one pass for a non-interlaced image, and in seven for an interlaced one.
bool decodePixels(const Png& png, uint8_t* filtered, size_t size, uint8_t* pixels) {
    size_t at = 0;
    if (!png.interlaced) {
        return decodePass(png, filtered, size, &at, png.width, png.height, 0, 0, 1, 1, pixels);
    }
    for (uint32_t pass = 0; pass < kPassCount; ++pass) {
        uint32_t count = 0;
        uint32_t rows = 0;
        passExtentOf(png, pass, &count, &rows);
        if (count == 0 || rows == 0) {
            continue;
        }
        if (!decodePass(png, filtered, size, &at, count, rows, kPassOriginX[pass],
                        kPassOriginY[pass], kPassStepX[pass], kPassStepY[pass], pixels)) {
            return false;
        }
    }
    return true;
}

// ImageMagick raw profiles store a description, a decimal byte count, and hex data. Decoding in place keeps the
// expanded text and the binary metadata in one allocation.
void readRawExif(uint8_t* text, size_t size, ImageMetadata* out) {
    if (size == 0 || text[0] != '\n')
        return;
    size_t at = 1;
    while (at < size && text[at] != '\n')
        ++at;
    while (at < size && asciiIsSpace(char(text[at])))
        ++at;
    size_t count = 0;
    const size_t start = at;
    while (at < size && asciiIsDigit(char(text[at]))) {
        const uint32_t digit = text[at++] - '0';
        if (count > size / 10 || count * 10 + digit > size)
            return;
        count = count * 10 + digit;
    }
    if (at == start || at == size || !asciiIsSpace(char(text[at])) || count < 6)
        return;
    for (size_t index = 0; index < count; ++index) {
        while (at < size && asciiIsSpace(char(text[at])))
            ++at;
        if (size - at < 2 || !asciiIsHexDigit(char(text[at]))
            || !asciiIsHexDigit(char(text[at + 1])))
            return;
        text[index] =
            uint8_t(asciiHexValue(char(text[at])) * 16 + asciiHexValue(char(text[at + 1])));
        at += 2;
    }
    while (at < size && asciiIsSpace(char(text[at])))
        ++at;
    constexpr uint8_t kExifPrefix[] = {'E', 'x', 'i', 'f', 0, 0};
    if (at == size && memEquals(text, kExifPrefix, sizeof(kExifPrefix)))
        imageReadExif(text + sizeof(kExifPrefix), count - sizeof(kExifPrefix), out);
}

PrStatus readProfile(PrCompressCodec* codec, const Chunk& profile, bool compressed,
                     size_t textLimit, ImageMetadata* out) {
    // The PNG working-storage estimate reserves 2 MiB for codec scratch.
    const size_t limit = textLimit < kPngProfileTextMaxBytes ? textLimit : kPngProfileTextMaxBytes;
    const uint8_t* source = profile.data;
    size_t size = profile.size;
    if (compressed && (size == 0 || source[0] != 0))
        return PrStatus_Ok;
    if (!compressed && size > limit)
        return PrStatus_Limit;
    uint8_t* text = static_cast<uint8_t*>(prMemoryAlloc(compressed ? limit + 1 : size));
    size_t written = size;
    bool valid = true;
    if (compressed) {
        PrCompressRead read{};
        PrStatus status =
            prCompressCodecDecode(codec, PrCompressFormat_Zlib, PrByteView{source + 1, size - 1},
                                  text, limit + 1, &read);
        written = read.written;
        valid = status == PrStatus_Ok && read.progress != PrCompressProgress_Input;
    } else {
        memCopy(text, source, size);
    }
    if (valid && written <= limit)
        readRawExif(text, written, out);
    prMemoryFree(text);
    return written > limit ? PrStatus_Limit : PrStatus_Ok;
}

// Decodes the scanlines in the zlib stream of the chunks, which must hold exactly the filtered bytes of the image, and
// unfilters them into pixels (png 5.6).
bool decodeChunks(const Png& png, PrCompressCodec* codec, const Chunk* chunks,
                  uint32_t chunkCount, uint8_t* pixels) {
    PrCompressDecoder* decoder = nullptr;
    if (chunkCount == 0
        || prCompressDecoderCreate(codec, PrCompressFormat_Zlib, &decoder) != PrStatus_Ok)
        return false;
    size_t filteredSize = size_t(filteredSizeOf(png));
    uint8_t* filtered = static_cast<uint8_t*>(prMemoryAlloc(filteredSize));
    size_t written = 0;
    bool done = false;
    bool ok = true;
    for (uint32_t i = 0; i < chunkCount && ok && !done; ++i) {
        PrByteView input{chunks[i].data, chunks[i].size};
        bool final = i + 1 == chunkCount;
        PrCompressRead read{};
        ok = prCompressDecoderRead(decoder, input, final, filtered + written,
                                   filteredSize - written, &read)
                 == PrStatus_Ok
             && read.progress != PrCompressProgress_Output;
        written += read.written;
        done = read.progress == PrCompressProgress_Done;
    }
    prCompressDecoderDestroy(decoder);
    ok = ok && done && written == filteredSize && decodePixels(png, filtered, filteredSize, pixels);
    prMemoryFree(filtered);
    return ok;
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
    writer.crc = crc32Update(writer.crc, bytes);
    if (writer.size < writer.capacity) {
        const size_t available = writer.capacity - writer.size;
        const size_t count = bytes.size < available ? bytes.size : available;
        memCopy(writer.out + writer.size, bytes.data, count);
    }
    writer.size += bytes.size;
}

void writeByte(Writer& writer, uint8_t value) {
    writeBytes(writer, {&value, 1});
}

void writeBeU32(Writer& writer, uint32_t value) {
    const uint8_t bytes[] = {uint8_t(value >> 24), uint8_t(value >> 16), uint8_t(value >> 8),
                             uint8_t(value)};
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
        const uint32_t block =
            remaining > kStoredBlockMaxBytes ? uint32_t(kStoredBlockMaxBytes) : uint32_t(remaining);
        writeByte(writer, remaining == block ? 1 : 0);
        writeByte(writer, uint8_t(block));
        writeByte(writer, uint8_t(block >> 8));
        writeByte(writer, uint8_t(~block));
        writeByte(writer, uint8_t((~block) >> 8));
        writeBytes(writer, PrByteView{bytes.data + written, block});
        written += block;
    } while (written < bytes.size);
    writeBeU32(writer, adler32Update(1, bytes));
}

// Writes each row of `rgba8` to `out` as its filter type and its filtered bytes (png 7.3, 9.2). Each row takes the
// filter whose bytes, as signed values, sum smallest in magnitude, which png 12.7 recommends for truecolor.
void filterRows(PrPixelView rgba8, uint8_t* out) {
    constexpr uint32_t kPixelBytes = 4;
    constexpr uint32_t kFilters = 5;
    const size_t rowBytes = size_t(rgba8.extent.x) * kPixelBytes;
    uint8_t* candidates = static_cast<uint8_t*>(prMemoryAlloc(kFilters * rowBytes + rowBytes));
    uint8_t* zeros = candidates + kFilters * rowBytes;
    memZero(zeros, rowBytes);
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
            memCopy(target + 1, candidates + chosen * rowBytes, rowBytes);
    }
    prMemoryFree(candidates);
}

struct PngAnimationInfo {
    uint32_t frames;
    uint32_t plays;
    bool cover;
};

// Validates the entire animation before allocating its frame records or canvases.
PrStatus scanAnimation(const uint8_t* bytes, size_t size, const PrImageLimits& limits,
                       uint32_t width, uint32_t height, PngAnimationInfo& info,
                       PngAnimation* animation) {
    uint32_t frames = 0;
    uint32_t sequence = 0;
    bool imageData = false;
    bool frameData = false;
    bool defaultFrame = false;
    uint64_t work = size;
    for (size_t at = 33; at + 12 <= size;) {
        const uint32_t length = readBeU32(bytes + at);
        if (length > 0x7FFFFFFFu || size_t(length) + 12 > size - at)
            return info.frames == 0 ? PrStatus_Ok : PrStatus_InvalidData;
        const uint8_t* type = bytes + at + 4;
        const uint8_t* data = bytes + at + 8;
        at += size_t(length) + 12;
        if (isType(type, "acTL")) {
            if (length != 8 || info.frames != 0 || imageData)
                return PrStatus_InvalidData;
            info.frames = readBeU32(data);
            info.plays = readBeU32(data + 4);
            if (info.frames == 0)
                return PrStatus_InvalidData;
            if (info.frames > limits.frames)
                return PrStatus_Limit;
        } else if (isType(type, "fcTL") && info.frames != 0) {
            if (length != 26 || readBeU32(data) != sequence++ || (frames != 0 && !frameData)
                || frames >= info.frames)
                return PrStatus_InvalidData;
            PngFrame frame = {};
            frame.width = readBeU32(data + 4);
            frame.height = readBeU32(data + 8);
            frame.x = readBeU32(data + 12);
            frame.y = readBeU32(data + 16);
            const uint32_t denominator = readBeU16(data + 22) == 0 ? 100 : readBeU16(data + 22);
            frame.duration =
                (uint32_t(readBeU16(data + 20)) * 1000 + denominator / 2) / denominator;
            frame.disposal = data[24];
            frame.blend = data[25];
            if (frame.width == 0 || frame.height == 0 || frame.width > width
                || frame.height > height || frame.x > width - frame.width
                || frame.y > height - frame.height || frame.disposal > PngDisposal_Previous
                || frame.blend > PngBlend_Over)
                return PrStatus_InvalidData;
            if (!imageData) {
                if (frames != 0 || frame.width != width || frame.height != height || frame.x != 0
                    || frame.y != 0)
                    return PrStatus_InvalidData;
                defaultFrame = true;
            }
            const uint64_t cost =
                uint64_t(width) * height * 8 + uint64_t(frame.width) * frame.height * 16;
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
            if (length < 4 || !imageData || frames == 0 || (defaultFrame && frames == 1)
                || readBeU32(data) != sequence++)
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
    return info.frames == 0 ? PrStatus_Ok : PrStatus_InvalidData;
}

template <typename Sample>
void composePngFrame(PngAnimation& animation, const PngFrame& frame) {
    const uint32_t channels = outputChannelsOf(animation.png);
    const uint32_t colors = channels - 1;
    const uint64_t maximum = sizeof(Sample) == 1 ? 255 : 65535;
    for (uint32_t y = 0; y < frame.height; ++y) {
        for (uint32_t x = 0; x < frame.width; ++x) {
            const Sample* source = reinterpret_cast<const Sample*>(animation.framePixels)
                                   + (size_t(y) * frame.width + x) * channels;
            Sample* destination =
                reinterpret_cast<Sample*>(animation.pixels)
                + (size_t(y + frame.y) * animation.png.width + x + frame.x) * channels;
            if (frame.blend == PngBlend_Source) {
                memCopy(destination, source, channels * sizeof(Sample));
                continue;
            }
            const uint64_t alpha = source[colors];
            const uint64_t remainder = destination[colors] * (maximum - alpha);
            const uint64_t combined = alpha * maximum + remainder;
            for (uint32_t color = 0; color < colors; ++color) {
                destination[color] = combined == 0
                                         ? 0
                                         : Sample((source[color] * alpha * maximum
                                                   + destination[color] * remainder + combined / 2)
                                                  / combined);
            }
            destination[colors] = Sample((combined + maximum / 2) / maximum);
        }
    }
}

// Returns the space of the code points of Rec. ITU-T H.273: colour primaries, transfer characteristics, matrix
// coefficients and the full-range flag. Only RGB at full range names a space.
PrColorSpace cicpSpace(uint8_t primaries, uint8_t transfer, uint8_t matrix, uint8_t fullRange) {
    if (matrix != 0 || fullRange != 1)
        return PrColorSpace_Count;
    // Primaries: 1 is BT.709 and sRGB, 9 is BT.2020 and BT.2100, 12 is Display P3. Transfer: 13 is sRGB's, 8 is linear,
    // 16 is PQ, 18 is HLG, and 1, 6, 14 and 15 are BT.709's and BT.2020's.
    bool rec709Curve = transfer == 1 || transfer == 6 || transfer == 14 || transfer == 15;
    switch (primaries) {
        case 1:
            return transfer == 13 ? PrColorSpace_Srgb : transfer == 8 ? PrColorSpace_SrgbLinear : PrColorSpace_Count;
        case 12:
            return transfer == 13 ? PrColorSpace_DisplayP3
                   : transfer == 8 ? PrColorSpace_DisplayP3Linear
                                   : PrColorSpace_Count;
        case 9:
            return transfer == 16  ? PrColorSpace_Rec2100Pq
                   : transfer == 18 ? PrColorSpace_Rec2100Hlg
                   : transfer == 8  ? PrColorSpace_Rec2100Linear
                   : rec709Curve    ? PrColorSpace_Rec2020
                                    : PrColorSpace_Count;
        default:
            return PrColorSpace_Count;
    }
}

// Returns the space that the cicp tag of the ICC profile names, or PrColorSpace_Count.
PrColorSpace profileCicpSpace(const uint8_t* profile, size_t size) {
    if (size < 132)
        return PrColorSpace_Count;
    uint32_t tags = readBeU32(profile + 128);
    for (uint32_t i = 0; i < tags && 132 + size_t(i + 1) * 12 <= size; ++i) {
        const uint8_t* entry = profile + 132 + size_t(i) * 12;
        uint32_t offset = readBeU32(entry + 4);
        uint32_t length = readBeU32(entry + 8);
        if (!isType(entry, "cicp") || length < 12 || offset > size || size - offset < 12)
            continue;
        const uint8_t* tag = profile + offset;
        if (!isType(tag, "cicp"))
            return PrColorSpace_Count;
        return cicpSpace(tag[8], tag[9], tag[10], tag[11]);
    }
    return PrColorSpace_Count;
}

} // namespace

PrStatus pngMetadata(PrCompressCodec* codec, PrByteView bytes, size_t textLimit,
                     ImageMetadata* out) {
    size_t at = sizeof(kSignature);
    if (bytes.size < at) {
        return PrStatus_Ok;
    }
    Chunk profile = {};
    bool compressed = false;
    bool hasExif = false;
    while (at + 12 <= bytes.size) {
        const uint32_t length = readBeU32(bytes.data + at);
        const uint8_t* type = bytes.data + at + 4;
        if (length > 0x7FFFFFFFu || size_t(length) + 12 > bytes.size - at) {
            break;
        }
        const Chunk chunk = {bytes.data + at + 8, length};
        at += size_t(length) + 12;
        // PNG 5.6 table 7: eXIf and pHYs precede the first IDAT.
        if (isType(type, "IDAT") || isType(type, "IEND")) {
            break;
        } else if (isType(type, "pHYs")) {
            readPhys(chunk, out);
        } else if (isType(type, "eXIf")) {
            hasExif = true;
            imageReadExif(chunk.data, chunk.size, out);
        } else if (profile.data == nullptr && (isType(type, "tEXt") || isType(type, "zTXt"))) {
            constexpr char kKeyword[] = "Raw profile type exif";
            if (chunk.size > sizeof(kKeyword)
                && memEquals(chunk.data, kKeyword, sizeof(kKeyword))) {
                profile = {chunk.data + sizeof(kKeyword), uint32_t(chunk.size - sizeof(kKeyword))};
                compressed = isType(type, "zTXt");
            }
        }
    }
    if (hasExif || profile.data == nullptr)
        return PrStatus_Ok;
    ImageMetadata fallback = *out;
    const PrStatus status = readProfile(codec, profile, compressed, textLimit, &fallback);
    out->orientation = fallback.orientation;
    if (out->pixelsPerMeterX == 0)
        out->pixelsPerMeterX = fallback.pixelsPerMeterX;
    if (out->pixelsPerMeterY == 0)
        out->pixelsPerMeterY = fallback.pixelsPerMeterY;
    return status;
}

PrColorSpace pngCicpSpace(PrCompressCodec* codec, PrByteView bytes) {
    size_t at = sizeof(kSignature);
    while (at + 12 <= bytes.size) {
        const uint32_t length = readBeU32(bytes.data + at);
        const uint8_t* type = bytes.data + at + 4;
        if (length > 0x7FFFFFFFu || size_t(length) + 12 > bytes.size - at)
            break;
        const Chunk chunk = {bytes.data + at + 8, length};
        at += size_t(length) + 12;
        if (isType(type, "IDAT") || isType(type, "IEND"))
            break;
        if (isType(type, "cICP") && chunk.size == 4)
            return cicpSpace(chunk.data[0], chunk.data[1], chunk.data[2], chunk.data[3]);
    }
    uint8_t* profile = nullptr;
    size_t size = 0;
    if (!pngIccProfile(codec, bytes, kPngProfileTextMaxBytes, &profile, &size))
        return PrColorSpace_Count;
    PrColorSpace space = profileCicpSpace(profile, size);
    prMemoryFree(profile);
    return space;
}

bool pngIccProfile(PrCompressCodec* codec, PrByteView bytes, size_t limit, uint8_t* PR_NULLABLE* PR_NONNULL out,
                   size_t* size) {
    *out = nullptr;
    *size = 0;
    size_t at = sizeof(kSignature);
    Chunk profile = {};
    while (at + 12 <= bytes.size) {
        const uint32_t length = readBeU32(bytes.data + at);
        const uint8_t* type = bytes.data + at + 4;
        if (length > 0x7FFFFFFFu || size_t(length) + 12 > bytes.size - at)
            break;
        const Chunk chunk = {bytes.data + at + 8, length};
        at += size_t(length) + 12;
        if (isType(type, "IDAT") || isType(type, "IEND"))
            break;
        if (isType(type, "iCCP"))
            profile = chunk;
    }
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
    PrStatus status = prCompressCodecDecode(codec, PrCompressFormat_Zlib,
                                            PrByteView{profile.data + name + 2, profile.size - name - 2}, expanded,
                                            limit, &read);
    if (status != PrStatus_Ok) {
        prMemoryFree(expanded);
        return false;
    }
    *out = expanded;
    *size = read.written;
    return true;
}

bool pngDecode(PrCompressCodec* codec, PrByteView bytes, uint8_t* pixels, size_t pixelsSize) {
    if (bytes.size < sizeof(kSignature)) {
        return false;
    }
    for (size_t index = 0; index < sizeof(kSignature); ++index) {
        if (bytes.data[index] != kSignature[index]) {
            return false;
        }
    }
    Png png = {};
    Array<Chunk> data;
    bool ok = readChunks(png, data, bytes.data, bytes.size);
    ok = ok
         && uint64_t(png.width) * uint64_t(png.height) * outputChannelsOf(png)
                    * outputSampleBytesOf(png)
                == uint64_t(pixelsSize);
    ok = ok && (png.colorType != PngColorType_Indexed || png.paletteCount != 0);
    ok = ok && decodeChunks(png, codec, data.data, data.count, pixels);
    data.reset();
    return ok;
}

PrStatus pngAnimationOpen(PrCompressCodec* codec, PrByteView bytes,
                          const PrImageLimits& limits, PrImageInfo& info, PngAnimation** out) {
    *out = nullptr;
    PngAnimationInfo scanned = {};
    const PrStatus status =
        scanAnimation(bytes.data, bytes.size, limits, info.extent.x, info.extent.y, scanned, nullptr);
    if (status != PrStatus_Ok || scanned.frames == 0)
        return status;
    const uint64_t extra = uint64_t(info.extent.x) * info.extent.y * 32 + uint64_t(scanned.frames) * 256
                           + sizeof(PngAnimation);
    if (info.workingBytes > limits.workingBytes || extra > limits.workingBytes - info.workingBytes)
        return PrStatus_Limit;
    info.workingBytes += extra;
    info.frameCount = scanned.frames;
    info.playCount = scanned.plays;
    info.hasCover = scanned.cover;
    info.format.alpha = PrImageAlpha_Straight;
    const uint64_t pixelsBytes = uint64_t(info.extent.x) * info.extent.y * prImageFormatSize(&info.format);
    if (pixelsBytes == 0 || pixelsBytes > limits.outputBytes)
        return PrStatus_Limit;
    const size_t pixelsSize = size_t(pixelsBytes);
    PngAnimation* animation = allocZeroed<PngAnimation>();
    animation->codec = codec;
    animation->at = kNoFrame;
    animation->pixelsSize = pixelsSize;
    scanned = {};
    const bool parsed = readChunks(animation->png, animation->cover, bytes.data, bytes.size)
                        && scanAnimation(bytes.data, bytes.size, limits, info.extent.x, info.extent.y,
                                         scanned, animation)
                               == PrStatus_Ok;
    if (!parsed) {
        pngAnimationClose(animation);
        return PrStatus_InvalidData;
    }
    if (!info.hasCover)
        animation->cover.reset();
    animation->png.forceAlpha = true;
    animation->pixels = static_cast<uint8_t*>(prMemoryAlloc(pixelsSize));
    animation->framePixels = static_cast<uint8_t*>(prMemoryAlloc(pixelsSize));
    *out = animation;
    return PrStatus_Ok;
}

void pngAnimationClose(PngAnimation* animation) {
    if (animation != nullptr) {
        animation->frames.reset();
        animation->chunks.reset();
        animation->cover.reset();
        prMemoryFree(animation->pixels);
        prMemoryFree(animation->previous);
        prMemoryFree(animation->framePixels);
        prMemoryFree(animation);
    }
}

uint32_t pngAnimationDelay(const PngAnimation& animation, uint32_t frame) {
    return frame < animation.frames.count ? animation.frames.data[frame].duration : 0;
}

bool pngAnimationFrame(PngAnimation& animation, uint32_t frame, uint8_t* pixels,
                       size_t pixelsSize) {
    if (pixelsSize != animation.pixelsSize)
        return false;
    if (frame == kPrImageFrameCover && animation.cover.count != 0) {
        return decodeChunks(animation.png, animation.codec, animation.cover.data,
                            animation.cover.count, pixels);
    }
    if (frame >= animation.frames.count)
        return false;
    if (animation.at == kNoFrame || frame < animation.at) {
        memZero(animation.pixels, pixelsSize);
        animation.at = kNoFrame;
    }
    const uint32_t pixelBytes =
        outputChannelsOf(animation.png) * outputSampleBytesOf(animation.png);
    while (animation.at != frame) {
        const uint32_t next = animation.at == kNoFrame ? 0 : animation.at + 1;
        if (animation.at != kNoFrame) {
            const PngFrame& previous = animation.frames.data[animation.at];
            if (previous.disposal == PngDisposal_Background) {
                for (uint32_t y = 0; y < previous.height; ++y)
                    memZero(animation.pixels
                                  + (size_t(y + previous.y) * animation.png.width + previous.x)
                                        * pixelBytes,
                            size_t(previous.width) * pixelBytes);
            } else if (previous.disposal == PngDisposal_Previous) {
                memCopy(animation.pixels, animation.previous, pixelsSize);
            }
        }
        const PngFrame& current = animation.frames.data[next];
        if (current.disposal == PngDisposal_Previous) {
            if (animation.previous == nullptr)
                animation.previous = static_cast<uint8_t*>(prMemoryAlloc(pixelsSize));
            memCopy(animation.previous, animation.pixels, pixelsSize);
        }
        Png png = animation.png;
        png.width = current.width;
        png.height = current.height;
        if (!decodeChunks(png, animation.codec, animation.chunks.data + current.dataStart,
                          current.dataCount, animation.framePixels)) {
            animation.at = kNoFrame;
            return false;
        }
        if (outputSampleBytesOf(png) == 2)
            composePngFrame<uint16_t>(animation, current);
        else
            composePngFrame<uint8_t>(animation, current);
        animation.at = next;
    }
    memCopy(pixels, animation.pixels, pixelsSize);
    return true;
}

size_t pngEncode(PrPixelView rgba8, PrCompressCodec* codec, uint8_t* PR_NULLABLE out, size_t capacity) {
    const uint32_t width = rgba8.extent.x;
    const uint32_t height = rgba8.extent.y;
    Writer writer = {out, out != nullptr ? capacity : 0, 0, 0};

    const uint8_t signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    for (size_t index = 0; index < countOf(signature); ++index) {
        writeByte(writer, signature[index]);
    }

    // png 11.2.2: the dimensions, then 8 bits per sample, color type 6 for truecolor with alpha, and the single
    // compression, filter and interlace method the format defines.
    beginChunk(writer, 13, "IHDR");
    writeBeU32(writer, width);
    writeBeU32(writer, height);
    writeByte(writer, 8);
    writeByte(writer, 6);
    writeByte(writer, 0);
    writeByte(writer, 0);
    writeByte(writer, 0);
    endChunk(writer);

    const size_t rowSize = size_t(width) * 4 + 1;
    const size_t filteredSize = size_t(height) * rowSize;
    uint8_t* filtered = static_cast<uint8_t*>(prMemoryAlloc(filteredSize));
    filterRows(rgba8, filtered);
    const PrByteView rows{filtered, filteredSize};

    // The rows go in one zlib stream, compressed by the codec, or stored if the codec encodes nothing.
    const size_t bound = prCompressCodecEncodeBound(codec, PrCompressFormat_Zlib, filteredSize);
    uint8_t* stream = bound != 0 ? static_cast<uint8_t*>(prMemoryAlloc(bound)) : nullptr;
    size_t streamSize = 0;
    if (stream != nullptr
        && prCompressCodecEncode(codec, PrCompressFormat_Zlib, PrCompressLevel_Default, rows, stream, &streamSize)
               == PrStatus_Ok) {
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

} // namespace pr_impl
