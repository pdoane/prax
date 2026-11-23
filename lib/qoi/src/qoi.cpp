// QOI decoding.

#include "pr/qoi.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/mem.h"

namespace pr_impl {

namespace {

constexpr uint8_t kMagic[4] = {'q', 'o', 'i', 'f'};

// The tags of the chunks. A chunk of none of these 2-bit tags is a run, of 0xc0. The two 8-bit tags take precedence
// over the 2-bit tags that share their top bits.
constexpr uint8_t kOpRgb = 0xfe;
constexpr uint8_t kOpRgba = 0xff;
constexpr uint8_t kOpIndex = 0x00;
constexpr uint8_t kOpDiff = 0x40;
constexpr uint8_t kOpLuma = 0x80;
constexpr uint8_t kMask2 = 0xc0;

struct Pixel {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t a;
};

// The position of a pixel in the array of the pixels seen before.
uint32_t hashOf(Pixel p) {
    return (uint32_t(p.r) * 3 + uint32_t(p.g) * 5 + uint32_t(p.b) * 7 + uint32_t(p.a) * 11) % 64;
}

// Writes transparent black to the pixels of `output` from (x, y) on, which the chunks did not code.
void clearFrom(PrPixels output, uint32_t channels, uint32_t x, uint32_t y) {
    prMemZero(output.data + size_t(y) * output.stride + size_t(x) * channels, size_t(output.extent.x - x) * channels);
    for (uint32_t row = y + 1; row < output.extent.y; ++row)
        prMemZero(output.data + size_t(row) * output.stride, size_t(output.extent.x) * channels);
}

// Decodes the chunks after the header into `output`. Returns Truncated if the chunks end before the pixels do, and
// InvalidData if a run passes the last pixel.
PrStatus decode(PrByteView bytes, const PrQoiInfo& info, PrPixels output) {
    PR_ASSERT(output.extent == info.extent);
    PR_ASSERT(output.stride >= size_t(info.extent.x) * info.channels);
    Pixel seen[64] = {};
    Pixel pixel{0, 0, 0, 255};
    size_t at = kPrQoiHeaderBytes;
    uint32_t run = 0;
    const uint32_t channels = info.channels;
    uint64_t left = uint64_t(info.extent.x) * info.extent.y;
    for (uint32_t y = 0; y < info.extent.y; ++y) {
        uint8_t* row = output.data + size_t(y) * output.stride;
        for (uint32_t x = 0; x < info.extent.x; ++x, --left) {
            if (run != 0) {
                --run;
            } else {
                // The bytes of the chunk after its tag.
                const size_t remaining = at < bytes.size ? bytes.size - at - 1 : 0;
                const uint8_t tag = at < bytes.size ? bytes.data[at] : 0;
                const uint32_t needed = tag == kOpRgb ? 3 : tag == kOpRgba ? 4 : (tag & kMask2) == kOpLuma ? 1 : 0;
                if (at >= bytes.size || remaining < needed) {
                    clearFrom(output, channels, x, y);
                    return PrStatus_Truncated;
                }
                const uint8_t* chunk = bytes.data + at + 1;
                at += 1 + needed;
                if (tag == kOpRgb) {
                    pixel.r = chunk[0];
                    pixel.g = chunk[1];
                    pixel.b = chunk[2];
                } else if (tag == kOpRgba) {
                    pixel = Pixel{chunk[0], chunk[1], chunk[2], chunk[3]};
                } else if ((tag & kMask2) == kOpIndex) {
                    pixel = seen[tag];
                } else if ((tag & kMask2) == kOpDiff) {
                    pixel.r = uint8_t(pixel.r + ((tag >> 4) & 3) - 2);
                    pixel.g = uint8_t(pixel.g + ((tag >> 2) & 3) - 2);
                    pixel.b = uint8_t(pixel.b + (tag & 3) - 2);
                } else if ((tag & kMask2) == kOpLuma) {
                    const int32_t dg = int32_t(tag & 0x3f) - 32;
                    pixel.r = uint8_t(int32_t(pixel.r) + dg + int32_t(chunk[0] >> 4) - 8);
                    pixel.g = uint8_t(int32_t(pixel.g) + dg);
                    pixel.b = uint8_t(int32_t(pixel.b) + dg + int32_t(chunk[0] & 15) - 8);
                } else {
                    // A run repeats the pixel before it, one to 62 times, this one among them.
                    run = tag & 0x3f;
                    if (run >= left)
                        return PrStatus_InvalidData;
                }
                seen[hashOf(pixel)] = pixel;
            }
            uint8_t* to = row + size_t(x) * channels;
            to[0] = pixel.r;
            to[1] = pixel.g;
            to[2] = pixel.b;
            if (channels == 4)
                to[3] = pixel.a;
        }
    }
    return PrStatus_Ok;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrQoiDecoder {
    PrByteView bytes;
    PrQoiInfo info;
};

bool prQoiHasSignature(PrByteView bytes) {
    return bytes.size >= sizeof(kMagic) && prMemEquals(bytes.data, kMagic, sizeof(kMagic));
}

PrQoiLimits prQoiLimitsDefault(void) {
    return PrQoiLimits{
        .pixels = 64u * 1024u * 1024u,
        .workingBytes = 512u * 1024u * 1024u,
        .outputBytes = 512u * 1024u * 1024u,
        .work = 1000000000u,
    };
}

PrStatus prQoiDecoderCreate(PrByteView bytes, const PrQoiLimits* PR_NULLABLE limits, uint64_t held,
        PrQoiDecoder* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    const PrQoiLimits bounds = limits != nullptr ? *limits : prQoiLimitsDefault();
    if (!prQoiHasSignature(bytes))
        return PrStatus_NotSupported;
    if (bytes.size < kPrQoiHeaderBytes)
        return PrStatus_InvalidData;
    const PrUVec2 extent{prMemReadBeU32(bytes.data + 4), prMemReadBeU32(bytes.data + 8)};
    const uint8_t channels = bytes.data[12];
    const uint8_t colorspace = bytes.data[13];
    if (extent.x == 0 || extent.y == 0 || (channels != 3 && channels != 4) || colorspace > PrQoiColorspace_Linear)
        return PrStatus_InvalidData;
    const uint64_t pixels = uint64_t(extent.x) * extent.y;
    const uint64_t working = held + sizeof(PrQoiDecoder);
    if (pixels > bounds.pixels || pixels > bounds.work || pixels * channels > bounds.outputBytes ||
            working > bounds.workingBytes)
        return PrStatus_Limit;
    PrQoiDecoder* decoder = allocZeroed<PrQoiDecoder>();
    decoder->bytes = bytes;
    decoder->info = PrQoiInfo{
        .extent = extent,
        .channels = channels,
        .colorspace = PrQoiColorspace(colorspace),
        .workingBytes = working,
    };
    *out = decoder;
    return PrStatus_Ok;
}

void prQoiDecoderDestroy(PrQoiDecoder* PR_NULLABLE decoder) {
    prMemoryFree(decoder);
}

const PrQoiInfo* prQoiDecoderInfo(const PrQoiDecoder* decoder) {
    return &decoder->info;
}

PrStatus prQoiDecoderRead(PrQoiDecoder* decoder, PrPixels output) {
    return decode(decoder->bytes, decoder->info, output);
}
