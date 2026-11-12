#include "pr/array.h"
#include "pr/compress_builtin.h"
#include "pr/draw_cpu.h"
#include "pr/draw_glyph.h"
#include "pr/image_builtin.h"
#include "pr/string.h"
#include "pr/vec.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

using namespace pr;

void put16(Array<uint8_t>& out, uint32_t value) {
    out.push(uint8_t(value >> 8));
    out.push(uint8_t(value));
}

void put32(Array<uint8_t>& out, uint32_t value) {
    put16(out, value >> 16);
    put16(out, value & 0xffff);
}

// Returns a big-endian TIFF of one strip of opaque RGB pixels, `size` on a side, all of one color.
Array<uint8_t> solidTiff(uint32_t size, uint8_t r, uint8_t g, uint8_t b) {
    Array<uint8_t> out({'M', 'M', 0, 42});
    uint32_t pixels = size * size * 3;
    put32(out, 8 + pixels);
    for (uint32_t i = 0; i < size * size; ++i) {
        out.push(r);
        out.push(g);
        out.push(b);
    }
    const uint32_t entries[][3] = {
        {256, 4, size}, {257, 4, size}, {259, 3, 1}, {262, 3, 2},
        {273, 4, 8}, {277, 3, 3}, {278, 4, size}, {279, 4, pixels},
    };
    put16(out, 8);
    for (const uint32_t* entry : entries) {
        put16(out, entry[0]);
        put16(out, entry[1]);
        put32(out, 1);
        if (entry[1] == 3) {
            put16(out, entry[2]);
            put16(out, 0);
        } else {
            put32(out, entry[2]);
        }
    }
    put32(out, 0);
    return out;
}

// Returns the font file with its sbix table replaced by one strike of `ppem` pixels per em. In that strike, glyph
// `glyph` is `image`, of the sbix graphic type `type`, with its bottom left corner at the origin.
Array<uint8_t> withSbixImage(Array<uint8_t> font, uint16_t glyph, uint16_t glyphCount, uint32_t type,
        const Array<uint8_t>& image, uint16_t ppem) {
    Array<uint8_t> sbix;
    put16(sbix, 1);
    put16(sbix, 1);
    put32(sbix, 1);
    put32(sbix, 12);
    uint32_t data = 4 + (uint32_t(glyphCount) + 1) * 4;
    put16(sbix, ppem);
    put16(sbix, 72);
    for (uint32_t g = 0; g <= glyphCount; ++g)
        put32(sbix, g <= glyph ? data : data + 8 + image.count());
    put16(sbix, 0);
    put16(sbix, 0);
    put32(sbix, type);
    sbix.append(image.span());
    while (font.count() % 4 != 0)
        font.push(0);
    uint32_t offset = font.count();
    font.append(sbix.span());
    uint32_t tables = uint32_t(font[4]) << 8 | font[5];
    for (uint32_t i = 0; i < tables; ++i) {
        uint8_t* record = font.data() + 12 + i * 16;
        if (PrStringView{reinterpret_cast<const char*>(record), 4} != "sbix")
            continue;
        for (uint32_t k = 0; k < 4; ++k) {
            record[8 + k] = uint8_t(offset >> (24 - 8 * k));
            record[12 + k] = uint8_t(sbix.count() >> (24 - 8 * k));
        }
    }
    return font;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a TIFF image in an sbix strike draws through the image codec") {
    Array<uint8_t> file = harness::readFile(format("{}/TwemojiSmileySbix.ttf", PR_FONTS_DIR));
    const PrFont* original = nullptr;
    REQUIRE(prFontCreate(file, 0, nullptr, &original) == PrStatus_Ok);
    uint16_t glyphs = uint16_t(prFontGlyphCount(original));
    prFontRelease(original);
    Array<uint8_t> bytes = withSbixImage(file, 2, glyphs, 0x74696666, solidTiff(32, 0, 0, 255), 32);
    const PrFont* font = nullptr;
    REQUIRE(prFontCreate(bytes, 0, nullptr, &font) == PrStatus_Ok);

    PrCompressCodec* compressCodec = prCompressBuiltinCreate();
    PrImageCodec* imageCodec = prImageBuiltinCreate(compressCodec);
    PrDrawGlyphDecoder* decoder = prDrawGlyphBitmapCreate(imageCodec);
    PrDrawCpuOptions options = prDrawCpuOptionsDefault();
    options.glyphDecoder = decoder;
    PrDrawRenderer* renderer = prDrawCpuCreate(&options);
    // The image's 32 pixels at 32 pixels per em fill an em above the baseline: 40 to 72 across, 8 to 40 down.
    PrFontGlyph glyph{2, PrVec2{40, 40}};
    PrFontGlyphRun run{};
    run.font = font;
    run.size = 32;
    run.glyphs = &glyph;
    run.glyphCount = 1;
    PrDrawList* list = prDrawListCreate();
    prDrawListGlyphs(list, &run, PrVec2{}, {.color = PrColor{0, 0, 0, 1}});
    Array<uint8_t> pixels(80 * 80 * 4);
    REQUIRE(prDrawRendererRenderPixels(renderer, list, PrPixels{pixels.data(), 80 * 4, {80, 80}},
        PrPixelFormat_Rgba8) == PrStatus_Ok);
    const uint8_t* inside = pixels.data() + (size_t(24) * 80 + 56) * 4;
    CHECK(inside[2] == 255);
    CHECK(inside[3] == 255);
    const uint8_t* outside = pixels.data() + (size_t(60) * 80 + 20) * 4;
    CHECK(outside[3] == 0);
    prDrawListRelease(list);
    prDrawRendererDestroy(renderer);
    prDrawGlyphDecoderDestroy(decoder);
    prImageCodecDestroy(imageCodec);
    prCompressCodecDestroy(compressCodec);
    prFontRelease(font);
}
