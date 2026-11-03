#include "image/backends/builtin/src/ico.h"

#include "pr/mem.h"

namespace pr_impl {
namespace {

constexpr uint8_t kPngSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};

bool inspectBitmap(IcoImage& image) {
    if (image.size < 40) {
        return false;
    }
    image.headerSize = prMemReadLeU32(image.bytes);
    image.bitCount = uint16_t(prMemReadLeU16(image.bytes + 14));
    if ((image.headerSize != 40 && image.headerSize != 108 && image.headerSize != 124) ||
            image.headerSize > image.size || prMemReadLeU32(image.bytes + 4) != image.width ||
            prMemReadLeU32(image.bytes + 8) != image.height * 2 || prMemReadLeU16(image.bytes + 12) != 1 ||
            prMemReadLeU32(image.bytes + 16) != 0 || (image.bitCount != 1 && image.bitCount != 4 && image.bitCount != 8 &&
                image.bitCount != 24 && image.bitCount != 32)) {
        return false;
    }
    const uint32_t colors = prMemReadLeU32(image.bytes + 32);
    if (image.bitCount <= 8) {
        const uint32_t maximum = 1u << image.bitCount;
        if (colors > maximum) {
            return false;
        }
        image.paletteCount = colors == 0 ? maximum : colors;
    } else if (colors != 0) {
        return false;
    }
    const size_t rowBytes = ((size_t(image.width) * image.bitCount + 31) / 32) * 4;
    const size_t maskRowBytes = ((size_t(image.width) + 31) / 32) * 4;
    const size_t bitmapEnd = image.headerSize + image.paletteCount * 4 + rowBytes * image.height;
    return bitmapEnd <= image.size && (image.bitCount == 32 || maskRowBytes * image.height <= image.size - bitmapEnd);
}

} // namespace

bool icoSelectImage(PrByteView bytes, IcoImage* out) {
    *out = {};
    if (bytes.size < 6 || prMemReadLeU16(bytes.data) != 0 || prMemReadLeU16(bytes.data + 2) != 1) {
        return false;
    }
    const uint32_t count = prMemReadLeU16(bytes.data + 4);
    const size_t directoryEnd = 6 + size_t(count) * 16;
    if (count == 0 || directoryEnd > bytes.size) {
        return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* entry = bytes.data + 6 + size_t(i) * 16;
        const size_t length = prMemReadLeU32(entry + 8);
        const size_t offset = prMemReadLeU32(entry + 12);
        if (offset < directoryEnd || offset > bytes.size || length > bytes.size - offset) {
            return false;
        }
        IcoImage image = {};
        image.bytes = bytes.data + offset;
        image.size = length;
        image.width = entry[0] == 0 ? 256 : entry[0];
        image.height = entry[1] == 0 ? 256 : entry[1];
        image.png = length >= sizeof(kPngSignature) && prMemEquals(image.bytes, kPngSignature, sizeof(kPngSignature));
        if (image.png) {
            if (length < 29 || prMemReadBeU32(image.bytes + 16) != image.width ||
                    prMemReadBeU32(image.bytes + 20) != image.height) {
                continue;
            }
            image.bitCount = 32;
        } else if (!inspectBitmap(image)) {
            continue;
        }
        const uint32_t area = image.width * image.height;
        const uint32_t heldArea = out->width * out->height;
        if (area > heldArea || (area == heldArea && image.bitCount > out->bitCount)) {
            *out = image;
        }
    }
    return out->bytes != nullptr;
}

bool icoDecodeBitmap(const IcoImage& image, uint8_t* pixels, size_t pixelsSize) {
    if (image.png || pixelsSize != size_t(image.width) * image.height * 4) {
        return false;
    }
    const size_t rowBytes = ((size_t(image.width) * image.bitCount + 31) / 32) * 4;
    const size_t maskRowBytes = ((size_t(image.width) + 31) / 32) * 4;
    const uint8_t* palette = image.bytes + image.headerSize;
    const uint8_t* bitmap = palette + image.paletteCount * 4;
    const size_t maskOffset = image.headerSize + image.paletteCount * 4 + rowBytes * image.height;
    const bool hasMask = maskRowBytes * image.height <= image.size - maskOffset;
    bool hasAlpha = false;
    if (image.bitCount == 32) {
        for (uint32_t y = 0; y < image.height && !hasAlpha; ++y) {
            for (uint32_t x = 0; x < image.width; ++x) {
                hasAlpha = hasAlpha || bitmap[size_t(y) * rowBytes + size_t(x) * 4 + 3] != 0;
            }
        }
    }
    for (uint32_t y = 0; y < image.height; ++y) {
        const uint32_t sourceY = image.height - 1 - y;
        const uint8_t* source = bitmap + size_t(sourceY) * rowBytes;
        const uint8_t* mask = hasMask ? image.bytes + maskOffset + size_t(sourceY) * maskRowBytes : nullptr;
        for (uint32_t x = 0; x < image.width; ++x) {
            const uint8_t* color = nullptr;
            if (image.bitCount <= 8) {
                const uint32_t bit = x * image.bitCount;
                const uint32_t index = (source[bit / 8] >> (8 - image.bitCount - bit % 8)) & ((1u << image.bitCount) - 1);
                if (index >= image.paletteCount) {
                    return false;
                }
                color = palette + index * 4;
            } else {
                color = source + size_t(x) * (image.bitCount / 8);
            }
            uint8_t* pixel = pixels + (size_t(y) * image.width + x) * 4;
            pixel[0] = color[2];
            pixel[1] = color[1];
            pixel[2] = color[0];
            // Legacy 32-bit icons with all-zero alpha use the AND mask.
            if (hasAlpha)
                pixel[3] = color[3];
            else if (mask != nullptr && (mask[x / 8] & (0x80u >> (x % 8))) != 0)
                pixel[3] = 0;
            else
                pixel[3] = 255;
        }
    }
    return true;
}

} // namespace pr_impl
