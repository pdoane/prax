#include "pr/array.h"
#include "pr/compress_select.h"
#include "pr/draw_cpu.h"
#include "pr/draw_glyph.h"
#include "pr/file.h"
#include "pr/image_select.h"
#include "pr/image_texture_source.h"
#include "pr/string.h"
#include "pr/svg_glyph.h"

#include "tools/harness/harness_textures.h"

#include "tools/harness/test.h"

using namespace harness;

namespace pr_impl {

namespace {

using namespace pr;

const PrFont* load(const char* name) {
    const PrFont* font = nullptr;
    REQUIRE(prFontCreate(openFile(format("{}/{}", PR_FONTS_DIR, name)), 0, nullptr, &font) == PrStatus_Ok);
    return font;
}

// Renders the list onto `size` by `size` transparent RGBA pixels on the tests' memory device, reading glyphs' images
// with the codec if it is given.
Array<uint8_t> render(const PrDrawList* list, uint32_t size, PrImageCodecSet* imageCodec = nullptr) {
    Array<uint8_t> bytes(size_t(size) * size * 4);
    PrDrawCpuOptions options = prDrawCpuOptionsDefault();
    options.device = harnessMemoryDevice();
    PrDrawGlyphDecoder* decoder =
        imageCodec != nullptr ? prSvgGlyphDecoderCreate(imageCodec, nullptr, harnessMemoryTextures()) : nullptr;
    options.glyphDecoder = decoder;
    PrDrawRenderer* renderer = prDrawCpuCreate(&options);
    PrPixels into{bytes.data(), size_t(size) * 4, {size, size}};
    REQUIRE(prDrawRendererRenderPixels(renderer, list, into, PrPixelFormat_Rgba8) == PrStatus_Ok);
    prDrawRendererDestroy(renderer);
    prDrawGlyphDecoderDestroy(decoder);
    return bytes;
}

String pixel(PrByteView bytes, uint32_t size, uint32_t x, uint32_t y) {
    const uint8_t* p = bytes.data + (size_t(y) * size + x) * 4;
    return format("{} {} {} {}", p[0], p[1], p[2], p[3]);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a glyph fills its outline") {
    const PrFont* font = load("NotoSans[wdth,wght].ttf");
    uint16_t h = prFontCodePointGlyph(font, 'H');
    PrRect bounds = prFontGlyphBounds(font, h);
    PrFontGlyph glyph{h, {10, 90}};
    PrFontGlyphRun run{};
    run.font = font;
    run.size = 100;
    run.glyphs = &glyph;
    run.glyphCount = 1;
    PrDrawList* list = prDrawListCreate();
    prDrawListGlyphs(list, &run, PrVec2{}, {.color = PrColor{0, 0, 1, 1}});
    Array<uint8_t> pixels = render(list, 100);
    // Checks a pixel inside the left stem, and a pixel in the counter between the stems above the bar.
    uint32_t stem = uint32_t(10 + bounds.left * 100 + 4);
    CHECK(pixel(pixels, 100, stem, 40) == "0 0 255 255");
    CHECK(pixel(pixels, 100, uint32_t(10 + (bounds.left + bounds.right) * 50), 40) == "0 0 0 0");
    prDrawListRelease(list);
    prFontRelease(font);
}

TEST_CASE("a color glyph paints its own colors, times the tint's alpha") {
    // The test font's sweep: linen from 0 to 90 degrees, counterclockwise, and dark slate gray from 270 to 360, around
    // (0.5, -0.6) ems.
    const PrFont* font = load("ColrTestGlyphs.ttf");
    PrFontGlyph glyph{12, {0, 100}};
    PrFontGlyphRun run{};
    run.font = font;
    run.size = 100;
    run.glyphs = &glyph;
    run.glyphCount = 1;
    PrDrawList* list = prDrawListCreate();
    prDrawListGlyphs(list, &run, PrVec2{}, {.color = PrColor{0, 0, 0, 1}});
    Array<uint8_t> pixels = render(list, 100);
    CHECK(pixel(pixels, 100, 70, 20) == "250 240 230 255");
    CHECK(pixel(pixels, 100, 70, 60) == "47 79 79 255");
    prDrawListClear(list);
    prDrawListGlyphs(list, &run, PrVec2{}, {.color = PrColor{0, 0, 0, 0.5f}});
    pixels = render(list, 100);
    CHECK(pixel(pixels, 100, 70, 60) == "24 40 40 128");
    prDrawListRelease(list);
    prFontRelease(font);
}

TEST_CASE("a bitmap glyph draws its strike's image, and nothing without a codec") {
    PrCompressCodecSet* compressCodec = prCompressCodecSetCreateDefault();
    PrImageCodecSet* imageCodec = prImageCodecSetCreateDefault(compressCodec);
    struct Case {
        const char* font;
        uint32_t codePoint;
    } cases[] = {{"TwemojiSmileySbix.ttf", 0}, {"NotoColorEmoji.subset.multiple_size_tables.ttf", '9'}};
    for (const Case& c : cases) {
        CAPTURE(c.font);
        const PrFont* font = load(c.font);
        uint16_t id = c.codePoint != 0 ? prFontCodePointGlyph(font, c.codePoint) : 2;
        PrFontGlyphImage image{};
        REQUIRE(prFontGlyphImage(font, id, 64, &image));
        // The glyph at 64 pixels per em, with its image's center at the target's center.
        PrFontGlyph glyph{
            id, {
                40 - (image.bounds.left + image.bounds.right) * 32,
                40 - (image.bounds.top + image.bounds.bottom) * 32,
            },
        };
        PrFontGlyphRun run{};
        run.font = font;
        run.size = 64;
        run.glyphs = &glyph;
        run.glyphCount = 1;
        PrDrawList* list = prDrawListCreate();
        prDrawListGlyphs(list, &run, PrVec2{}, {.color = PrColor{0, 0, 0, 1}});
        // The glyph is opaque somewhere, and is drawn in its own colors instead of the black tint.
        auto most = [](const Array<uint8_t>& bytes, uint32_t channel) {
            uint8_t m = 0;
            for (uint32_t i = channel; i < bytes.count(); i += 4)
                m = bytes[i] > m ? bytes[i] : m;
            return m;
        };
        Array<uint8_t> drawn = render(list, 80, imageCodec);
        CHECK(most(drawn, 3) == 255);
        CHECK(most(drawn, 0) > 200);
        Array<uint8_t> none = render(list, 80);
        CHECK(most(none, 3) == 0);
        prDrawListRelease(list);
        prFontRelease(font);
    }
    prImageCodecSetDestroy(imageCodec);
    prCompressCodecSetDestroy(compressCodec);
}

TEST_CASE("a bitmap glyph that mirrors another glyph's image draws it reflected") {
    // Apple Color Emoji mirrors the images of some glyphs for others. It is the system's, and a machine may lack it.
    BlobRef file = BlobRef::adopt(prFileOpen("/System/Library/Fonts/Apple Color Emoji.ttc"));
    if (file.get() == nullptr) {
        MESSAGE("skipped: this machine has no Apple Color Emoji");
        return;
    }
    const PrFont* font = nullptr;
    REQUIRE(prFontCreate(file, 0, nullptr, &font) == PrStatus_Ok);
    PrFontGlyphImage image{};
    uint16_t glyph = 0;
    while (glyph < prFontGlyphCount(font) && !(prFontGlyphImage(font, glyph, 64, &image) && image.mirrored))
        ++glyph;
    REQUIRE(glyph < prFontGlyphCount(font));
    PrCompressCodecSet* compressCodec = prCompressCodecSetCreateDefault();
    PrImageCodecSet* imageCodec = prImageCodecSetCreateDefault(compressCodec);
    const PrTextureSource* decoded = nullptr;
    REQUIRE(prImageTextureSourceOpenBytes(image.bytes, imageCodec, nullptr, PrImageTextureColor_Srgb, &decoded) ==
        PrStatus_Ok);
    // The glyph at its strike's size, a pixel of the image to a pixel of the target, with the image at the origin.
    const uint32_t width = image.extent.x;
    const uint32_t height = image.extent.y;
    PrDrawList* list = prDrawListCreate();
    prDrawListPushTransformAffine2(list, PrAffine2{
        image.pixelsPerEm, 0, 0, image.pixelsPerEm,
        -image.bounds.left * image.pixelsPerEm, -image.bounds.top * image.pixelsPerEm,
    });
    PrDrawGlyphDecoder* decoder = prSvgGlyphDecoderCreate(imageCodec, nullptr, harnessMemoryTextures());
    REQUIRE(prDrawGlyphDecoderDecode(decoder, font, glyph, 64, nullptr, PrColor{0, 0, 0, 1}, list) ==
        PrDrawGlyphDecoded_Kept);
    prDrawGlyphDecoderDestroy(decoder);
    prDrawListPopTransform(list);
    PrDrawList* plain = prDrawListCreate();
    const PrRect whole{0, 0, float(width), float(height)};
    PrDrawBrush brush = prDrawBrushImage(harnessMemoryImage(decoded, whole, PrTextureCacheFlag_Levels), whole,
        PrDrawSampling_Mipmap);
    prDrawListRect(plain, whole, {.brush = &brush});
    const uint32_t size = width > height ? width : height;
    const Array<uint8_t> drawn = render(list, size);
    const Array<uint8_t> unmirrored = render(plain, size);
    // Each pixel is that of the image across the vertical line through its middle.
    uint32_t differing = 0;
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const uint8_t* a = drawn.data() + (size_t(y) * size + x) * 4;
            const uint8_t* b = unmirrored.data() + (size_t(y) * size + (width - 1 - x)) * 4;
            for (uint32_t k = 0; k < 4; ++k)
                differing += (a[k] > b[k] ? a[k] - b[k] : b[k] - a[k]) > 2 ? 1 : 0;
        }
    }
    CHECK(differing == 0);
    prDrawListRelease(plain);
    prDrawListRelease(list);
    prTextureSourceRelease(decoded);
    prImageCodecSetDestroy(imageCodec);
    prCompressCodecSetDestroy(compressCodec);
    prFontRelease(font);
}
