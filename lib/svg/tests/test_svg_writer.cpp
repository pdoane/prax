// lib/draw's SVG writer read back by lib/svg: a list written as SVG and drawn again renders as the list does.

#include "pr/draw_svg.h"
#include "pr/font.h"

#include "svg/tests/svg_fixture.h"

#include <cstdlib>

namespace pr_impl {

namespace {

constexpr uint32_t kSide = 64;

void collect(void* PR_NULLABLE context, uint32_t output, PrByteView bytes) {
    CHECK(output == 0);
    static_cast<std::string*>(context)->append(reinterpret_cast<const char*>(bytes.data), bytes.size);
}

std::string writeSvg(const PrDrawList* list) {
    PrDrawWriter* writer = prDrawSvgCreate(nullptr);
    std::string out;
    PrDrawDocumentDesc desc{.write = collect, .context = &out};
    PrDrawPage page{.list = list, .extent = PrVec2{float(kSide), float(kSide)}};
    prDrawWriterWrite(writer, &desc, &page, 1);
    prDrawWriterDestroy(writer);
    return out;
}

// Renders the list on the CPU over white.
std::vector<uint8_t> render(const PrDrawList* list) {
    std::vector<uint8_t> bytes(size_t(kSide) * kSide * 4, 255);
    PrDrawRenderer* renderer = prDrawCpuCreate(nullptr);
    prDrawRendererRenderPixels(renderer, list, PrPixels{bytes.data(), size_t(kSide) * 4, {kSide, kSide}},
        PrPixelFormat_Rgba8);
    prDrawRendererDestroy(renderer);
    return bytes;
}

// Returns the largest difference of a channel between the list and its SVG drawn again.
int roundTrip(const PrDrawList* list) {
    std::string svg = writeSvg(list);
    Drawing drawing(svg, PrVec2{float(kSide), float(kSide)});
    std::vector<uint8_t> direct = render(list);
    std::vector<uint8_t> again = render(drawing.list);
    int largest = 0;
    for (size_t i = 0; i < direct.size(); ++i)
        largest = std::max(largest, std::abs(int(direct[i]) - int(again[i])));
    if (largest > 2)
        MESSAGE(svg);
    return largest;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("shapes, strokes and transforms read back as they were drawn") {
    PrDrawList* list = prDrawListCreate();
    prDrawListRect(list, PrRect{4, 4, 30, 20}, {.color = PrColor{1, 0, 0, 1}});
    prDrawListEllipse(list, PrVec2{40, 40}, PrVec2{12, 8}, 0.3f, {.color = PrColor{0, 0.5f, 1, 0.75f}});
    PrDrawStroke stroke = prDrawStrokeDefault();
    stroke.shape.width = 3;
    stroke.shape.join = PrPathJoin_Round;
    stroke.shape.startCap = stroke.shape.endCap = stroke.shape.dashCap = PrPathCap_Round;
    prDrawListPushTransformAffine2(list, PrAffine2{1, 0.2f, -0.1f, 1, 6, 30});
    prDrawListLine(list, PrVec2{0, 0}, PrVec2{40, 20}, {.color = PrColor{0, 0, 0, 1}, .stroke = &stroke});
    prDrawListPopTransform(list);
    CHECK(roundTrip(list) <= 2);
    prDrawListRelease(list);
}

TEST_CASE("clips, groups and gradients read back as they were drawn") {
    PrDrawList* list = prDrawListCreate();
    PrDrawColorStop stops[3] = {
        {.offset = 0, .color = PrColor{1, 0, 0, 1}},
        {.offset = 0.5f, .color = PrColor{0, 1, 0, 1}},
        {.offset = 1, .color = PrColor{0, 0, 1, 1}},
    };
    PrDrawBrush linear = prDrawBrushLinear(PrVec2{0, 0}, PrVec2{64, 0}, stops, 3);
    PrPath* circle = prPathCreate();
    prPathAddEllipse(circle, PrVec2{32, 32}, PrVec2{28, 28}, 0);
    prDrawListPushClipPath(list, circle, PrPathFillRule_NonZero, PrDrawEdges_Antialiased);
    prDrawListRect(list, PrRect{0, 0, 64, 64}, {.brush = &linear});
    PrDrawGroup group = prDrawGroupDefault();
    group.opacity = 0.5f;
    prDrawListPushGroup(list, &group);
    prDrawListRect(list, PrRect{10, 10, 40, 40}, {.color = PrColor{0, 0, 0, 1}});
    prDrawListRect(list, PrRect{20, 20, 50, 50}, {.color = PrColor{1, 1, 1, 1}});
    prDrawListPopGroup(list);
    prDrawListPopClip(list);
    CHECK(roundTrip(list) <= 2);
    prPathDestroy(circle);
    prDrawListRelease(list);
}

TEST_CASE("glyph outlines read back as the glyphs drew") {
    const PrFont* font = nullptr;
    REQUIRE(prFontCreateFromFile(PR_FONTS_DIR "/NotoSans[wdth,wght].ttf", 0, nullptr, &font) == PrStatus_Ok);
    PrFontGlyph glyphs[3] = {
        {prFontCodePointGlyph(font, 'S'), PrVec2{0, 0}},
        {prFontCodePointGlyph(font, 'v'), PrVec2{14, 0}},
        {prFontCodePointGlyph(font, 'g'), PrVec2{28, 0}},
    };
    PrFontGlyphRun run{.font = font, .size = 24, .glyphs = glyphs, .glyphCount = 3};
    PrDrawList* list = prDrawListCreate();
    prDrawListGlyphs(list, &run, PrVec2{6, 40}, {.color = PrColor{0, 0, 0, 1}});
    CHECK(roundTrip(list) <= 2);
    prDrawListRelease(list);
    prFontRelease(font);
}
