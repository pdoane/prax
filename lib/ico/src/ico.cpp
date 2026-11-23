// ICO decoding. lib/bmp decodes the bitmap images.

#include "pr/ico.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/bmp.h"
#include "pr/mem.h"

namespace pr_impl {

namespace {

// An image chosen from an ICO directory: a PNG, still encoded, or the decoder of a bitmap.
struct IcoImage {
    const uint8_t* PR_NULLABLE bytes;   // the encoded image
    size_t size;
    uint32_t width;
    uint32_t height;
    uint16_t bitCount;
    PrBmpDecoder* PR_NULLABLE bitmap;
};

constexpr uint8_t kPngSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};

// Opens the bitmap of `image` without limits, which the chosen image meets later. Returns false for a bitmap that does
// not decode whole, or whose extent is not the directory's.
bool openBitmap(IcoImage& image) {
    const PrBmpLimits unbounded{
        .pixels = UINT64_MAX,
        .workingBytes = UINT64_MAX,
        .outputBytes = UINT64_MAX,
        .work = UINT64_MAX,
    };
    PrBmpDecoder* bitmap = nullptr;
    const PrStatus status = prBmpDecoderCreate(PrByteView{image.bytes, image.size}, PrBmpSource_Icon, &unbounded, 0, &bitmap);
    if (status != PrStatus_Ok || prBmpDecoderInfo(bitmap)->extent != PrUVec2{image.width, image.height}) {
        prBmpDecoderDestroy(bitmap);
        return false;
    }
    image.bitCount = prBmpDecoderInfo(bitmap)->bitCount;
    image.bitmap = bitmap;
    return true;
}

// Chooses the largest complete image in `bytes` that the decoder reads. Among images of equal area, it chooses the one
// with the greatest bit depth. Returns false for a malformed directory or one without such an image.
bool selectImage(PrByteView bytes, IcoImage* out) {
    *out = {};
    if (bytes.size < 6 || prMemReadLeU16(bytes.data) != 0 || prMemReadLeU16(bytes.data + 2) != 1)
        return false;
    const uint32_t count = prMemReadLeU16(bytes.data + 4);
    const size_t directoryEnd = 6 + size_t(count) * 16;
    if (count == 0 || directoryEnd > bytes.size)
        return false;
    bool ok = true;
    for (uint32_t i = 0; i < count && ok; ++i) {
        const uint8_t* entry = bytes.data + 6 + size_t(i) * 16;
        const size_t length = prMemReadLeU32(entry + 8);
        const size_t offset = prMemReadLeU32(entry + 12);
        if (offset < directoryEnd || offset > bytes.size || length > bytes.size - offset) {
            ok = false;
            continue;
        }
        IcoImage image = {};
        image.bytes = bytes.data + offset;
        image.size = length;
        image.width = entry[0] == 0 ? 256 : entry[0];
        image.height = entry[1] == 0 ? 256 : entry[1];
        const bool png =
            length >= sizeof(kPngSignature) && prMemEquals(image.bytes, kPngSignature, sizeof(kPngSignature));
        if (png) {
            if (length < 29 || prMemReadBeU32(image.bytes + 16) != image.width ||
                    prMemReadBeU32(image.bytes + 20) != image.height)
                continue;
            image.bitCount = 32;
        } else if (!openBitmap(image)) {
            continue;
        }
        const uint32_t area = image.width * image.height;
        const uint32_t heldArea = out->width * out->height;
        if (area > heldArea || (area == heldArea && image.bitCount > out->bitCount)) {
            prBmpDecoderDestroy(out->bitmap);
            *out = image;
        } else {
            prBmpDecoderDestroy(image.bitmap);
        }
    }
    if (!ok || out->bytes == nullptr) {
        prBmpDecoderDestroy(out->bitmap);
        *out = {};
        return false;
    }
    return true;
}

} // namespace

// An icon open for reading.
struct IcoDecoder {
    PrIcoInfo info;
    IcoImage image;
};

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrIcoDecoder : IcoDecoder {};

bool prIcoHasSignature(PrByteView bytes) {
    return bytes.size >= 6 && prMemReadLeU16(bytes.data) == 0 && prMemReadLeU16(bytes.data + 2) == 1;
}

PrIcoLimits prIcoLimitsDefault(void) {
    return PrIcoLimits{
        .pixels = 64u * 1024u * 1024u,
        .workingBytes = 512u * 1024u * 1024u,
        .outputBytes = 512u * 1024u * 1024u,
        .work = 1000000000u,
    };
}

PrStatus prIcoDecoderCreate(PrByteView bytes, const PrIcoLimits* PR_NULLABLE limits, uint64_t held,
        PrIcoDecoder* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    PR_ASSERT(bytes.data != nullptr || bytes.size == 0);
    PR_ASSERT(held >= bytes.size);
    const PrIcoLimits budget = limits != nullptr ? *limits : prIcoLimitsDefault();
    if (!prIcoHasSignature(bytes))
        return PrStatus_NotSupported;
    if (prMemReadLeU16(bytes.data + 4) > budget.work)
        return PrStatus_Limit;
    IcoImage image{};
    if (!selectImage(bytes, &image))
        return PrStatus_InvalidData;
    PrIcoInfo info{.extent = PrUVec2{image.width, image.height}};
    uint64_t working = held + sizeof(PrIcoDecoder);
    if (image.bitmap == nullptr) {
        info.png = PrByteView{image.bytes, image.size};
    } else {
        // The bitmap decodes into the caller's pixels, four bytes a pixel.
        const uint64_t pixels = uint64_t(image.width) * image.height;
        working += prBmpDecoderInfo(image.bitmap)->workingBytes;
        if (pixels > budget.pixels || pixels > budget.work || pixels * 4 > budget.outputBytes) {
            prBmpDecoderDestroy(image.bitmap);
            return PrStatus_Limit;
        }
    }
    if (working > budget.workingBytes) {
        prBmpDecoderDestroy(image.bitmap);
        return PrStatus_Limit;
    }
    info.workingBytes = working;
    PrIcoDecoder* decoder = allocZeroed<PrIcoDecoder>();
    decoder->info = info;
    decoder->image = image;
    *out = decoder;
    return PrStatus_Ok;
}

void prIcoDecoderDestroy(PrIcoDecoder* PR_NULLABLE decoder) {
    if (decoder == nullptr)
        return;
    prBmpDecoderDestroy(decoder->image.bitmap);
    prMemoryFree(decoder);
}

const PrIcoInfo* prIcoDecoderInfo(const PrIcoDecoder* decoder) {
    return &decoder->info;
}

PrStatus prIcoDecoderRead(PrIcoDecoder* decoder, PrPixels output) {
    PR_ASSERT(output.extent.x == decoder->info.extent.x && output.extent.y == decoder->info.extent.y);
    if (decoder->image.bitmap == nullptr)
        return PrStatus_NotSupported;
    return prBmpDecoderRead(decoder->image.bitmap, output) == PrStatus_Ok ? PrStatus_Ok : PrStatus_InvalidData;
}
