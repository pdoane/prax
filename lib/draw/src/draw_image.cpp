// Draw images: pixels that a list draws. An image is immutable once it is created.

#include "pr/draw_image.h"

#include "pr/assert.h"
#include "pr/atomic.h"
#include "pr/color_space.h"
#include "pr/float.h"
#include "pr/float16.h"
#include "pr/mem.h"
#include "pr/memory.h"
#include "pr/vec.h"

#include "draw/src/identity.h"

namespace pr_impl {

namespace {

// An image. Its pixels follow it in the same allocation.
struct DrawImage {
    mutable Atomic<uint32_t> references;    // changes behind const, as holders retain and release the immutable image
    uint64_t id;
    PrUVec2 extent;
    size_t stride;
    PrPixelFormat format;
    PrDrawAlpha alpha;
};

uint8_t* pixelsOf(DrawImage* image) {
    return reinterpret_cast<uint8_t*>(image + 1);
}

const uint8_t* pixelsOf(const DrawImage* image) {
    return reinterpret_cast<const uint8_t*>(image + 1);
}

// A pixel's channels in RGBA order as floats, in the alpha form that they were stored in.
void readPixel(const uint8_t* p, PrPixelFormat format, float out[4]) {
    if (format == PrPixelFormat_RgbaF16) {
        PrFloat16 stored[4];
        prMemCopy(stored, p, sizeof(stored));
        for (uint32_t i = 0; i < 4; ++i)
            out[i] = prFloat16ToFloat(stored[i]);
        return;
    }
    bool bgra = format == PrPixelFormat_Bgra8;
    out[0] = float(p[bgra ? 2 : 0]) / 255;
    out[1] = float(p[1]) / 255;
    out[2] = float(p[bgra ? 0 : 2]) / 255;
    out[3] = float(p[3]) / 255;
}

// Creates an image of the first frame of the open image, which is in a named RGB space other than sRGB: its stored
// samples, converted to half floats with straight alpha, arranged as the image's orientation displays them, then
// converted to extended sRGB.
PrStatus createWide(PrImageDecoder* decoder, const PrImageInfo& info, const PrDrawImage* PR_NULLABLE* PR_NONNULL out) {
    uint32_t width = info.extent.x;
    uint32_t height = info.extent.y;
    size_t storedStride = size_t(width) * prImageFormatSize(&info.format);
    size_t halfStride = size_t(width) * 8;
    uint8_t* stored = static_cast<uint8_t*>(prMemoryAlloc(storedStride * height));
    uint8_t* half = static_cast<uint8_t*>(prMemoryAlloc(halfStride * height));
    PrStatus status = prImageDecoderRead(decoder, 0, PrPixels{stored, storedStride, {width, height}});
    PrImageFormat wide{
        .model = PrColorModel_Rgb,
        .sample = PrImageSample_F16,
        .alpha = PrImageAlpha_Straight,
        .color = info.format.color,
    };
    if (status == PrStatus_Ok)
        status = prImageConvert(PrPixelView{stored, storedStride, {width, height}}, &info.format,
            PrPixels{half, halfStride, {width, height}}, &wide, 0);
    PrPixelView displayed{half, halfStride, {width, height}};
    uint8_t* oriented = nullptr;
    if (status == PrStatus_Ok && info.orientation != PrImageOrientation_Identity) {
        bool swaps = prImageOrientationSwapsAxes(info.orientation);
        PrUVec2 extent = swaps ? PrUVec2{height, width} : PrUVec2{width, height};
        oriented = static_cast<uint8_t*>(prMemoryAlloc(halfStride * height));
        PrPixels into{oriented, size_t(extent.x) * 8, extent};
        prImageOrient(displayed, 8, info.orientation, into);
        displayed = PrPixelView{oriented, into.stride, extent};
    }
    if (status == PrStatus_Ok) {
        PrDrawImageDesc desc{displayed, PrPixelFormat_RgbaF16, PrDrawAlpha_Straight, info.format.color.space};
        status = prDrawImageCreate(&desc, out);
    }
    prMemoryFree(oriented);
    prMemoryFree(half);
    prMemoryFree(stored);
    return status;
}

// Initializes an image of `extent` that has one reference and a new id. Its pixels follow it in the allocation, at
// `stride` bytes a row, and the caller fills them.
void initImage(DrawImage* image, PrUVec2 extent, size_t stride, PrPixelFormat format, PrDrawAlpha alpha) {
    prMemZero(image, sizeof(DrawImage));
    image->references.storeRelaxed(1);
    image->id = drawNextId();
    image->extent = extent;
    image->stride = stride;
    image->format = format;
    image->alpha = alpha;
}

// Converts the pixels into extended sRGB 16-bit floats, keeping their alpha as it was stored.
void convertPixels(const PrDrawImageDesc& desc, DrawImage* image) {
    uint32_t sourceBytes = prPixelFormatSize(desc.format);
    for (uint32_t y = 0; y < desc.pixels.extent.y; ++y) {
        const uint8_t* row = desc.pixels.data + y * desc.pixels.stride;
        uint8_t* out = pixelsOf(image) + y * image->stride;
        for (uint32_t x = 0; x < desc.pixels.extent.x; ++x) {
            float c[4];
            readPixel(row + x * sourceBytes, desc.format, c);
            float alpha = desc.alpha == PrDrawAlpha_Opaque ? 1 : c[3];
            bool premultiplied = desc.alpha == PrDrawAlpha_Premultiplied;
            double channels[3];
            for (uint32_t i = 0; i < 3; ++i)
                channels[i] = premultiplied && alpha > 0 ? double(c[i] / alpha) : double(c[i]);
            prColorConvert(desc.space, PrColorSpace_Srgb, channels, channels);
            PrFloat16 stored[4];
            for (uint32_t i = 0; i < 3; ++i)
                stored[i] = prFloat16FromFloat(float(channels[i]) * (premultiplied ? alpha : 1));
            stored[3] = prFloat16FromFloat(c[3]);
            prMemCopy(out + x * 8, stored, sizeof(stored));
        }
    }
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrDrawImage : DrawImage {};

PrStatus prDrawImageCreate(const PrDrawImageDesc* desc, const PrDrawImage* PR_NULLABLE* PR_NONNULL out) {
    const PrPixelView& pixels = desc->pixels;
    PR_ASSERT(pixels.data != nullptr && (desc->format == PrPixelFormat_Rgba8 || desc->format == PrPixelFormat_Bgra8 ||
        desc->format == PrPixelFormat_RgbaF16) && desc->alpha <= PrDrawAlpha_Opaque);
    PR_ASSERT(prColorSpaceModel(desc->space) == PrColorModel_Rgb);
    PR_ASSERT(size_t(pixels.extent.x) * prPixelFormatSize(desc->format) <= pixels.stride);
    PR_ASSERT(pixels.extent.x != 0 && pixels.extent.y != 0);
    *out = nullptr;
    bool convert = desc->space != PrColorSpace_Srgb;
    PrPixelFormat format = convert ? PrPixelFormat_RgbaF16 : desc->format;
    size_t stride = size_t(pixels.extent.x) * prPixelFormatSize(format);
    if (stride > (SIZE_MAX - sizeof(DrawImage)) / pixels.extent.y)
        return PrStatus_Limit;
    PrDrawImage* image = static_cast<PrDrawImage*>(prMemoryAlloc(sizeof(PrDrawImage) + stride * pixels.extent.y));
    initImage(image, pixels.extent, stride, format, desc->alpha);
    if (convert) {
        convertPixels(*desc, image);
    } else {
        for (uint32_t y = 0; y < pixels.extent.y; ++y)
            prMemCopy(pixelsOf(image) + y * stride, pixels.data + y * pixels.stride, stride);
    }
    *out = image;
    return PrStatus_Ok;
}

PrStatus prDrawImageCreateFromBytes(PrImageCodec* codec, PrByteView bytes,
        const PrDrawImage* PR_NULLABLE* PR_NONNULL out, PrImageOrientation* PR_NULLABLE orientation) {
    *out = nullptr;
    PrImageSource source{};
    source.memory = bytes;
    PrImageDecoder* decoder = prImageDecoderCreate(codec);
    PrStatus status = prImageDecoderOpen(decoder, source, nullptr);
    if (status != PrStatus_Ok) {
        prImageDecoderDestroy(decoder);
        return status;
    }
    const PrImageInfo* info = prImageDecoderInfo(decoder);
    if (orientation != nullptr)
        *orientation = info->orientation;
    // An image that names a space other than sRGB keeps its range: it is read as it is stored, converted to half floats
    // in its space, and created in that space, which converts it to extended sRGB.
    const PrImageColor& color = info->format.color;
    if (color.encoding == PrImageColorEncoding_Named && color.space != PrColorSpace_Srgb &&
            prColorSpaceModel(color.space) == PrColorModel_Rgb) {
        status = createWide(decoder, *info, out);
        prImageDecoderDestroy(decoder);
        return status;
    }
    uint32_t width = info->extent.x;
    uint32_t height = info->extent.y;
    const bool swaps = prImageOrientationSwapsAxes(info->orientation);
    if (swaps) {
        width = info->extent.y;
        height = info->extent.x;
    }
    size_t size = size_t(width) * height * 4;
    uint8_t* pixels = static_cast<uint8_t*>(prMemoryAlloc(size != 0 ? size : 4));
    if (size == 0)
        status = PrStatus_InvalidData;
    else
        status = prImageDecoderReadRgba8(decoder, 0, PrImageAlpha_Premultiplied, info->orientation,
            PrPixels{pixels, size_t(width) * 4, {width, height}});
    if (status == PrStatus_Ok) {
        PrDrawImageDesc desc{
            PrPixelView{pixels, size_t(width) * 4, {width, height}}, PrPixelFormat_Rgba8,
            PrDrawAlpha_Premultiplied, PrColorSpace_Srgb,
        };
        status = prDrawImageCreate(&desc, out);
    }
    prMemoryFree(pixels);
    prImageDecoderDestroy(decoder);
    return status;
}

const PrDrawImage* prDrawImageCreateOriented(const PrDrawImage* image, PrImageOrientation orientation) {
    bool swaps = prImageOrientationSwapsAxes(orientation);
    PrUVec2 extent = swaps ? PrUVec2{image->extent.y, image->extent.x} : image->extent;
    size_t stride = size_t(extent.x) * prPixelFormatSize(image->format);
    PrDrawImage* oriented = static_cast<PrDrawImage*>(prMemoryAlloc(sizeof(PrDrawImage) + stride * extent.y));
    initImage(oriented, extent, stride, image->format, image->alpha);
    prImageOrient(prDrawImagePixels(image), prPixelFormatSize(image->format), orientation,
        PrPixels{pixelsOf(oriented), oriented->stride, extent});
    return oriented;
}

void prDrawImageRetain(const PrDrawImage* image) {
    image->references.fetchAdd(1);
}

void prDrawImageRelease(const PrDrawImage* PR_NULLABLE image) {
    if (image == nullptr || image->references.fetchSub(1) != 1)
        return;
    prMemoryFree(const_cast<PrDrawImage*>(image));
}

PrUVec2 prDrawImageExtent(const PrDrawImage* image) {
    return image->extent;
}

PrPixelFormat prDrawImageFormat(const PrDrawImage* image) {
    return image->format;
}

PrDrawAlpha prDrawImageAlpha(const PrDrawImage* image) {
    return image->alpha;
}

PrPixelView prDrawImagePixels(const PrDrawImage* image) {
    return PrPixelView{pixelsOf(image), image->stride, image->extent};
}

uint64_t prDrawImageId(const PrDrawImage* image) {
    return image->id;
}

void prDrawImageWritePixels(const PrDrawImage* image, PrPixels target, PrPixelFormat format, PrIVec2 origin) {
    const PrPixelView source = prDrawImagePixels(image);
    const uint8_t* pixels = source.data;
    size_t stride = source.stride;
    PrPixelFormat imageFormat = prDrawImageFormat(image);
    PrDrawAlpha alpha = prDrawImageAlpha(image);
    PR_ASSERT(format == PrPixelFormat_Rgba8 || format == PrPixelFormat_Bgra8 || format == PrPixelFormat_RgbaF16);
    bool targetBgra = format == PrPixelFormat_Bgra8;
    bool targetHalf = format == PrPixelFormat_RgbaF16;
    for (uint32_t row = 0; row < prDrawImageExtent(image).y; ++row) {
        int64_t ty = int64_t(origin.y) + row;
        if (ty < 0 || ty >= int64_t(target.extent.y))
            continue;
        const uint8_t* from = pixels + size_t(row) * stride;
        uint8_t* to = target.data + size_t(ty) * target.stride;
        for (uint32_t column = 0; column < prDrawImageExtent(image).x; ++column) {
            int64_t tx = int64_t(origin.x) + column;
            if (tx < 0 || tx >= int64_t(target.extent.x))
                continue;
            // Premultiplied RGBA, with alpha in [0, 1].
            float c[4];
            if (imageFormat == PrPixelFormat_RgbaF16) {
                PrFloat16 stored[4];
                prMemCopy(stored, from + size_t(column) * 8, sizeof(stored));
                for (uint32_t k = 0; k < 4; ++k)
                    c[k] = prFloat16ToFloat(stored[k]);
            } else {
                const uint8_t* p = from + size_t(column) * 4;
                bool bgra = imageFormat == PrPixelFormat_Bgra8;
                c[0] = float(p[bgra ? 2 : 0]) / 255;
                c[1] = float(p[1]) / 255;
                c[2] = float(p[bgra ? 0 : 2]) / 255;
                c[3] = float(p[3]) / 255;
            }
            c[3] = alpha == PrDrawAlpha_Opaque ? 1 : prF32Clamp(c[3], 0, 1);
            if (alpha != PrDrawAlpha_Premultiplied) {
                for (uint32_t k = 0; k < 3; ++k)
                    c[k] = (targetHalf ? c[k] : prF32Clamp(c[k], 0, 1)) * c[3];
            }
            if (targetHalf) {
                PrFloat16 stored[4];
                for (uint32_t k = 0; k < 4; ++k)
                    stored[k] = prFloat16FromFloat(c[k]);
                prMemCopy(to + size_t(tx) * 8, stored, sizeof(stored));
                continue;
            }
            uint8_t bytes[4];
            for (uint32_t k = 0; k < 3; ++k)
                bytes[k] = uint8_t(prF32Clamp(c[k], 0, c[3]) * 255 + 0.5f);
            bytes[3] = uint8_t(c[3] * 255 + 0.5f);
            uint8_t* p = to + size_t(tx) * 4;
            p[0] = bytes[targetBgra ? 2 : 0];
            p[1] = bytes[1];
            p[2] = bytes[targetBgra ? 0 : 2];
            p[3] = bytes[3];
        }
    }
}
