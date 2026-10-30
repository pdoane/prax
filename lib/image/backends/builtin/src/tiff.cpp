// Built-in TIFF codec internals.
//
// TIFF 6.0 - https://www.itu.int/itudoc/itu-t/com16/tiff-fx/docs/tiff6.pdf

#include "image/backends/builtin/src/tiff.h"

#include "pr/mem.h"

namespace pr_impl {

namespace {

enum {
    TiffTag_ImageWidth = 256,
    TiffTag_ImageLength = 257,
    TiffTag_BitsPerSample = 258,
    TiffTag_Compression = 259,
    TiffTag_Photometric = 262,
    TiffTag_StripOffsets = 273,
    TiffTag_SamplesPerPixel = 277,
    TiffTag_RowsPerStrip = 278,
    TiffTag_StripByteCounts = 279,
    TiffTag_PlanarConfiguration = 284,
    TiffTag_ExtraSamples = 338,
};
typedef uint16_t TiffTag;

enum {
    TiffType_Short = 3,
    TiffType_Long = 4,
};
typedef uint16_t TiffType;

constexpr uint32_t kEntrySize = 12;

uint16_t read16(const TiffImage& image, uint32_t at) {
    if (image.bytes.data == nullptr || at > image.bytes.size || image.bytes.size - at < 2)
        return 0;
    return image.bigEndian ? readBeU16(image.bytes.data + at) : readLeU16(image.bytes.data + at);
}

uint32_t read32(const TiffImage& image, uint32_t at) {
    if (image.bytes.data == nullptr || at > image.bytes.size || image.bytes.size - at < 4)
        return 0;
    return image.bigEndian ? readBeU32(image.bytes.data + at) : readLeU32(image.bytes.data + at);
}

uint32_t typeSize(TiffType type) {
    return type == TiffType_Short ? 2 : type == TiffType_Long ? 4 : 0;
}

// Returns the location of the values of an entry: inside the entry if they fit in four bytes, and otherwise at the
// offset that the entry holds. Returns zero for a type that this codec does not read.
uint32_t valuesAt(const TiffImage& image, uint32_t entry, TiffType type, uint32_t count) {
    uint32_t size = typeSize(type);
    if (size == 0 || count > 0xffffffffu / size)
        return 0;
    return size * count <= 4 ? entry + 8 : read32(image, entry + 8);
}

// Returns the value at `index` among the values of `type` at `at`.
uint32_t valueAt(const TiffImage& image, uint32_t at, TiffType type, uint32_t index) {
    return type == TiffType_Short ? read16(image, at + index * 2) : read32(image, at + index * 4);
}

} // namespace

bool tiffHasSignature(PrByteView bytes) {
    if (bytes.data == nullptr || bytes.size < 4)
        return false;
    const uint8_t* b = bytes.data;
    bool little = b[0] == 'I' && b[1] == 'I' && b[2] == 42 && b[3] == 0;
    bool big = b[0] == 'M' && b[1] == 'M' && b[2] == 0 && b[3] == 42;
    return little || big;
}

PrStatus tiffImage(PrByteView bytes, TiffImage* out) {
    *out = TiffImage{};
    if (!tiffHasSignature(bytes) || bytes.size < 8)
        return PrStatus_InvalidData;
    TiffImage& image = *out;
    image.bytes = bytes;
    image.bigEndian = bytes.data[0] == 'M';
    image.samples = 1;
    image.rowsPerStrip = 0xffffffffu;
    uint32_t ifd = read32(image, 4);
    uint32_t entries = read16(image, ifd);
    if (ifd < 8 || ifd > bytes.size || (bytes.size - ifd - 2) / kEntrySize < entries)
        return PrStatus_InvalidData;
    uint32_t photometric = 0xffffffffu;
    uint32_t extra = 0;
    uint32_t extraKind = 0;
    bool offsets = false;
    bool counts = false;
    uint32_t bitsAt = 0;
    uint32_t bitsCount = 0;
    TiffType bitsType = 0;
    for (uint32_t i = 0; i < entries; ++i) {
        uint32_t entry = ifd + 2 + i * kEntrySize;
        TiffTag tag = read16(image, entry);
        TiffType type = read16(image, entry + 2);
        uint32_t count = read32(image, entry + 4);
        uint32_t at = valuesAt(image, entry, type, count);
        uint32_t value = count != 0 && at != 0 ? valueAt(image, at, type, 0) : 0;
        switch (tag) {
            case TiffTag_ImageWidth:
                image.width = value;
                break;
            case TiffTag_ImageLength:
                image.height = value;
                break;
            case TiffTag_BitsPerSample:
                bitsAt = at;
                bitsCount = count;
                bitsType = type;
                break;
            case TiffTag_Compression:
                if (value != 1)
                    return PrStatus_NotSupported;
                break;
            case TiffTag_Photometric:
                photometric = value;
                break;
            case TiffTag_StripOffsets:
                image.stripOffsets = count == 1 ? value : at;
                image.offsetType = type;
                image.stripCount = count;
                offsets = at != 0;
                break;
            case TiffTag_SamplesPerPixel:
                image.samples = value;
                break;
            case TiffTag_RowsPerStrip:
                image.rowsPerStrip = value;
                break;
            case TiffTag_StripByteCounts:
                image.stripCounts = count == 1 ? value : at;
                image.countType = type;
                counts = at != 0 && count == image.stripCount;
                break;
            case TiffTag_PlanarConfiguration:
                if (value != 1)
                    return PrStatus_NotSupported;
                break;
            case TiffTag_ExtraSamples:
                extra = count;
                extraKind = value;
                break;
            default:
                break;
        }
    }
    if (image.width == 0 || image.height == 0 || image.rowsPerStrip == 0 || !offsets || !counts)
        return PrStatus_InvalidData;
    if (photometric > 2 || extra > 1)
        return PrStatus_NotSupported;
    image.gray = photometric != 2;
    image.whiteIsZero = photometric == 0;
    if (image.samples != (image.gray ? 1u : 3u) + extra)
        return PrStatus_NotSupported;
    if (bitsCount != 0 && bitsCount != image.samples)
        return PrStatus_InvalidData;
    for (uint32_t i = 0; i < bitsCount; ++i) {
        if (valueAt(image, bitsAt, bitsType, i) != 8)
            return PrStatus_NotSupported;
    }
    // A strip count of zero, or one that does not cover the rows, cannot hold the image.
    uint32_t strips = (image.height - 1) / image.rowsPerStrip + 1;
    if (image.stripCount < strips)
        return PrStatus_InvalidData;
    if (extra == 0)
        image.alpha = PrImageAlpha_None;
    else if (extraKind == 1)
        image.alpha = PrImageAlpha_Premultiplied;
    else if (extraKind == 2)
        image.alpha = PrImageAlpha_Straight;
    else
        image.alpha = PrImageAlpha_Independent;
    return PrStatus_Ok;
}

bool tiffDecode(const TiffImage& image, uint8_t* pixels, size_t pixelsSize) {
    size_t rowBytes = size_t(image.width) * image.samples;
    if (pixelsSize != rowBytes * image.height)
        return false;
    for (uint32_t row = 0, strip = 0; row < image.height; row += image.rowsPerStrip, ++strip) {
        uint32_t offset;
        if (image.stripCount == 1)
            offset = image.stripOffsets;
        else
            offset = valueAt(image, image.stripOffsets, image.offsetType, strip);
        uint32_t count = image.stripCount == 1 ? image.stripCounts : valueAt(image, image.stripCounts, image.countType, strip);
        uint32_t rows = image.height - row < image.rowsPerStrip ? image.height - row : image.rowsPerStrip;
        size_t size = rowBytes * rows;
        if (count < size || offset > image.bytes.size || image.bytes.size - offset < size)
            return false;
        memCopy(pixels + rowBytes * row, image.bytes.data + offset, size);
    }
    if (image.whiteIsZero) {
        for (size_t i = 0; i < pixelsSize; i += image.samples)
            pixels[i] = uint8_t(255 - pixels[i]);
    }
    return true;
}

} // namespace pr_impl
