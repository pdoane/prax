// PNG files that the harnesses write, with the built-in codecs.

#include "tools/harness/harness_png.h"

#include "pr/compress_builtin.h"
#include "pr/image_builtin.h"

#include <cstdio>
#include <vector>

using namespace pr;

namespace harness {

bool harnessWritePng(const char* path, PrPixelView rgba) {
    PrCompressCodec* compress = prCompressBuiltinCreate();
    PrImageCodec* images = prImageBuiltinCreate(compress);
    // Room for the rows stored uncompressed, which is the most that the encoding can grow to, so that one call
    // compresses the whole image.
    std::vector<uint8_t> png(size_t(rgba.extent.x) * rgba.extent.y * 4 + size_t(rgba.extent.y) * 2 + 4096);
    size_t size = 0;
    bool ok = prImageCodecEncode(images, "image/png", rgba, png.data(), png.size(), &size) == PrStatus_Ok;
    if (ok && size > png.size()) {
        png.resize(size);
        ok = prImageCodecEncode(images, "image/png", rgba, png.data(), png.size(), &size) == PrStatus_Ok;
    }
    prImageCodecDestroy(images);
    prCompressCodecDestroy(compress);
    if (!ok)
        return false;
    FILE* file = std::fopen(path, "wb");
    if (file == nullptr)
        return false;
    bool written = std::fwrite(png.data(), 1, size, file) == size;
    return std::fclose(file) == 0 && written;
}

void harnessStraightRgba(PrPixelView bgra, uint8_t* out) {
    for (uint32_t y = 0; y < bgra.extent.y; ++y) {
        for (uint32_t x = 0; x < bgra.extent.x; ++x) {
            const uint8_t* in = bgra.data + bgra.stride * y + x * 4;
            uint8_t* pixel = out + (size_t(y) * bgra.extent.x + x) * 4;
            uint32_t a = in[3];
            auto straight = [&](uint32_t c) { return uint8_t(a == 0 ? 0 : (c * 255 + a / 2) / a); };
            pixel[0] = straight(in[2]);
            pixel[1] = straight(in[1]);
            pixel[2] = straight(in[0]);
            pixel[3] = uint8_t(a);
        }
    }
}

} // namespace harness
