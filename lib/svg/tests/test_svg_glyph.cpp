#include "pr/array.h"
#include "pr/checksum.h"
#include "pr/compress_builtin.h"
#include "pr/draw_cpu.h"
#include "pr/float.h"
#include "pr/image_builtin.h"
#include "pr/string.h"
#include "pr/svg_glyph.h"
#include "pr/vec.h"

#include "tools/harness/test.h"

extern "C" int prTestSvgGlyphCApi(void);

namespace pr_impl {

using namespace pr;

namespace {

// Returns the bytes of the font file `name`, in the fonts directory.
Array<uint8_t> fontFile(const char* name) {
    return harness::readFile(format("{}/{}", PR_FONTS_DIR, name));
}

const PrFont* load(const char* name) {
    Array<uint8_t> bytes = fontFile(name);
    const PrFont* font = nullptr;
    REQUIRE(prFontCreate(bytes, 0, nullptr, &font) == PrStatus_Ok);
    return font;
}

void put16(Array<uint8_t>& out, uint32_t value) {
    out.push(uint8_t(value >> 8));
    out.push(uint8_t(value));
}

void put32(Array<uint8_t>& out, uint32_t value) {
    put16(out, value >> 16);
    put16(out, value & 0xffff);
}

// Returns the bytes as a gzip member of stored deflate blocks, which needs no compressor.
Array<uint8_t> gzipStored(const Array<uint8_t>& bytes) {
    Array<uint8_t> out({0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 0xff});
    for (uint32_t at = 0; at < bytes.count() || at == 0; at += 65535) {
        uint32_t length = bytes.count() - at < 65535 ? bytes.count() - at : 65535;
        out.push(at + length >= bytes.count() ? 1 : 0);
        out.push(uint8_t(length));
        out.push(uint8_t(length >> 8));
        out.push(uint8_t(~length));
        out.push(uint8_t(~length >> 8));
        out.append(bytes.data() + at, length);
        if (bytes.isEmpty())
            break;
    }
    uint32_t crc = prCrc32Update(0, bytes);
    uint32_t size = bytes.count();
    const uint32_t trailer[] = {crc, size};
    for (uint32_t value : trailer) {
        for (int k = 0; k < 4; ++k)
            out.push(uint8_t(value >> (8 * k)));
    }
    return out;
}

// Returns the font file with its SVG table replaced by a table that holds `document` for glyph `glyph` alone.
Array<uint8_t> withSvgDocument(Array<uint8_t> font, uint16_t glyph, const Array<uint8_t>& document) {
    Array<uint8_t> table;
    put16(table, 0);
    put32(table, 10);
    put32(table, 0);
    put16(table, 1);
    put16(table, glyph);
    put16(table, glyph);
    put32(table, 14);
    put32(table, document.count());
    table.append(document.span());
    while (font.count() % 4 != 0)
        font.push(0);
    uint32_t offset = font.count();
    font.append(table.span());
    uint32_t tables = uint32_t(font[4]) << 8 | font[5];
    for (uint32_t i = 0; i < tables; ++i) {
        uint8_t* record = font.data() + 12 + i * 16;
        if (PrStringView(reinterpret_cast<const char*>(record), 4) != "SVG ")
            continue;
        for (uint32_t k = 0; k < 4; ++k) {
            record[8 + k] = uint8_t(offset >> (24 - 8 * k));
            record[12 + k] = uint8_t(table.count() >> (24 - 8 * k));
        }
    }
    return font;
}

constexpr uint32_t kSize = 100;

Array<uint8_t> render(const PrDrawList* list) {
    Array<uint8_t> bytes(kSize * kSize * 4);
    PrDrawRenderer* renderer = prDrawCpuCreate(nullptr);
    PrPixels into{bytes.data(), size_t(kSize) * 4, {kSize, kSize}};
    REQUIRE(prDrawRendererRenderPixels(renderer, list, into, PrPixelFormat_Rgba8) == PrStatus_Ok);
    prDrawRendererDestroy(renderer);
    return bytes;
}

// The coverage of the pixels, and the mean color of the pixels that are mostly opaque.
struct Ink {
    double coverage;
    double r;
    double g;
    double b;
};

Ink inkOf(const Array<uint8_t>& pixels) {
    Ink ink{0, 0, 0, 0};
    double opaque = 0;
    for (uint32_t i = 0; i < pixels.count(); i += 4) {
        ink.coverage += pixels[i + 3] / 255.0;
        if (pixels[i + 3] < 250)
            continue;
        ink.r += pixels[i];
        ink.g += pixels[i + 1];
        ink.b += pixels[i + 2];
        ++opaque;
    }
    if (opaque > 0) {
        ink.r /= opaque;
        ink.g /= opaque;
        ink.b /= opaque;
    }
    return ink;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("the glyph decoder's header compiles as C") {
    CHECK(prTestSvgGlyphCApi() == 0);
}

TEST_CASE("an SVG glyph draws as the same glyph's color layers do, in the same palette") {
    // Nabla carries its color glyphs as both COLR layers and SVG documents, which draw alike. The colors in its
    // documents are var(--colorN), with the default palette's colors as fallbacks. An override of every entry therefore
    // shows that the palette reaches the document.
    const PrFont* font = load("Nabla.ttf");
    uint16_t glyph = prFontCodePointGlyph(font, 'A');
    PrFontGlyphSvg svg{};
    REQUIRE(prFontGlyphSvg(font, glyph, &svg));
    Array<PrFontPaletteColor> overrides;
    for (uint32_t i = 0; i < prFontPaletteEntryCount(font); ++i)
        overrides.push(PrFontPaletteColor{i, PrColor{0.1f, 0.3f + 0.6f * float(i % 2), 0.9f, 1}});
    PrDrawGlyphDecoder* decoder = prSvgGlyphDecoderCreate(nullptr, nullptr);
    const bool recolorings[] = {false, true};
    for (bool recolored : recolorings) {
        CAPTURE(recolored);
        PrFontPalette palette{0, recolored ? overrides.data() : nullptr, recolored ? overrides.count() : 0};
        PrFontGlyph placed{glyph, PrVec2{10, 85}};
        PrFontGlyphRun run{};
        run.font = font;
        run.size = 70;
        run.glyphs = &placed;
        run.glyphCount = 1;
        run.palette = palette;
        PrDrawList* layers = prDrawListCreate();
        prDrawListGlyphs(layers, &run, PrVec2{}, {.color = PrColor{0, 0, 0, 1}});
        Ink expected = inkOf(render(layers));
        REQUIRE(expected.coverage > 100);

        PrDrawList* document = prDrawListCreate();
        prDrawListPushTransformAffine2(document, prDrawGlyphTransform(&run, 0));
        REQUIRE(prDrawGlyphDecoderDecode(decoder, font, glyph, 70, &palette, PrColor{0, 0, 0, 1}, document));
        prDrawListPopTransform(document);
        Ink drawn = inkOf(render(document));
        CHECK(drawn.coverage == doctest::Approx(expected.coverage).epsilon(0.05));
        CHECK(prF64Abs(drawn.r - expected.r) < 20);
        CHECK(prF64Abs(drawn.g - expected.g) < 20);
        CHECK(prF64Abs(drawn.b - expected.b) < 20);
        if (recolored)
            CHECK(drawn.b > 150);
        prDrawListRelease(document);
        prDrawListRelease(layers);
    }
    prDrawGlyphDecoderDestroy(decoder);
    prFontRelease(font);
}

TEST_CASE("a glyph with no document is an image, and without an image codec nothing") {
    const PrFont* sans = load("NotoSans[wdth,wght].ttf");
    PrDrawGlyphDecoder* decoder = prSvgGlyphDecoderCreate(nullptr, nullptr);
    PrDrawList* list = prDrawListCreate();
    CHECK_FALSE(prDrawGlyphDecoderDecode(decoder, sans, prFontCodePointGlyph(sans, 'A'), 70, nullptr,
        PrColor{0, 0, 0, 1}, list));
    CHECK(prDrawListCommandCount(list) == 0);
    prDrawGlyphDecoderDestroy(decoder);
    // With an image codec, a bitmap glyph draws its strike's image.
    PrCompressCodec* compressCodec = prCompressBuiltinCreate();
    PrImageCodec* imageCodec = prImageBuiltinCreate(compressCodec);
    decoder = prSvgGlyphDecoderCreate(imageCodec, compressCodec);
    const PrFont* smiley = load("TwemojiSmileySbix.ttf");
    CHECK(prDrawGlyphDecoderDecode(decoder, smiley, 2, 64, nullptr, PrColor{0, 0, 0, 1}, list));
    CHECK(prDrawListCommandCount(list) != 0);
    prDrawListRelease(list);
    prDrawGlyphDecoderDestroy(decoder);
    prImageCodecDestroy(imageCodec);
    prCompressCodecDestroy(compressCodec);
    prFontRelease(smiley);
    prFontRelease(sans);
}

TEST_CASE("a gzip-compressed document draws as it does uncompressed, and not without a compress codec") {
    Array<uint8_t> file = fontFile("Nabla.ttf");
    const PrFont* font = load("Nabla.ttf");
    uint16_t glyph = prFontCodePointGlyph(font, 'A');
    PrFontGlyphSvg svg{};
    REQUIRE(prFontGlyphSvg(font, glyph, &svg));
    Array<uint8_t> document;
    document.append(svg.document.data, uint32_t(svg.document.size));
    Array<uint8_t> compressed = gzipStored(document);
    Array<uint8_t> bytes = withSvgDocument(file, glyph, compressed);
    const PrFont* gzipped = nullptr;
    REQUIRE(prFontCreate(bytes, 0, nullptr, &gzipped) == PrStatus_Ok);
    PrFontGlyphSvg packed{};
    REQUIRE(prFontGlyphSvg(gzipped, glyph, &packed));
    CHECK(packed.document.data[0] == 0x1f);

    PrCompressCodec* compressCodec = prCompressBuiltinCreate();
    PrDrawGlyphDecoder* decoder = prSvgGlyphDecoderCreate(nullptr, compressCodec);
    PrDrawGlyphDecoder* plain = prSvgGlyphDecoderCreate(nullptr, nullptr);
    PrAffine2 place = prAffine2Concat(prAffine2Scale(PrVec2{70, 70}), prAffine2Translate(PrVec2{10, 85}));
    auto draw = [&](PrDrawGlyphDecoder* with, const PrFont* from, bool* decoded) {
        PrDrawList* list = prDrawListCreate();
        prDrawListPushTransformAffine2(list, place);
        *decoded = prDrawGlyphDecoderDecode(with, from, glyph, 70, nullptr, PrColor{0, 0, 0, 1}, list);
        prDrawListPopTransform(list);
        Ink ink = inkOf(render(list));
        prDrawListRelease(list);
        return ink;
    };
    bool decoded = false;
    Ink expected = draw(decoder, font, &decoded);
    REQUIRE(decoded);
    Ink drawn = draw(decoder, gzipped, &decoded);
    REQUIRE(decoded);
    CHECK(drawn.coverage == doctest::Approx(expected.coverage).epsilon(0.001));
    draw(plain, gzipped, &decoded);
    CHECK_FALSE(decoded);
    prDrawGlyphDecoderDestroy(plain);
    prDrawGlyphDecoderDestroy(decoder);
    prCompressCodecDestroy(compressCodec);
    prFontRelease(gzipped);
    prFontRelease(font);
}
