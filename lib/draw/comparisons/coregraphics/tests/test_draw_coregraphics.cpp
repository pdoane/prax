#include "pr/draw_coregraphics.h"
#include "pr/draw_cpu.h"
#include "pr/text_coretext.h"

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

std::vector<uint8_t> render(PrDrawRenderer* renderer, const PrDrawList* list, uint32_t width, uint32_t height) {
    std::vector<uint8_t> bytes(size_t(width) * height * 4, 0);
    PrPixels into{bytes.data(), size_t(width) * 4, {width, height}};
    REQUIRE(prDrawRendererRenderPixels(renderer, list, into, PrPixelFormat_Bgra8) == PrStatus_Ok);
    return bytes;
}

// Returns the total alpha of the pixels.
double inkOf(const std::vector<uint8_t>& pixels) {
    double sum = 0;
    for (size_t i = 3; i < pixels.size(); i += 4)
        sum += pixels[i];
    return sum / 255;
}

// Records one glyph of the font at `size`, painted with `brush` if it is not null.
void recordGlyph(PrDrawList* list, const PrFont* font, uint16_t glyph, float size, const PrDrawBrush* brush = nullptr) {
    PrFontGlyph g{glyph, {10, size * 1.2f}};
    PrFontGlyphRun run{};
    run.font = font;
    run.size = size;
    run.glyphs = &g;
    run.glyphCount = 1;
    prDrawListClear(list);
    prDrawListGlyphs(list, &run, PrVec2{}, {.color = PrColor{0, 0, 0, 1}, .brush = brush});
}

