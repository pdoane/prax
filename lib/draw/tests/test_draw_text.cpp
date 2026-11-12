#include "pr/array.h"
#include "pr/draw_glyph.h"
#include "pr/draw_lower.h"
#include "pr/string.h"
#include "pr/string_ops.h"
#include "pr/vec.h"

#include "draw/src/draw_glyph.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

using namespace pr;

const PrFont* load(const char* name) {
    Array<uint8_t> bytes = harness::readFile(format("{}/{}", PR_FONTS_DIR, name));
    const PrFont* font = nullptr;
    REQUIRE(prFontCreate(bytes, 0, nullptr, &font) == PrStatus_Ok);
    return font;
}

constexpr PrColor kBlack{0, 0, 0, 1};

String dump(const PrDrawList* list) {
    return String::adopt(prDrawListDump(list));
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a glyph run is recorded with copies of its glyphs and a reference to its stack") {
    const PrFont* font = load("NotoSans[wdth,wght].ttf");
    PrDrawList* list = prDrawListCreate();
    PrFontGlyph glyphs[] = {{prFontCodePointGlyph(font, 'H'), {10, 20}}, {prFontCodePointGlyph(font, 'i'), {18, 20}}};
    PrFontGlyphRun run{};
    run.font = font;
    run.size = 10;
    run.glyphs = glyphs;
    run.glyphCount = 2;
    prDrawListGlyphs(list, &run, PrVec2{}, {.color = kBlack});
    glyphs[0].position.x = 99;
    // No glyphs, no size, and no paint each record nothing.
    run.glyphCount = 0;
    prDrawListGlyphs(list, &run, PrVec2{}, {.color = kBlack});
    run.glyphCount = 2;
    run.size = 0;
    prDrawListGlyphs(list, &run, PrVec2{}, {.color = kBlack});
    run.size = 10;
    prDrawListGlyphs(list, &run, PrVec2{}, {.color = PrColor{0, 0, 0, 0}});
    prFontRelease(font);
    REQUIRE(prDrawListCommandCount(list) == 1);
    const PrDrawCommand& c = prDrawListCommands(list)[0];
    CHECK(c.kind == PrDrawCommandKind_Glyphs);
    CHECK(c.glyphs->glyphs[0].position.x == 10.0f);
    CHECK(prStringStartsWith(dump(list), "glyphs size 10 43 10 20 76 18 20 color 0 0 0 1"));
    // The list holds a reference to the stack.
    CHECK(prFontGlyphAdvance(c.glyphs->font, c.glyphs->glyphs[0].id, 0) > 0.0f);
    prDrawListRelease(list);
}

TEST_CASE("a glyph run is recorded with its glyphs placed from its origin") {
    const PrFont* font = load("NotoSans[wdth,wght].ttf");
    PrDrawList* list = prDrawListCreate();
    PrFontGlyph glyph{prFontCodePointGlyph(font, 'H'), {10, 20}};
    PrFontGlyphRun run{};
    run.font = font;
    run.size = 10;
    run.glyphs = &glyph;
    run.glyphCount = 1;
    prDrawListGlyphs(list, &run, PrVec2{5, -2}, {.color = kBlack});
    prFontRelease(font);
    REQUIRE(prDrawListCommandCount(list) == 1);
    const PrFontGlyphRun* recorded = prDrawListCommands(list)[0].glyphs;
    CHECK(recorded->glyphs[0].position.x == 15.0f);
    CHECK(recorded->glyphs[0].position.y == 18.0f);
    // The caller's glyphs are not moved.
    CHECK(glyph.position.x == 10.0f);
    prDrawListRelease(list);
}

