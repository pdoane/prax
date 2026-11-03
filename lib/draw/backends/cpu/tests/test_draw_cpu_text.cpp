#include "pr/compress_builtin.h"
#include "pr/draw_cpu.h"
#include "pr/draw_glyph.h"
#include "pr/image_builtin.h"

#include "doctest/doctest.h"

#include <cstdio>
#include <string>
#include <vector>

namespace pr_impl {

namespace {

const PrFont* load(const char* name) {
    std::string path = std::string(PR_FONTS_DIR) + "/" + name;
    std::vector<uint8_t> bytes;
    FILE* file = std::fopen(path.c_str(), "rb");
    REQUIRE(file != nullptr);
    uint8_t buffer[65536];
    size_t read = 0;
    while ((read = std::fread(buffer, 1, sizeof(buffer), file)) != 0)
        bytes.insert(bytes.end(), buffer, buffer + read);
    std::fclose(file);
    const PrFont* font = nullptr;
    REQUIRE(prFontCreate(bytes, 0, nullptr, &font) == PrStatus_Ok);
    return font;
}

// Renders the list onto `size` by `size` transparent RGBA pixels, reading images with the decoders if they are given.
std::vector<uint8_t> render(const PrDrawList* list, uint32_t size, PrImageCodec* imageCodec = nullptr) {
    std::vector<uint8_t> bytes(size_t(size) * size * 4, 0);
    PrDrawCpuOptions options = prDrawCpuOptionsDefault();
    PrDrawGlyphDecoder* decoder = imageCodec != nullptr ? prDrawGlyphBitmapCreate(imageCodec) : nullptr;
    options.glyphDecoder = decoder;
    PrDrawRenderer* renderer = prDrawCpuCreate(&options);
    PrPixels into{bytes.data(), size_t(size) * 4, {size, size}};
    REQUIRE(prDrawRendererRenderPixels(renderer, list, into, PrPixelFormat_Rgba8) == PrStatus_Ok);
    prDrawRendererDestroy(renderer);
    prDrawGlyphDecoderDestroy(decoder);
    return bytes;
}

std::string pixel(const std::vector<uint8_t>& bytes, uint32_t size, uint32_t x, uint32_t y) {
    const uint8_t* p = bytes.data() + (size_t(y) * size + x) * 4;
    return std::to_string(p[0]) + " " + std::to_string(p[1]) + " " + std::to_string(p[2]) + " " + std::to_string(p[3]);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a glyph fills its outline") {
    const PrFont* font = load("NotoSans.ttf");
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
    std::vector<uint8_t> pixels = render(list, 100);
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
    std::vector<uint8_t> pixels = render(list, 100);
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
    PrCompressCodec* compressCodec = prCompressBuiltinCreate();
    PrImageCodec* imageCodec = prImageBuiltinCreate(compressCodec);
    struct Case {
        const char* font;
        uint32_t codePoint;
    } cases[] = {{"TwemojiSmileySbix.ttf", 0}, {"NotoColorEmojiCbdt.ttf", '9'}};
    for (const Case& c : cases) {
        CAPTURE(std::string(c.font));
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
        PrFontGlyphRun run{};        run.font = font;
        run.size = 64;
        run.glyphs = &glyph;
        run.glyphCount = 1;
        PrDrawList* list = prDrawListCreate();
        prDrawListGlyphs(list, &run, PrVec2{}, {.color = PrColor{0, 0, 0, 1}});
        // The glyph is opaque somewhere, and is drawn in its own colors instead of the black tint.
        auto most = [](const std::vector<uint8_t>& bytes, uint32_t channel) {
            uint8_t m = 0;
            for (size_t i = channel; i < bytes.size(); i += 4)
                m = bytes[i] > m ? bytes[i] : m;
            return m;
        };
        std::vector<uint8_t> drawn = render(list, 80, imageCodec);
        CHECK(most(drawn, 3) == 255);
        CHECK(most(drawn, 0) > 200);
        std::vector<uint8_t> none = render(list, 80);
        CHECK(most(none, 3) == 0);
        prDrawListRelease(list);
        prFontRelease(font);
    }
    prImageCodecDestroy(imageCodec);
    prCompressCodecDestroy(compressCodec);
}
