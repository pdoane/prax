#include "pr/draw_shared_atlas.h"

#include "doctest/doctest.h"

#include <cstdio>
#include <string>
#include <vector>

extern "C" int prTestDrawSharedCApi(void);

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

PrFontGlyphRun runOf(const PrFont* font, const PrFontGlyph* glyphs, uint32_t count, float size) {
    PrFontGlyphRun run{};
    run.font = font;
    run.size = size;
    run.glyphs = glyphs;
    run.glyphCount = count;
    return run;
}

// Returns the total coverage of a glyph's texels.
double inked(const PrDrawAtlas* atlas, const PrDrawAtlasEntry& g) {
    PrPixelView texels = prDrawAtlasPageTexels(atlas, g.page);
    double sum = 0;
    for (uint32_t y = 0; y < g.extent.y; ++y)
        for (uint32_t x = 0; x < g.extent.x; ++x)
            sum += texels.data[size_t(g.texel.y + y) * texels.stride + g.texel.x + x] / 255.0;
    return sum;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("the public headers compile as C") {
    CHECK(prTestDrawSharedCApi() == 0);
}

TEST_CASE("the atlas rasterizes a glyph once for each quarter pixel and places it on the pixel grid") {
    const PrFont* font = load("NotoSans.ttf");
    PrDrawAtlasOptions options{256, 2, 4, 0.05f};
    PrDrawAtlas* atlas = prDrawAtlasCreate(&options);
    prDrawAtlasNext(atlas);
    PrIRect region{};
    REQUIRE(prDrawAtlasPageCount(atlas) == 1);
    // The solid block is full coverage.
    CHECK(prDrawAtlasPageTexels(atlas, 0).data[0] == 255);
    CHECK(prDrawAtlasTakeChanges(atlas, 0, &region));
    CHECK(prIRectWidth(region) == 256);
    CHECK_FALSE(prDrawAtlasTakeChanges(atlas, 0, &region));

    uint16_t h = prFontCodePointGlyph(font, 'H');
    PrFontGlyph glyphs[3] = {{h, {10, 40}}, {h, {50.02f, 40}}, {h, {90.5f, 40.3f}}};
    PrFontGlyphRun run = runOf(font, glyphs, 3, 24);
    PrDrawAtlasEntry a{};
    PrDrawAtlasEntry b{};
    PrDrawAtlasEntry c{};
    REQUIRE(prDrawAtlasGlyph(atlas, &run, 0, prAffine2Identity(), &a));
    REQUIRE(prDrawAtlasGlyph(atlas, &run, 1, prAffine2Identity(), &b));
    REQUIRE(prDrawAtlasGlyph(atlas, &run, 2, prAffine2Identity(), &c));
    CHECK(a.extent.x > 0);
    // A glyph at the same phase shares texels, and is placed 40 pixels over.
    CHECK(b.texel.x == a.texel.x);
    CHECK(b.texel.y == a.texel.y);
    CHECK(b.origin.x == a.origin.x + 40);
    CHECK(b.origin.y == a.origin.y);
    // A glyph half a pixel over has another phase.
    CHECK((c.texel.x != a.texel.x || c.texel.y != a.texel.y));
    CHECK(inked(atlas, c) == doctest::Approx(inked(atlas, a)).epsilon(0.02));
    REQUIRE(prDrawAtlasTakeChanges(atlas, 0, &region));
    CHECK(region.left <= int32_t(a.texel.x));
    CHECK(region.right >= int32_t(c.texel.x + c.extent.x));

    // A scale applies to the rasterized glyph and to the position where it is placed.
    PrDrawAtlasEntry scaled{};
    REQUIRE(prDrawAtlasGlyph(atlas, &run, 0, prAffine2Scale(PrVec2{2, 2}), &scaled));
    CHECK(scaled.extent.y >= 2 * a.extent.y - 2);
    CHECK(scaled.extent.y <= 2 * a.extent.y + 2);
    CHECK(scaled.origin.y >= 2 * a.origin.y - 2);
    CHECK(scaled.origin.y <= 2 * a.origin.y + 2);

    // Rotated or mirrored glyphs are not drawn from the atlas.
    CHECK_FALSE(prDrawAtlasGlyph(atlas, &run, 0, prAffine2Rotate(0.5f), &a));
    CHECK_FALSE(prDrawAtlasGlyph(atlas, &run, 0, prAffine2Scale(PrVec2{-1, 1}), &a));
    prDrawAtlasDestroy(atlas);
    prFontRelease(font);
}

TEST_CASE("the atlas empties a page no glyph was drawn from this use when it is full") {
    const PrFont* font = load("NotoSans.ttf");
    PrDrawAtlasOptions options{64, 1, 0, 0.05f};
    PrDrawAtlas* atlas = prDrawAtlasCreate(&options);
    PrFontGlyph glyphs[2] = {{prFontCodePointGlyph(font, 'W'), {0, 40}},
                             {prFontCodePointGlyph(font, 'M'), {0, 40}}};
    PrFontGlyphRun run = runOf(font, glyphs, 2, 44);
    PrDrawAtlasEntry g{};
    prDrawAtlasNext(atlas);
    REQUIRE(prDrawAtlasGlyph(atlas, &run, 0, prAffine2Identity(), &g));
    // The only page has no room beside the first glyph, and this use draws from that page.
    CHECK_FALSE(prDrawAtlasGlyph(atlas, &run, 1, prAffine2Identity(), &g));
    prDrawAtlasNext(atlas);
    REQUIRE(prDrawAtlasGlyph(atlas, &run, 1, prAffine2Identity(), &g));
    CHECK(g.texel.x == 1);
    prDrawAtlasDestroy(atlas);
    prFontRelease(font);
}

TEST_CASE("the atlas rasterizes a path's coverage once for each fill, stroke, scale and quarter pixel") {
    PrDrawAtlas* atlas = prDrawAtlasCreate(nullptr);
    prDrawAtlasNext(atlas);
    PrPath* path = prPathCreate();
    prPathMoveTo(path, PrVec2{4, 4});
    prPathLineTo(path, PrVec2{20, 12});
    prPathLineTo(path, PrVec2{4, 20});
    PrDrawStroke stroke = prDrawStrokeDefault();
    stroke.shape.width = 2;
    stroke.shape.join = PrPathJoin_Round;
    stroke.shape.startCap = stroke.shape.endCap = PrPathCap_Round;
    PrAffine2 at = prAffine2Concat(prAffine2Scale(PrVec2{0.75f, 0.75f}), prAffine2Translate(PrVec2{10, 30}));

    PrDrawAtlasEntry a{};
    REQUIRE(prDrawAtlasPath(atlas, path, PrPathFillRule_NonZero, &stroke, at, &a));
    CHECK(a.extent.x > 0);
    CHECK(prDrawAtlasInfo(atlas).rasterizations == 1);
    // A clone shares the path's id, and a different position on the pixel grid shares its texels.
    PrPath* clone = prPathClone(path);
    PrDrawAtlasEntry b{};
    PrAffine2 over = at;
    over.e += 40;
    REQUIRE(prDrawAtlasPath(atlas, clone, PrPathFillRule_NonZero, &stroke, over, &b));
    CHECK(prDrawAtlasInfo(atlas).rasterizations == 1);
    CHECK(b.texel.x == a.texel.x);
    CHECK(b.texel.y == a.texel.y);
    CHECK(b.origin.x == a.origin.x + 40);
    CHECK(b.origin.y == a.origin.y);

    // A different width, phase or scale, or a fill, makes a different entry.
    PrDrawStroke wide = stroke;
    wide.shape.width = 3;
    PrAffine2 half = at;
    half.e += 0.5f;
    PrAffine2 larger = prAffine2Concat(prAffine2Scale(PrVec2{1, 1}), prAffine2Translate(PrVec2{10, 30}));
    PrDrawAtlasEntry c{};
    REQUIRE(prDrawAtlasPath(atlas, path, PrPathFillRule_NonZero, &wide, at, &c));
    CHECK(prDrawAtlasInfo(atlas).rasterizations == 2);
    CHECK(inked(atlas, c) > inked(atlas, a));
    REQUIRE(prDrawAtlasPath(atlas, path, PrPathFillRule_NonZero, &stroke, half, &c));
    CHECK(prDrawAtlasInfo(atlas).rasterizations == 3);
    CHECK(inked(atlas, c) == doctest::Approx(inked(atlas, a)).epsilon(0.02));
    REQUIRE(prDrawAtlasPath(atlas, path, PrPathFillRule_NonZero, &stroke, larger, &c));
    CHECK(prDrawAtlasInfo(atlas).rasterizations == 4);
    REQUIRE(prDrawAtlasPath(atlas, path, PrPathFillRule_NonZero, nullptr, at, &c));
    CHECK(prDrawAtlasInfo(atlas).rasterizations == 5);
    CHECK(prDrawAtlasInfo(atlas).entryCount == 5);

    // A change to the path gives it a new id, and so a new entry.
    prPathLineTo(clone, PrVec2{12, 4});
    CHECK(prPathId(clone) != prPathId(path));
    REQUIRE(prDrawAtlasPath(atlas, clone, PrPathFillRule_NonZero, &stroke, at, &c));
    CHECK(prDrawAtlasInfo(atlas).rasterizations == 6);

    // Rotated, mirrored or large paths are not drawn from the atlas.
    CHECK_FALSE(prDrawAtlasPath(atlas, path, PrPathFillRule_NonZero, &stroke, prAffine2Rotate(0.5f), &c));
    CHECK_FALSE(prDrawAtlasPath(atlas, path, PrPathFillRule_NonZero, &stroke, prAffine2Scale(PrVec2{-1, 1}), &c));
    CHECK_FALSE(prDrawAtlasPath(atlas, path, PrPathFillRule_NonZero, &stroke, prAffine2Scale(PrVec2{10, 10}), &c));
    CHECK(prDrawAtlasInfo(atlas).rasterizations == 6);
    prPathDestroy(clone);
    prPathDestroy(path);
    prDrawAtlasDestroy(atlas);
}

TEST_CASE("the atlas keeps the paths found since the last use and empties pages of others when it is full") {
    PrDrawAtlasOptions options{64, 1, 0, 0.05f};
    PrDrawAtlas* atlas = prDrawAtlasCreate(&options);
    PrPath* square = prPathCreate();
    prPathAddRect(square, PrRect{0, 0, 40, 40});
    PrPath* other = prPathClone(square);
    prPathTransform(other, prAffine2Translate(PrVec2{1, 0}));
    PrDrawAtlasEntry e{};
    prDrawAtlasNext(atlas);
    REQUIRE(prDrawAtlasPath(atlas, square, PrPathFillRule_NonZero, nullptr, prAffine2Identity(), &e));
    CHECK_FALSE(prDrawAtlasPath(atlas, other, PrPathFillRule_NonZero, nullptr, prAffine2Identity(), &e));
    // The square is found again in the next use, so it keeps its page.
    prDrawAtlasNext(atlas);
    REQUIRE(prDrawAtlasPath(atlas, square, PrPathFillRule_NonZero, nullptr, prAffine2Identity(), &e));
    CHECK_FALSE(prDrawAtlasPath(atlas, other, PrPathFillRule_NonZero, nullptr, prAffine2Identity(), &e));
    CHECK(prDrawAtlasInfo(atlas).rasterizations == 1);
    // When the square is not found, it gives up its page.
    prDrawAtlasNext(atlas);
    REQUIRE(prDrawAtlasPath(atlas, other, PrPathFillRule_NonZero, nullptr, prAffine2Identity(), &e));
    CHECK(prDrawAtlasInfo(atlas).entryCount == 1);
    prPathDestroy(other);
    prPathDestroy(square);
    prDrawAtlasDestroy(atlas);
}