TEST_CASE("a glyph is placed by its size, shear, transform and position") {
    const PrFont* font = load("NotoSans[wdth,wght].ttf");
    PrFontGlyph glyph{prFontCodePointGlyph(font, 'H'), {100, 50}};
    PrAffine2 turn = prAffine2Rotate(3.14159265f / 2);
    PrFontDerivation leaning = prFontDerivationDefault();
    leaning.oblique = 0.25f;
    const PrFont* oblique = prFontCreateDerived(font, &leaning);
    PrFontGlyphRun run{};
    run.font = oblique;
    run.size = 20;
    run.glyphs = &glyph;
    run.glyphCount = 1;
    PrAffine2 t = prDrawGlyphTransform(&run, 0);
    // A point one em up the glyph leans a quarter em right.
    PrVec2 up = prAffine2MapPoint(t, PrVec2{0, -1});
    CHECK(up.x == doctest::Approx(105));
    CHECK(up.y == doctest::Approx(30));
    prFontRelease(oblique);
    run.font = font;
    run.transforms = &turn;
    up = prAffine2MapPoint(prDrawGlyphTransform(&run, 0), PrVec2{0, -1});
    CHECK(up.x == doctest::Approx(120));
    CHECK(up.y == doctest::Approx(50).epsilon(1e-4));
    run.transforms = nullptr;
    PrPath* path = prPathCreate();
    CHECK(prDrawGlyphPath(&run, 0, nullptr, path));
    PrRect bounds = prPathBounds(path);
    PrFontMetrics m = prFontMetrics(font);
    CHECK(bounds.bottom == doctest::Approx(50));
    CHECK(bounds.top == doctest::Approx(50 - 20 * m.capHeight));
    // The run's bounds for a brush are its cells, from ascent to descent.
    PrDrawList* list = prDrawListCreate();
    prDrawListGlyphs(list, &run, PrVec2{}, {.color = kBlack});
    PrRect cells = prDrawFillBounds(prDrawListCommands(list));
    CHECK(cells.left == doctest::Approx(100));
    float advance = prFontGlyphAdvance(font, glyph.id, 0);
    CHECK(cells.right == doctest::Approx(100 + 20 * advance));
    CHECK(cells.top == doctest::Approx(50 - 20 * m.ascent));
    CHECK(cells.bottom == doctest::Approx(50 + 20 * m.descent));
    // The shape of the run is every glyph's outline.
    prPathClear(path);
    prDrawShapePath(prDrawListCommands(list), path);
    CHECK(prPathBounds(path).top == doctest::Approx(bounds.top));
    prDrawListRelease(list);
    prPathDestroy(path);
    prFontRelease(font);
}

TEST_CASE("a sweep turns counterclockwise from +x over the whole turn") {
    // The test font's sweep from 0 to 360 degrees, padded outside its stops at a quarter and three quarters of the
    // turn: linen, blue, red, dark slate gray.
    const PrFont* font = load("ColrTestGlyphs.ttf");
    PrDrawList* drawn = drawGlyphList(font, 12, nullptr, kBlack);
    REQUIRE(drawn != nullptr);
    const PrDrawBrush* brush = nullptr;
    for (uint32_t i = 0; i < prDrawListCommandCount(drawn); ++i)
        brush = brush != nullptr ? brush : prDrawListCommands(drawn)[i].brush;
    REQUIRE(brush != nullptr);
    CHECK(brush->kind == PrDrawBrushKind_Conic);
    CHECK(brush->conic.angle == 0.0f);
    // The brush is mirrored about its center, so that its clockwise turn becomes counterclockwise.
    CHECK(brush->transform.d == -1.0f);
    const float offsets[] = {0, 0.25f, 5.0f / 12, 7.0f / 12, 0.75f, 1};
    REQUIRE(brush->stopCount == 6);
    for (uint32_t i = 0; i < 6; ++i)
        CHECK(brush->stops[i].offset == doctest::Approx(offsets[i]).epsilon(1e-3));
    CHECK(prColorFromColor16(brush->stops[2].color).b == 1.0f);
    CHECK(prColorFromColor16(brush->stops[3].color).r == 1.0f);
    prDrawListRelease(drawn);
    // A glyph with no color layers has no list.
    CHECK(drawGlyphList(font, 0, nullptr, kBlack) == nullptr);
    prFontRelease(font);
}

TEST_CASE("a color glyph's fill under a transform covers the em box, mapped through the transform") {
    const PrFont* font = load("ColrTestGlyphs.ttf");
    uint32_t checked = 0;
    for (uint32_t g = 200; g < 230; ++g) {
        PrDrawList* drawn = drawGlyphList(font, uint16_t(g), nullptr, kBlack);
        if (drawn == nullptr)
            continue;
        const PrDrawCommand* commands = prDrawListCommands(drawn);
        Array<PrAffine2> transforms;
        transforms.push(prAffine2Identity());
        for (uint32_t i = 0; i < prDrawListCommandCount(drawn); ++i) {
            const PrDrawCommand& c = commands[i];
            if (c.kind == PrDrawCommandKind_PushTransformAffine2)
                transforms.push(prAffine2Concat(prAffine2FromDAffine2(c.affine), transforms.last()));
            else if (c.kind == PrDrawCommandKind_PopTransform)
                transforms.pop();
            if (c.kind != PrDrawCommandKind_Rect || transforms.count() == 1)
                continue;
            PrRect ems = prAffine2MapRect(transforms.last(), c.rect);
            CHECK(ems.left <= -15.99f);
            CHECK(ems.top <= -15.99f);
            CHECK(ems.right >= 15.99f);
            CHECK(ems.bottom >= 15.99f);
            ++checked;
        }
        prDrawListRelease(drawn);
    }
    CHECK(checked > 0);
    prFontRelease(font);
}
