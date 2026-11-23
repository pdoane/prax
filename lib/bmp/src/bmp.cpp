// BMP decoding.
//
// A DIB is an information header, bit fields for some versions of it, a color table, and the pixels. A BMP file puts a
// 14-byte file header before the DIB, which locates the pixels. All integers are little-endian.

#include "pr/bmp.h"

#include "pr/align.h"
#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/mem.h"
#include "pr/pixels.h"

namespace pr_impl {

namespace {

constexpr uint32_t kFileHeaderBytes = 14;

// The sizes of the information headers: OS/2 1.x and Windows 2.x's BITMAPCOREHEADER, BITMAPINFOHEADER, the two headers
// of Adobe that add bit fields to it, OS/2 2.x's, BITMAPV4HEADER and BITMAPV5HEADER.
constexpr uint32_t kCoreHeader = 12;
constexpr uint32_t kOs2ShortHeader = 16;
constexpr uint32_t kInfoHeader = 40;
constexpr uint32_t kV2Header = 52;
constexpr uint32_t kV3Header = 56;
constexpr uint32_t kOs2Header = 64;
constexpr uint32_t kV4Header = 108;
constexpr uint32_t kV5Header = 124;

// The compressions.
enum {
    Compression_Rgb = 0,
    Compression_Rle8 = 1,
    Compression_Rle4 = 2,
    Compression_Bitfields = 3,
    Compression_Jpeg = 4,
    Compression_Png = 5,
    Compression_AlphaBitfields = 6,
};
typedef uint32_t Compression;

// The color space types of a version 4 or 5 header, as their four characters read in a little-endian integer.
constexpr uint32_t kSpaceSrgb = 0x73524742;     // 'sRGB'
constexpr uint32_t kSpaceWindows = 0x57696e20;  // 'Win '
constexpr uint32_t kSpaceEmbedded = 0x4d424544; // 'MBED'

bool isHeaderSize(uint32_t size) {
    return size == kCoreHeader || size == kOs2ShortHeader || size == kInfoHeader || size == kV2Header ||
        size == kV3Header || size == kOs2Header || size == kV4Header || size == kV5Header;
}

// One bit field: the mask of a channel, the shift to its lowest bit, and its bits.
struct Field {
    uint32_t mask;
    uint32_t shift;
    uint32_t bits;
};

// Returns the field of `mask`. Returns false for a mask whose bits are not contiguous.
bool fieldOf(uint32_t mask, Field* out) {
    *out = Field{mask, 0, 0};
    if (mask == 0)
        return true;
    uint32_t shift = 0;
    while (((mask >> shift) & 1) == 0)
        ++shift;
    const uint32_t value = mask >> shift;
    if ((value & (value + 1)) != 0)
        return false;
    uint32_t bits = 0;
    while (value >> bits != 0)
        ++bits;
    *out = Field{mask, shift, bits};
    return true;
}

// Returns the 8-bit value of field `f` of the pixel `v`, widened or narrowed (prPixelChannel8). Microsoft's
// documentation of BITMAPINFOHEADER and BITMAPV5HEADER says that a pixel's fields are the relative intensities of its
// colors, and gives no conversion of a field of n bits to a wider value, so a field widens by replicating its bits and
// narrows to its high bits.
uint8_t valueOf(const Field& f, uint32_t v) {
    return f.bits == 0 ? 0 : prPixelChannel8((v & f.mask) >> f.shift, f.bits);
}

// A DIB, as its headers describe it.
struct Dib {
    PrByteView pixels;              // from the first stored row to the end of the bytes
    const uint8_t* PR_NULLABLE palette;
    uint32_t paletteCount;
    uint32_t entryBytes;            // of a color table entry: 3 in a core header, and 4 otherwise
    uint32_t width;
    uint32_t height;
    uint64_t rowBytes;
    uint16_t bits;
    Compression compression;
    bool topDown;
    bool icon;
    bool alphaField;                // true if the alpha of the pixels is a bit field
    Field fields[4];                // red, green, blue and alpha
};

// Writes the color of entry `index` of the color table to `out`, or black past the table.
void paletteColor(const Dib& dib, uint32_t index, uint8_t* out) {
    if (index >= dib.paletteCount) {
        out[0] = out[1] = out[2] = 0;
        return;
    }
    const uint8_t* entry = dib.palette + size_t(index) * dib.entryBytes;
    out[0] = entry[2];
    out[1] = entry[1];
    out[2] = entry[0];
}

// Decodes the stored row `row` into `out`, as `channels` channels a pixel. Returns true if any pixel's alpha is not
// zero.
bool decodeRow(const Dib& dib, const uint8_t* row, uint32_t channels, uint8_t* out) {
    bool anyAlpha = false;
    for (uint32_t x = 0; x < dib.width; ++x, out += channels) {
        if (dib.bits <= 8) {
            const uint32_t bit = x * dib.bits;
            const uint32_t index = (row[bit / 8] >> (8 - dib.bits - bit % 8)) & ((1u << dib.bits) - 1);
            paletteColor(dib, index, out);
            if (channels == 4)
                out[3] = 255;
            continue;
        }
        if (dib.bits == 24) {
            const uint8_t* p = row + size_t(x) * 3;
            out[0] = p[2];
            out[1] = p[1];
            out[2] = p[0];
            if (channels == 4)
                out[3] = 255;
            continue;
        }
        const uint32_t v = dib.bits == 16 ? prMemReadLeU16(row + size_t(x) * 2) : prMemReadLeU32(row + size_t(x) * 4);
        out[0] = valueOf(dib.fields[0], v);
        out[1] = valueOf(dib.fields[1], v);
        out[2] = valueOf(dib.fields[2], v);
        if (channels == 4) {
            out[3] = dib.alphaField ? valueOf(dib.fields[3], v) : 255;
            anyAlpha = anyAlpha || out[3] != 0;
        }
    }
    return anyAlpha;
}

// Returns the row of `output` that stored row `row` of `dib` decodes to.
uint8_t* outputRow(const Dib& dib, PrPixels output, uint32_t row) {
    const uint32_t y = dib.topDown ? row : dib.height - 1 - row;
    return output.data + size_t(y) * output.stride;
}

// Sets the alpha of every pixel of `output`: to 255, or from an icon's AND mask where it has one.
void applyMask(const Dib& dib, PrPixels output, bool useMask) {
    const uint64_t maskRow = ((uint64_t(dib.width) + 31) / 32) * 4;
    const uint64_t maskAt = dib.rowBytes * dib.height;
    const bool hasMask = useMask && dib.pixels.size >= maskAt && dib.pixels.size - maskAt >= maskRow * dib.height;
    for (uint32_t row = 0; row < dib.height; ++row) {
        uint8_t* out = outputRow(dib, output, row);
        const uint8_t* mask = hasMask ? dib.pixels.data + size_t(maskAt + row * maskRow) : nullptr;
        for (uint32_t x = 0; x < dib.width; ++x)
            out[size_t(x) * 4 + 3] = mask != nullptr && (mask[x / 8] & (0x80u >> (x % 8))) != 0 ? 0 : 255;
    }
}

// Decodes stored rows. Returns Truncated if the bytes end before the rows do, after which the rows that they do not
// hold are transparent black.
PrStatus decodeRows(const Dib& dib, uint32_t channels, PrPixels output) {
    bool anyAlpha = false;
    PrStatus status = PrStatus_Ok;
    for (uint32_t row = 0; row < dib.height; ++row) {
        uint8_t* out = outputRow(dib, output, row);
        const uint64_t at = row * dib.rowBytes;
        if (dib.pixels.size < at || dib.pixels.size - at < dib.rowBytes) {
            prMemZero(out, size_t(dib.width) * channels);
            status = PrStatus_Truncated;
            continue;
        }
        anyAlpha = decodeRow(dib, dib.pixels.data + size_t(at), channels, out) || anyAlpha;
    }
    if (status != PrStatus_Ok)
        return status;
    if (dib.icon && !(dib.alphaField && anyAlpha))
        applyMask(dib, output, true);
    else if (dib.alphaField && !anyAlpha)
        applyMask(dib, output, false);
    return PrStatus_Ok;
}

// Decodes run-length encoded rows, which are stored from the bottom. A pixel that the codes do not write stays
// transparent black. Returns Truncated if the bytes end before the end of the bitmap, and InvalidData for an absolute
// run or a delta past the bitmap.
PrStatus decodeRle(const Dib& dib, PrPixels output) {
    for (uint32_t y = 0; y < dib.height; ++y)
        prMemZero(output.data + size_t(y) * output.stride, size_t(dib.width) * 4);
    const PrByteView bytes = dib.pixels;
    const bool four = dib.compression == Compression_Rle4;
    uint32_t x = 0;
    uint32_t row = 0;
    size_t at = 0;
    auto put = [&](uint32_t index) {
        if (x < dib.width && row < dib.height) {
            uint8_t* out = outputRow(dib, output, row) + size_t(x) * 4;
            paletteColor(dib, index, out);
            out[3] = 255;
        }
        ++x;
    };
    while (row < dib.height) {
        if (bytes.size - at < 2)
            return PrStatus_Truncated;
        const uint32_t count = bytes.data[at];
        const uint32_t value = bytes.data[at + 1];
        at += 2;
        if (count != 0) {
            // An encoded run: `count` pixels of one index, or of two alternating nibbles.
            for (uint32_t i = 0; i < count; ++i)
                put(four ? (i % 2 == 0 ? value >> 4 : value & 15) : value);
            continue;
        }
        if (value == 0) {
            x = 0;
            ++row;
        } else if (value == 1) {
            return PrStatus_Ok;
        } else if (value == 2) {
            if (bytes.size - at < 2)
                return PrStatus_Truncated;
            x += bytes.data[at];
            row += bytes.data[at + 1];
            at += 2;
            if (x > dib.width || row > dib.height)
                return PrStatus_InvalidData;
        } else {
            // An absolute run of `value` indices, padded to a whole number of 16-bit words.
            const size_t length = four ? (size_t(value) + 1) / 2 : value;
            const size_t padded = alignUp(length, size_t(2));
            if (bytes.size - at < length)
                return PrStatus_Truncated;
            for (uint32_t i = 0; i < value; ++i) {
                const uint32_t byte = bytes.data[at + (four ? i / 2 : i)];
                put(four ? (i % 2 == 0 ? byte >> 4 : byte & 15) : byte);
            }
            at += bytes.size - at < padded ? bytes.size - at : padded;
        }
    }
    return PrStatus_Ok;
}

// Reads the headers of the DIB at `dib` of `size` bytes. `pixelsAt` is the offset of the pixels from the start of the
// DIB for a file, or zero for an icon, whose pixels follow its color table. Writes the color of the pixels to `info`.
PrStatus readDib(const uint8_t* at, size_t size, PrBmpSource source, uint64_t pixelsAt, Dib* out, PrBmpInfo* info) {
    Dib& dib = *out;
    dib.icon = source == PrBmpSource_Icon;
    if (size < 4)
        return PrStatus_InvalidData;
    const uint32_t headerSize = prMemReadLeU32(at);
    if (!isHeaderSize(headerSize) || headerSize > size)
        return PrStatus_InvalidData;
    int64_t width = 0;
    int64_t height = 0;
    dib.compression = Compression_Rgb;
    uint32_t used = 0;
    if (headerSize == kCoreHeader) {
        width = prMemReadLeU16(at + 4);
        height = prMemReadLeU16(at + 6);
        dib.bits = uint16_t(prMemReadLeU16(at + 10));
        dib.entryBytes = 3;
    } else {
        width = int32_t(prMemReadLeU32(at + 4));
        height = int32_t(prMemReadLeU32(at + 8));
        dib.bits = uint16_t(prMemReadLeU16(at + 14));
        dib.entryBytes = 4;
        // OS/2's short version 2 header ends after the bit count, and its other fields are zero.
        if (headerSize >= kInfoHeader) {
            dib.compression = prMemReadLeU32(at + 16);
            const int32_t perMeterX = int32_t(prMemReadLeU32(at + 24));
            const int32_t perMeterY = int32_t(prMemReadLeU32(at + 28));
            if (perMeterX > 0 && perMeterY > 0)
                info->pixelsPerMeter = PrVec2{float(perMeterX), float(perMeterY)};
            used = prMemReadLeU32(at + 32);
        }
    }
    if (dib.compression == Compression_Jpeg || dib.compression == Compression_Png)
        return PrStatus_NotSupported;
    // OS/2's header numbers its own compressions from 3: Huffman 1D and RLE24.
    if (headerSize == kOs2Header && dib.compression > Compression_Rle4)
        return PrStatus_NotSupported;
    if (height < 0) {
        dib.topDown = true;
        height = -height;
    }
    if (dib.icon) {
        if (height % 2 != 0 || dib.topDown)
            return PrStatus_InvalidData;
        height /= 2;
    }
    if (width <= 0 || height <= 0 || width > 0x7fffffff || height > 0x7fffffff)
        return PrStatus_InvalidData;
    dib.width = uint32_t(width);
    dib.height = uint32_t(height);
    const uint16_t bits = dib.bits;
    // Windows CE's 2-bit DIBs index a table of four entries. 64-bit DIBs hold fixed-point linear channels.
    if (bits == 64)
        return PrStatus_NotSupported;
    if (bits != 1 && bits != 2 && bits != 4 && bits != 8 && bits != 16 && bits != 24 && bits != 32)
        return PrStatus_InvalidData;
    const Compression c = dib.compression;
    const bool rle = c == Compression_Rle8 || c == Compression_Rle4;
    const bool bitfields = c == Compression_Bitfields || c == Compression_AlphaBitfields;
    if ((c == Compression_Rle8 && bits != 8) || (c == Compression_Rle4 && bits != 4) ||
            (bitfields && bits != 16 && bits != 32) || (!rle && !bitfields && c != Compression_Rgb) ||
            (rle && dib.topDown) || (dib.icon && (rle || c == Compression_AlphaBitfields)))
        return PrStatus_InvalidData;

    // The bit fields: in the header from version 2 on, and otherwise in three or four masks after it.
    size_t tableAt = headerSize;
    uint32_t masks[4] = {0, 0, 0, 0};
    if (bitfields) {
        const uint32_t count = c == Compression_AlphaBitfields || headerSize >= kV3Header ? 4 : 3;
        size_t masksAt = headerSize >= kV2Header ? kInfoHeader : headerSize;
        if (headerSize < kV2Header)
            tableAt += size_t(count) * 4;
        if (masksAt + size_t(count) * 4 > size)
            return PrStatus_InvalidData;
        for (uint32_t i = 0; i < count; ++i)
            masks[i] = prMemReadLeU32(at + masksAt + size_t(i) * 4);
    } else if (bits == 16) {
        masks[0] = 0x7c00;
        masks[1] = 0x03e0;
        masks[2] = 0x001f;
    } else if (bits == 32) {
        masks[0] = 0xff0000;
        masks[1] = 0xff00;
        masks[2] = 0xff;
        // An icon's 32-bit bitmap keeps alpha in the byte that a file reserves.
        masks[3] = dib.icon ? 0xff000000 : 0;
    }
    for (uint32_t i = 0; i < 4; ++i) {
        if (!fieldOf(masks[i], &dib.fields[i]) || (bits == 16 && masks[i] > 0xffff))
            return PrStatus_InvalidData;
    }
    dib.alphaField = masks[3] != 0;

    // The color table, of the entries that the header counts, or of every index. A file's pixels may begin before the
    // table ends, which leaves fewer entries.
    if (bits <= 8) {
        const uint32_t most = 1u << bits;
        dib.paletteCount = used == 0 || used > most ? most : used;
    }
    const size_t tableBytes = size_t(bits <= 8 ? dib.paletteCount : used) * dib.entryBytes;
    size_t pixels = 0;
    if (dib.icon) {
        if (tableAt + tableBytes > size)
            return PrStatus_InvalidData;
        pixels = tableAt + tableBytes;
    } else {
        if (pixelsAt < headerSize)
            return PrStatus_InvalidData;
        const size_t end = pixelsAt < size ? size_t(pixelsAt) : size;
        const size_t room = end > tableAt ? end - tableAt : 0;
        if (dib.paletteCount * dib.entryBytes > room)
            dib.paletteCount = uint32_t(room / dib.entryBytes);
        pixels = end;
    }
    dib.palette = at + tableAt;
    dib.pixels = PrByteView{at + pixels, size - pixels};
    dib.rowBytes = ((uint64_t(dib.width) * bits + 31) / 32) * 4;

    // The color space, of a version 4 or 5 header. An embedded profile's offset counts from the start of the header.
    if (headerSize >= kV4Header) {
        const uint32_t space = prMemReadLeU32(at + 56);
        if (space == kSpaceSrgb || space == kSpaceWindows) {
            info->color = PrBmpColor_Srgb;
        } else if (space == kSpaceEmbedded && headerSize >= kV5Header) {
            const uint64_t offset = prMemReadLeU32(at + 112);
            const uint64_t length = prMemReadLeU32(at + 116);
            if (length != 0 && offset <= size && length <= size - offset) {
                info->color = PrBmpColor_Profile;
                info->profile = PrByteView{at + offset, size_t(length)};
            }
        }
    }
    return PrStatus_Ok;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrBmpDecoder {
    Dib dib;
    PrBmpInfo info;
};

bool prBmpHasSignature(PrByteView bytes) {
    return bytes.size >= kPrBmpSignatureBytes && bytes.data[0] == 'B' && bytes.data[1] == 'M' &&
        prMemReadLeU32(bytes.data + 6) == 0 && isHeaderSize(prMemReadLeU32(bytes.data + kFileHeaderBytes));
}

PrBmpLimits prBmpLimitsDefault(void) {
    return PrBmpLimits{
        .pixels = 64u * 1024u * 1024u,
        .workingBytes = 512u * 1024u * 1024u,
        .outputBytes = 512u * 1024u * 1024u,
        .work = 1000000000u,
    };
}

PrStatus prBmpDecoderCreate(PrByteView bytes, PrBmpSource source, const PrBmpLimits* PR_NULLABLE limits, uint64_t held,
        PrBmpDecoder* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    const PrBmpLimits bounds = limits != nullptr ? *limits : prBmpLimitsDefault();
    const uint8_t* at = bytes.data;
    size_t size = bytes.size;
    uint64_t pixelsAt = 0;
    if (source == PrBmpSource_File) {
        if (!prBmpHasSignature(bytes))
            return PrStatus_NotSupported;
        const uint64_t offset = prMemReadLeU32(bytes.data + 10);
        if (offset < kFileHeaderBytes)
            return PrStatus_InvalidData;
        pixelsAt = offset - kFileHeaderBytes;
        at += kFileHeaderBytes;
        size -= kFileHeaderBytes;
    }
    Dib dib{};
    PrBmpInfo info{};
    PrStatus status = readDib(at, size, source, pixelsAt, &dib, &info);
    if (status != PrStatus_Ok)
        return status;
    const bool rle = dib.compression == Compression_Rle8 || dib.compression == Compression_Rle4;
    info.extent = PrUVec2{dib.width, dib.height};
    info.channels = dib.icon || rle || dib.alphaField ? 4 : 3;
    info.bitCount = dib.bits;
    info.workingBytes = held + sizeof(PrBmpDecoder);
    const uint64_t pixels = uint64_t(dib.width) * dib.height;
    if (pixels > bounds.pixels || pixels > bounds.work || pixels * info.channels > bounds.outputBytes ||
            info.workingBytes > bounds.workingBytes)
        return PrStatus_Limit;
    // The rows of a bitmap that is not run-length encoded, and the mask of an icon's bitmap that has no alpha of its
    // own, have sizes that the header gives.
    const uint64_t rows = dib.rowBytes * dib.height;
    const uint64_t mask = dib.icon && dib.bits != 32 ? ((uint64_t(dib.width) + 31) / 32) * 4 * dib.height : 0;
    if (!rle && dib.pixels.size < rows + mask)
        status = PrStatus_Truncated;
    PrBmpDecoder* decoder = allocZeroed<PrBmpDecoder>();
    decoder->dib = dib;
    decoder->info = info;
    *out = decoder;
    return status;
}

void prBmpDecoderDestroy(PrBmpDecoder* PR_NULLABLE decoder) {
    prMemoryFree(decoder);
}

const PrBmpInfo* prBmpDecoderInfo(const PrBmpDecoder* decoder) {
    return &decoder->info;
}

PrStatus prBmpDecoderRead(PrBmpDecoder* decoder, PrPixels output) {
    const Dib& dib = decoder->dib;
    PR_ASSERT(output.extent == decoder->info.extent);
    PR_ASSERT(output.stride >= size_t(output.extent.x) * decoder->info.channels);
    if (dib.compression == Compression_Rle8 || dib.compression == Compression_Rle4)
        return decodeRle(dib, output);
    return decodeRows(dib, decoder->info.channels, output);
}