// Records a plane tilted back by `radians` about its horizontal center line, seen from `distance` in front, at the
// center of a 64 by 64 target. It holds a square over its bounds and a smaller square over that.
void recordTilted(PrDrawList* list, float radians, float distance) {
    PrMatrix4 tilt = prMatrix4Rotate(PrVec3{1, 0, 0}, radians);
    PrMatrix4 eye = prMatrix4Perspective(distance);
    PrMatrix4 place = prMatrix4Translate(PrVec3{32, 32, 0});
    PrMatrix4 seen = prMatrix4Concat(&tilt, &eye);
    PrMatrix4 projection = prMatrix4Concat(&seen, &place);
    PrDrawGroup group = prDrawGroupDefault();
    group.bounds = PrRect{-20, -20, 20, 20};
    group.projection = &projection;
    prDrawListPushGroup(list, &group);
    prDrawListRect(list, group.bounds, {.color = PrColor{0.2f, 0.4f, 0.8f, 1}});
    prDrawListRect(list, PrRect{-8, -8, 8, 8}, {.color = PrColor{0.9f, 0.3f, 0.1f, 1}});
    prDrawListPopGroup(list);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// CoreText cannot paint a brush, so a glyph with a brush fills its outline.
TEST_CASE("a glyph with a brush fills its outline as cpu does") {
    const PrFont* font = load("NotoSans.ttf");
    PrDrawRenderer* cg = prDrawCoreGraphicsCreate(nullptr);
    PrDrawRenderer* cpu = prDrawCpuCreate(nullptr);
    PrDrawList* list = prDrawListCreate();
    const PrDrawColorStop stops[] = {
        {.offset = 0, .color = PrColor{0, 0, 0, 1}},
        {.offset = 1, .color = PrColor{0, 0, 0, 1}},
    };
    PrDrawBrush brush = prDrawBrushLinear(PrVec2{0, 0}, PrVec2{100, 0}, stops, 2);
    for (char c : std::string("AVTRWEyoSMLPI")) {
        recordGlyph(list, font, prFontCodePointGlyph(font, uint32_t(c)), 60, &brush);
        INFO("glyph ", c);
        CHECK(inkOf(render(cg, list, 100, 100)) == doctest::Approx(inkOf(render(cpu, list, 100, 100))).epsilon(0.03));
    }
    prDrawListRelease(list);
    prDrawRendererDestroy(cg);
    prDrawRendererDestroy(cpu);
    prFontRelease(font);
}

// A variable font at its default instance, beside a CoreText shaper that holds fonts created from the same bytes.
// Quartz's copy of a font at its defaults draws some glyphs as nothing.
TEST_CASE("native text draws each glyph of a font at its defaults") {
    const PrFont* font = load("NotoSans.ttf");
    PrTextShaper* shaper = prTextCoreTextCreate();
    PrTextGlyphBuffer* shaped = prTextGlyphBufferCreate();
    PrTextRun text{};
    text.font = font;
    text.text = "AVATAR Toyota WAVE small caps";
    REQUIRE(prTextShaperShape(shaper, &text, shaped) == PrStatus_Ok);
    // Smoothing thickens glyphs, so the ink is comparable only without smoothing.
    PrDrawCoreGraphicsOptions options = prDrawCoreGraphicsOptionsDefault();
    options.fontSmoothing = false;
    PrDrawRenderer* native = prDrawCoreGraphicsCreate(&options);
    PrDrawRenderer* cpu = prDrawCpuCreate(nullptr);
    PrDrawList* list = prDrawListCreate();
    for (uint32_t i = 0; i < prTextGlyphBufferGlyphCount(shaped); ++i) {
        uint16_t glyph = prTextGlyphBufferGlyphs(shaped)[i].id;
        for (float size : {11.0f, 26.0f, 60.0f}) {
            recordGlyph(list, font, glyph, size);
            INFO("glyph ", glyph, " at ", size);
            CHECK(inkOf(render(native, list, 100, 100)) ==
                doctest::Approx(inkOf(render(cpu, list, 100, 100))).epsilon(0.1));
        }
    }
    prDrawListRelease(list);
    prDrawRendererDestroy(native);
    prDrawRendererDestroy(cpu);
    prTextGlyphBufferDestroy(shaped);
    prTextShaperDestroy(shaper);
    prFontRelease(font);
}

// Quartz's copy of a font at a design point draws the glyphs of that point.
TEST_CASE("native text draws each glyph of a variation") {
    const PrFont* font = load("NotoSans.ttf");
    // Smoothing thickens glyphs, so the ink is comparable only without smoothing.
    PrDrawCoreGraphicsOptions options = prDrawCoreGraphicsOptionsDefault();
    options.fontSmoothing = false;
    PrDrawRenderer* native = prDrawCoreGraphicsCreate(&options);
    PrDrawRenderer* cpu = prDrawCpuCreate(nullptr);
    PrDrawList* list = prDrawListCreate();
    const PrFontVariation points[][2] = {
        {{PR_FONT_TAG('w', 'g', 'h', 't'), 700}, {0, 0}},
        {{PR_FONT_TAG('w', 'g', 'h', 't'), 300}, {PR_FONT_TAG('w', 'd', 't', 'h'), 75}},
    };
    for (const PrFontVariation* point : points) {
        const PrFont* varied = nullptr;
        PrFontDerivation derivation = prFontDerivationDefault();
        derivation.variations = point;
        derivation.variationCount = 2;
        varied = prFontCreateDerived(font, &derivation);
        for (char c : std::string("AVTRWEyoSMLPIacegn")) {
            recordGlyph(list, varied, prFontCodePointGlyph(varied, uint32_t(c)), 40);
            INFO("glyph ", c, " at wght ", point[0].value);
            CHECK(inkOf(render(native, list, 100, 100)) ==
                doctest::Approx(inkOf(render(cpu, list, 100, 100))).epsilon(0.1));
        }
        prFontRelease(varied);
    }
    prDrawListRelease(list);
    prDrawRendererDestroy(native);
    prDrawRendererDestroy(cpu);
    prFontRelease(font);
}

TEST_CASE("an operator acts where its shape covers, and a group's across the clip, as cpu does") {
    PrDrawList* list = prDrawListCreate();
    prDrawListRect(list, PrRect{0, 0, 48, 32}, {.color = PrColor{0, 0, 0, 1}});
    prDrawListPushClipRect(list, PrRect{0, 0, 24, 32}, PrDrawEdges_Antialiased);
    prDrawListRect(list, PrRect{4, 4, 12, 12}, {.color = PrColor{1, 0, 0, 0.5f}, .composite = PrDrawComposite_Copy});
    PrDrawGroup copy = prDrawGroupDefault();
    copy.composite = PrDrawComposite_Copy;
    prDrawListPushGroup(list, &copy);
    prDrawListRect(list, PrRect{14, 14, 20, 20}, {.color = PrColor{0, 1, 0, 1}});
    prDrawListPopGroup(list);
    prDrawListPopClip(list);
    PrDrawRenderer* cg = prDrawCoreGraphicsCreate(nullptr);
    PrDrawRenderer* cpu = prDrawCpuCreate(nullptr);
    std::vector<uint8_t> drawn = render(cg, list, 48, 32);
    std::vector<uint8_t> expected = render(cpu, list, 48, 32);
    size_t differing = 0;
    for (size_t i = 0; i < drawn.size(); ++i)
        differing += (drawn[i] > expected[i] ? drawn[i] - expected[i] : expected[i] - drawn[i]) > 2;
    CHECK(differing == 0);
    prDrawRendererDestroy(cg);
    prDrawRendererDestroy(cpu);
    prDrawListRelease(list);
}

TEST_CASE("a group is masked by a gradient's alpha, or by its inverse, as cpu does") {
    PrDrawColorStop stops[2] = {
        {.offset = 0, .color = PrColor{0, 0, 0, 1}},
        {.offset = 1, .color = PrColor{0, 0, 0, 0}},
    };
    PrDrawBrush fade = prDrawBrushLinear(PrVec2{8, 0}, PrVec2{56, 0}, stops, 2);
    PrDrawRenderer* cg = prDrawCoreGraphicsCreate(nullptr);
    PrDrawRenderer* cpu = prDrawCpuCreate(nullptr);
    for (bool invert : {false, true}) {
        PrDrawMask mask = prDrawMaskDefault(&fade);
        mask.invert = invert;
        PrDrawGroup group = prDrawGroupDefault();
        group.mask = &mask;
        PrDrawList* list = prDrawListCreate();
        prDrawListPushGroup(list, &group);
        prDrawListRect(list, PrRect{8, 8, 56, 40}, {.color = PrColor{0.2f, 0.4f, 0.8f, 1}});
        prDrawListPopGroup(list);
        std::vector<uint8_t> drawn = render(cg, list, 64, 48);
        std::vector<uint8_t> expected = render(cpu, list, 64, 48);
        size_t differing = 0;
        for (size_t i = 0; i < drawn.size(); ++i)
            differing += (drawn[i] > expected[i] ? drawn[i] - expected[i] : expected[i] - drawn[i]) > 6;
        CHECK(differing == 0);
        prDrawListRelease(list);
    }
    prDrawRendererDestroy(cg);
    prDrawRendererDestroy(cpu);
}

TEST_CASE("transforms compose in doubles, so a shape far down and moved back keeps a quarter pixel") {
    PrDrawList* far = prDrawListCreate();
    prDrawListPushTransformDAffine2(far, prDAffine2Translate(0, 8000000.25));
    prDrawListPushTransformDAffine2(far, prDAffine2Translate(0, -8000000));
    prDrawListRect(far, PrRect{0, 0, 8, 4}, {.color = PrColor{0, 0, 0, 1}});
    prDrawListPopTransform(far);
    prDrawListPopTransform(far);
    PrDrawList* near = prDrawListCreate();
    prDrawListRect(near, PrRect{0, 0.25f, 8, 4.25f}, {.color = PrColor{0, 0, 0, 1}});
    PrDrawRenderer* cg = prDrawCoreGraphicsCreate(nullptr);
    CHECK(render(cg, far, 8, 6) == render(cg, near, 8, 6));
    prDrawRendererDestroy(cg);
    prDrawListRelease(far);
    prDrawListRelease(near);
}

TEST_CASE("a projected group draws through its projection's tangent, where cpu draws through the projection") {
    PrDrawRenderer* cg = prDrawCoreGraphicsCreate(nullptr);
    PrDrawRenderer* cpu = prDrawCpuCreate(nullptr);
    PrDrawList* list = prDrawListCreate();
    // A light tilt, which the tangent approximates within a fraction of a pixel.
    recordTilted(list, 0.35f, 300);
    std::vector<uint8_t> drawn = render(cg, list, 64, 64);
    std::vector<uint8_t> expected = render(cpu, list, 64, 64);
    CHECK(inkOf(drawn) == doctest::Approx(inkOf(expected)).epsilon(0.02));
    size_t differing = 0;
    for (size_t i = 0; i < drawn.size(); i += 4) {
        for (size_t k = 0; k < 4; ++k) {
            int delta = int(drawn[i + k]) - int(expected[i + k]);
            if (delta > 48 || delta < -48) {
                ++differing;
                break;
            }
        }
    }
    // The tangent misses the projected near edge by most of a pixel, so that edge's last row differs from cpu's.
    CHECK(differing <= 112);
    // An edge-on plane draws nothing.
    prDrawListClear(list);
    recordTilted(list, 1.5707964f, 300);
    CHECK(inkOf(render(cg, list, 64, 64)) == 0);
    prDrawListRelease(list);
    prDrawRendererDestroy(cg);
    prDrawRendererDestroy(cpu);
}
