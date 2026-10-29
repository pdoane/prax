// PNGs as sources (source_image.h).

#include "tools/viewer/source_image.h"

#include "pr/compress_builtin.h"
#include "pr/draw_image.h"
#include "pr/image_builtin.h"

#include "tools/viewer/documents.h"

namespace viewer {

namespace fs = std::filesystem;

struct LoadedImage {
    const PrDrawImage* image = nullptr;

    LoadedImage() = default;
    LoadedImage(const LoadedImage&) = delete;
    LoadedImage& operator=(const LoadedImage&) = delete;
    ~LoadedImage() { prDrawImageRelease(image); }
};

std::shared_ptr<LoadedImage> loadImage(const fs::path& path) {
    std::string bytes = readFile(path);
    PrCompressCodec* compressCodec = prCompressBuiltinCreate();
    PrImageCodec* imageCodec = prImageBuiltinCreate(compressCodec);
    PrImageSource source{};
    source.memory = PrByteView{reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()};
    PrImageDecoder* decoder = prImageDecoderCreate(imageCodec);
    std::shared_ptr<LoadedImage> image = std::make_shared<LoadedImage>();
    if (prImageDecoderOpen(decoder, source, nullptr) == PrStatus_Ok) {
        const PrImageInfo* info = prImageDecoderInfo(decoder);
        const bool swaps = prImageOrientationSwapsAxes(info->orientation);
        const uint32_t width = swaps ? info->extent.y : info->extent.x;
        const uint32_t height = swaps ? info->extent.x : info->extent.y;
        std::vector<uint8_t> pixels(size_t(width) * height * 4);
        const PrPixels decoded{pixels.data(), size_t(width) * 4, {width, height}};
        if (prImageDecoderReadRgba8(decoder, 0, PrImageAlpha_Straight, info->orientation, decoded) == PrStatus_Ok) {
            PrDrawImageDesc desc{prPixelsView(decoded), PrPixelFormat_Rgba8, PrDrawAlpha_Straight, PrColorSpace_Srgb};
            prDrawImageCreate(&desc, &image->image);
        }
    }
    prImageDecoderDestroy(decoder);
    prImageCodecDestroy(imageCodec);
    prCompressCodecDestroy(compressCodec);
    return image->image != nullptr ? image : nullptr;
}

PrVec2 imageSize(const LoadedImage& image) {
    PrUVec2 extent = prDrawImageExtent(image.image);
    return PrVec2{float(extent.x), float(extent.y)};
}

void drawImage(LoadedImage& image, PrDrawList* list, float width, float height) {
    PrUVec2 extent = prDrawImageExtent(image.image);
    PrDrawSource source = prDrawSourceImage(image.image, PrRect{0, 0, float(extent.x), float(extent.y)});
    PrDrawBrush brush = prDrawBrushImage(source, PrRect{0, 0, width, height}, PrDrawSampling_Nearest);
    prDrawListRect(list, PrRect{0, 0, width, height}, {.brush = &brush});
}

bool imagePixels(LoadedImage& image, uint32_t width, uint32_t height, std::vector<uint8_t>& bgra) {
    PrUVec2 extent = prDrawImageExtent(image.image);
    if (extent.x != width || extent.y != height)
        return false;
    bgra.resize(size_t(width) * height * 4);
    const PrPixelView source = prDrawImagePixels(image.image);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const uint8_t* from = source.data + y * source.stride + x * 4;
            uint8_t* to = bgra.data() + (size_t(y) * width + x) * 4;
            uint32_t a = from[3];
            to[0] = uint8_t((from[2] * a + 127) / 255 + 255 - a);
            to[1] = uint8_t((from[1] * a + 127) / 255 + 255 - a);
            to[2] = uint8_t((from[0] * a + 127) / 255 + 255 - a);
            to[3] = 255;
        }
    }
    return true;
}

} // namespace viewer
