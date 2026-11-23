// Targa decoding.
//
// A file is an 18-byte header, an image ID, a color map, and the pixels. A TGA 2.0 file ends with a footer that locates
// an extension area. All integers are little-endian.

#include "pr/tga.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/mem.h"
#include "pr/pixels.h"

namespace pr_impl {

namespace {

constexpr uint8_t kSignature[18] = {'T', 'R', 'U', 'E', 'V', 'I', 'S', 'I', 'O', 'N', '-', 'X', 'F', 'I', 'L', 'E', '.', 0};

// The image types: color-mapped, true-color and black and white pixels, and each of them run-length encoded.
enum {
    Type_Mapped = 1,
    Type_Color = 2,
    Type_Gray = 3,
    Type_RleMapped = 9,
    Type_RleColor = 10,
    Type_RleGray = 11,
};
typedef uint8_t Type;

// The extension area's size, and where its attributes type is: what the alpha of the pixels means.
constexpr uint32_t kExtensionBytes = 495;
constexpr uint32_t kAttributesType = 494;

// The fields of a header.
struct Header {
    uint8_t idLength;
    uint8_t mapType;
    uint8_t type;
    uint16_t mapFirst;
    uint16_t mapLength;
    uint8_t mapEntryBits;
    uint16_t width;
    uint16_t height;
    uint8_t depth;
    uint8_t descriptor;
};

Header headerOf(const uint8_t* at) {
    return Header{
        .idLength = at[0],
        .mapType = at[1],
        .type = at[2],
        .mapFirst = prMemReadLeU16(at + 3),
        .mapLength = prMemReadLeU16(at + 5),
        .mapEntryBits = at[7],
        .width = prMemReadLeU16(at + 12),
        .height = prMemReadLeU16(at + 14),
        .depth = at[16],
        .descriptor = at[17],
    };
}

// Returns the image type without its run-length encoding.
uint8_t baseType(uint8_t type) {
    return type >= Type_RleMapped ? uint8_t(type - 8) : type;
}

bool isColorBits(uint8_t bits) {
    return bits == 15 || bits == 16 || bits == 24 || bits == 32;
}

// Returns true if the header is one that the specification allows, as prTgaHasHeader describes.
bool plausible(const Header& h) {
    const uint8_t base = baseType(h.type);
    if (base < Type_Mapped || base > Type_Gray || h.type > Type_RleGray ||
            (h.type > Type_Gray && h.type < Type_RleMapped))
        return false;
    if (h.mapType > 1 || h.width == 0 || h.height == 0 || (h.descriptor & 0xc0) != 0)
        return false;
    if (h.mapType == 1 && (h.mapLength == 0 || !isColorBits(h.mapEntryBits)))
        return false;
    if (h.mapType == 0 && (h.mapFirst != 0 || h.mapLength != 0))
        return false;
    switch (base) {
        case Type_Mapped: return h.mapType == 1 && (h.depth == 8 || h.depth == 16);
        case Type_Color: return isColorBits(h.depth);
        default: return h.depth == 8 || h.depth == 16;
    }
}

// What a decoder reads the pixels with: the color map, how each pixel or map entry is stored, and the decoded layout.
struct Layout {
    PrByteView pixels;          // from the first pixel to the end of the file
    const uint8_t* PR_NULLABLE map;
    uint32_t mapFirst;
    uint32_t mapLength;
    uint32_t entryBits;         // of a color: a pixel of a true-color image, or a map entry
    uint32_t pixelBytes;        // of a stored pixel
    bool mapped;
    bool gray;
    bool rle;
};

// Decodes the color of `bits` at `at` to red, green, blue and alpha.
void colorOf(const uint8_t* at, uint32_t bits, uint8_t* rgba) {
    if (bits <= 16) {
        const uint32_t v = prMemReadLeU16(at);
        const uint32_t r = (v >> 10) & 31;
        const uint32_t g = (v >> 5) & 31;
        const uint32_t b = v & 31;
        // The Truevision specification gives the layout of a 16-bit pixel, ARRRRRGG GGGBBBBB, and no conversion of its
        // 5-bit fields to wider values, so each widens by replicating its bits (prPixelChannel8).
        rgba[0] = prPixelChannel8(r, 5);
        rgba[1] = prPixelChannel8(g, 5);
        rgba[2] = prPixelChannel8(b, 5);
        rgba[3] = (v & 0x8000) != 0 ? 255 : 0;
        return;
    }
    rgba[0] = at[2];
    rgba[1] = at[1];
    rgba[2] = at[0];
    rgba[3] = bits == 32 ? at[3] : 255;
}

// Decodes the stored pixel at `at` into `out`, of `channels` channels. Returns false for an index past the color map.
bool pixelOf(const Layout& layout, const uint8_t* at, uint32_t channels, uint8_t* out) {
    if (layout.gray) {
        out[0] = at[0];
        if (channels == 2)
            out[1] = at[1];
        return true;
    }
    const uint8_t* color = at;
    if (layout.mapped) {
        const uint32_t index = layout.pixelBytes == 1 ? at[0] : prMemReadLeU16(at);
        if (index < layout.mapFirst || index - layout.mapFirst >= layout.mapLength)
            return false;
        color = layout.map + size_t(index - layout.mapFirst) * ((layout.entryBits + 7) / 8);
    }
    uint8_t rgba[4];
    colorOf(color, layout.entryBits, rgba);
    prMemCopy(out, rgba, channels);
    return true;
}

// Decodes the pixels into `output`, in the stored order. Returns Truncated if the bytes end first, and InvalidData for
// an index past the color map.
PrStatus decode(const Layout& layout, const PrTgaInfo& info, PrPixels output) {
    const uint32_t width = info.extent.x;
    const uint32_t channels = info.channels;
    const uint64_t total = uint64_t(width) * info.extent.y;
    const PrByteView bytes = layout.pixels;
    size_t at = 0;
    // The pixels left in the packet, and whether it repeats one pixel.
    uint32_t packet = 0;
    bool repeats = false;
    uint8_t repeated[4] = {};
    for (uint64_t i = 0; i < total; ++i) {
        uint8_t* to = output.data + size_t(i / width) * output.stride + size_t(i % width) * channels;
        bool ok = true;
        if (!layout.rle) {
            if (bytes.size - at < layout.pixelBytes) {
                ok = false;
            } else {
                if (!pixelOf(layout, bytes.data + at, channels, to))
                    return PrStatus_InvalidData;
                at += layout.pixelBytes;
            }
        } else {
            // A packet's header is its count less one, with the high bit set for a run of one pixel.
            if (packet == 0 && at < bytes.size) {
                const uint8_t head = bytes.data[at++];
                packet = (head & 0x7fu) + 1;
                repeats = (head & 0x80) != 0;
                if (repeats) {
                    if (bytes.size - at < layout.pixelBytes) {
                        packet = 0;
                    } else {
                        if (!pixelOf(layout, bytes.data + at, channels, repeated))
                            return PrStatus_InvalidData;
                        at += layout.pixelBytes;
                    }
                }
            }
            if (packet == 0) {
                ok = false;
            } else if (repeats) {
                prMemCopy(to, repeated, channels);
                --packet;
            } else if (bytes.size - at < layout.pixelBytes) {
                ok = false;
            } else {
                if (!pixelOf(layout, bytes.data + at, channels, to))
                    return PrStatus_InvalidData;
                at += layout.pixelBytes;
                --packet;
            }
        }
        if (!ok) {
            // The pixels that the file does not hold are zero.
            for (uint64_t rest = i; rest < total; ++rest)
                prMemZero(output.data + size_t(rest / width) * output.stride + size_t(rest % width) * channels, channels);
            return PrStatus_Truncated;
        }
    }
    return PrStatus_Ok;
}

// Returns the attributes type of the extension area that the footer of `bytes` locates, or -1 if there is none.
int32_t attributesType(PrByteView bytes) {
    if (!prTgaHasFooter(bytes))
        return -1;
    const uint64_t offset = prMemReadLeU32(bytes.data + bytes.size - kPrTgaFooterBytes);
    if (offset == 0 || offset > bytes.size - kPrTgaFooterBytes ||
            bytes.size - kPrTgaFooterBytes - offset < kExtensionBytes ||
            prMemReadLeU16(bytes.data + offset) != kExtensionBytes)
        return -1;
    return bytes.data[offset + kAttributesType];
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrTgaDecoder {
    Layout layout;
    PrTgaInfo info;
};

bool prTgaHasHeader(PrByteView bytes) {
    return bytes.size >= kPrTgaHeaderBytes && plausible(headerOf(bytes.data));
}

bool prTgaHasFooter(PrByteView bytes) {
    return bytes.size >= kPrTgaHeaderBytes + kPrTgaFooterBytes &&
        prMemEquals(bytes.data + bytes.size - sizeof(kSignature), kSignature, sizeof(kSignature));
}

PrTgaLimits prTgaLimitsDefault(void) {
    return PrTgaLimits{
        .pixels = 64u * 1024u * 1024u,
        .workingBytes = 512u * 1024u * 1024u,
        .outputBytes = 512u * 1024u * 1024u,
        .work = 1000000000u,
    };
}

PrStatus prTgaDecoderCreate(PrByteView bytes, const PrTgaLimits* PR_NULLABLE limits, uint64_t held,
        PrTgaDecoder* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    const PrTgaLimits bounds = limits != nullptr ? *limits : prTgaLimitsDefault();
    if (!prTgaHasHeader(bytes))
        return PrStatus_InvalidData;
    const Header h = headerOf(bytes.data);
    const uint8_t base = baseType(h.type);
    if (base == Type_Mapped && h.depth != 8)
        return PrStatus_NotSupported;
    const uint64_t mapBytes = h.mapType == 1 ? uint64_t(h.mapLength) * ((h.mapEntryBits + 7u) / 8u) : 0;
    const uint64_t mapAt = kPrTgaHeaderBytes + uint64_t(h.idLength);
    if (mapAt + mapBytes > bytes.size)
        return PrStatus_InvalidData;
    Layout layout{
        .pixels = PrByteView{bytes.data + mapAt + mapBytes, size_t(bytes.size - mapAt - mapBytes)},
        .map = base == Type_Mapped ? bytes.data + mapAt : nullptr,
        .mapFirst = h.mapFirst,
        .mapLength = h.mapLength,
        .entryBits = base == Type_Mapped ? h.mapEntryBits : h.depth,
        .pixelBytes = (h.depth + 7u) / 8u,
        .mapped = base == Type_Mapped,
        .gray = base == Type_Gray,
        .rle = h.type >= Type_RleMapped,
    };
    // Alpha is stored by a pixel of 32 or 16 bits, a map entry of those, and black and white of 16 bits. The attribute
    // bits say whether it is alpha, and the extension area's attributes type says what kind.
    const uint32_t colorBits = layout.gray ? 0 : layout.entryBits;
    const bool stored = colorBits == 32 || colorBits == 16 || (layout.gray && h.depth == 16);
    PrTgaAlpha alpha = stored && (h.descriptor & 15) != 0 ? PrTgaAlpha_Straight : PrTgaAlpha_None;
    const int32_t attributes = stored ? attributesType(bytes) : -1;
    if (attributes == 3)
        alpha = PrTgaAlpha_Straight;
    else if (attributes == 4)
        alpha = PrTgaAlpha_Premultiplied;
    else if (attributes >= 0)
        alpha = PrTgaAlpha_None;
    const uint8_t colors = layout.gray ? 1 : 3;
    PrTgaInfo info{
        .extent = PrUVec2{h.width, h.height},
        .channels = uint8_t(colors + (alpha != PrTgaAlpha_None ? 1 : 0)),
        .gray = layout.gray,
        .alpha = alpha,
        .fromRight = (h.descriptor & 0x10) != 0,
        .fromBottom = (h.descriptor & 0x20) == 0,
        .workingBytes = held + sizeof(PrTgaDecoder),
    };
    const uint64_t pixels = uint64_t(h.width) * h.height;
    if (pixels > bounds.pixels || pixels > bounds.work || pixels * info.channels > bounds.outputBytes ||
            info.workingBytes > bounds.workingBytes)
        return PrStatus_Limit;
    PrTgaDecoder* decoder = allocZeroed<PrTgaDecoder>();
    decoder->layout = layout;
    decoder->info = info;
    *out = decoder;
    return PrStatus_Ok;
}

void prTgaDecoderDestroy(PrTgaDecoder* PR_NULLABLE decoder) {
    prMemoryFree(decoder);
}

const PrTgaInfo* prTgaDecoderInfo(const PrTgaDecoder* decoder) {
    return &decoder->info;
}

PrStatus prTgaDecoderRead(PrTgaDecoder* decoder, PrPixels output) {
    PR_ASSERT(output.extent == decoder->info.extent);
    PR_ASSERT(output.stride >= size_t(output.extent.x) * decoder->info.channels);
    return decode(decoder->layout, decoder->info, output);
}
