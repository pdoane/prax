#include "pr/draw_blend2d.h"
#include "pr/draw_cpu.h"
#include "pr/vec.h"

#include "doctest/doctest.h"

#include <cstdlib>
#include <vector>

namespace pr_impl {

namespace {

std::vector<uint8_t> render(PrDrawRenderer* renderer, const PrDrawList* list, uint32_t width,
                            uint32_t height, PrPixelFormat format = PrPixelFormat_Bgra8) {
    std::vector<uint8_t> bytes(size_t(width) * height * 4, 0);
    PrPixels into{bytes.data(), size_t(width) * 4, {width, height}};
    REQUIRE(prDrawRendererRenderPixels(renderer, list, into, format) == PrStatus_Ok);
    return bytes;
}

// Returns the number of pixels of two renders that differ in some channel by more than `tolerance`.
size_t differing(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, int tolerance) {
    size_t count = 0;
    for (size_t i = 0; i < a.size(); i += 4) {
        for (size_t k = 0; k < 4; ++k) {
            if (std::abs(int(a[i + k]) - int(b[i + k])) > tolerance) {
                ++count;
                break;
            }
        }
    }
    return count;
}

// Renders the list with cpu and with Blend2D, both synchronous and threaded. Returns the number of pixels where Blend2D
// differs from cpu by more than `tolerance` levels. The two rasterizers differ by a few levels along antialiased edges.
size_t againstCpu(const PrDrawList* list, uint32_t width, uint32_t height, int tolerance = 4) {
    PrDrawRenderer* cpu = prDrawCpuCreate(nullptr);
    PrDrawBlend2dOptions options = prDrawBlend2dOptionsDefault();
    PrDrawRenderer* blend2d = prDrawBlend2dCreate(&options);
    options.threads = 4;
    PrDrawRenderer* threaded = prDrawBlend2dCreate(&options);
    std::vector<uint8_t> expected = render(cpu, list, width, height);
    std::vector<uint8_t> actual = render(blend2d, list, width, height);
    std::vector<uint8_t> async = render(threaded, list, width, height);
    CHECK(differing(actual, async, 0) == 0);
    prDrawRendererDestroy(cpu);
    prDrawRendererDestroy(blend2d);
    prDrawRendererDestroy(threaded);
    return differing(expected, actual, tolerance);
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

TEST_CASE("blend2d fills as cpu does, onto either format") {
    PrDrawList* list = prDrawListCreate();
    prDrawListRect(list, PrRect{4, 4, 28, 20}, {.color = PrColor{0.2f, 0.4f, 0.8f, 1}});
    prDrawListEllipse(list, PrVec2{40, 20}, PrVec2{12, 8}, 0, {.color = PrColor{0.9f, 0.3f, 0.1f, 0.5f}});
    CHECK(againstCpu(list, 64, 40) == 0);
    PrDrawRenderer* cpu = prDrawCpuCreate(nullptr);
    PrDrawRenderer* blend2d = prDrawBlend2dCreate(nullptr);
    CHECK(differing(render(cpu, list, 64, 40, PrPixelFormat_Rgba8),
                    render(blend2d, list, 64, 40, PrPixelFormat_Rgba8), 4)
          == 0);
    prDrawRendererDestroy(cpu);
    prDrawRendererDestroy(blend2d);
    prDrawListRelease(list);
}

TEST_CASE("blend2d clips to a rectangle until its pop") {
    PrDrawList* list = prDrawListCreate();
    prDrawListPushClipRect(list, PrRect{8, 8, 24, 24}, PrDrawEdges_Antialiased);
    prDrawListRect(list, PrRect{0, 0, 32, 32}, {.color = PrColor{0, 0, 0, 1}});
    prDrawListPopClip(list);
    prDrawListRect(list, PrRect{32, 0, 40, 8}, {.color = PrColor{0, 0, 1, 1}});
    CHECK(againstCpu(list, 48, 32) == 0);
    prDrawListRelease(list);
}

TEST_CASE("blend2d clips to other shapes through their coverage") {
    PrDrawList* list = prDrawListCreate();
    prDrawListPushClipCornerRect(list, prCornerRectRound(PrRect{4, 4, 44, 28}, 8), PrDrawEdges_Antialiased);
    prDrawListRect(list, PrRect{0, 0, 48, 32}, {.color = PrColor{0.1f, 0.6f, 0.3f, 1}});
    prDrawListPopClip(list);
    // Where a corner barely touches a pixel, Blend2D's coverage is a few levels below the area.
    CHECK(againstCpu(list, 48, 32, 6) == 0);
    prDrawListRelease(list);
}

TEST_CASE("blend2d composites a group with an effect as cpu does") {
    PrDrawList* list = prDrawListCreate();
    prDrawListRect(list, PrRect{0, 0, 48, 32}, {.color = PrColor{1, 1, 1, 1}});
    PrDrawGroup group = prDrawGroupDefault();
    group.opacity = 0.5f;
    prDrawListPushGroup(list, &group);
    prDrawListRect(list, PrRect{4, 4, 30, 28}, {.color = PrColor{1, 0, 0, 1}});
    prDrawListRect(list, PrRect{18, 4, 44, 28}, {.color = PrColor{0, 0, 1, 1}});
    prDrawListPopGroup(list);
    CHECK(againstCpu(list, 48, 32) == 0);
    prDrawListRelease(list);
}

TEST_CASE("blend2d draws an operator where its shape covers, and a group's across the clip") {
    PrDrawList* list = prDrawListCreate();
    prDrawListRect(list, PrRect{0, 0, 48, 32}, {.color = PrColor{0, 0, 0, 1}});
    prDrawListPushClipRect(list, PrRect{0, 0, 24, 32}, PrDrawEdges_Antialiased);
    prDrawListRect(list, PrRect{4, 4, 12.5f, 12}, {.color = PrColor{1, 0, 0, 0.5f}, .composite = PrDrawComposite_Copy});
    PrDrawGroup copy = prDrawGroupDefault();
    copy.composite = PrDrawComposite_Copy;
    prDrawListPushGroup(list, &copy);
    prDrawListRect(list, PrRect{14, 14, 20, 20}, {.color = PrColor{0, 1, 0, 1}});
    prDrawListPopGroup(list);
    prDrawListPopClip(list);
    CHECK(againstCpu(list, 48, 32) == 0);
    prDrawListRelease(list);
}

TEST_CASE("blend2d nests a rotated clip inside a target-pixel clip") {
    PrDrawList* list = prDrawListCreate();
    prDrawListPushTransformAffine2(list, prAffine2Concat(prAffine2Rotate(0.6f),
                                                         prAffine2Translate(PrVec2{32, 32})));
    prDrawListPushClipTargetRect(list, PrRect{8, 8, 56, 56}, PrDrawEdges_Antialiased);
    prDrawListRect(list, PrRect{-40, -40, 40, 40}, {.color = PrColor{0, 0.6f, 0.3f, 1}});
    prDrawListPushClipRect(list, PrRect{-20, -20, 20, 20}, PrDrawEdges_Antialiased);
    for (float x = -60; x < 60; x += 12) {
        PrVec2 band[] = {{x, -20}, {x + 6, -20}, {x + 46, 20}, {x + 40, 20}};
        prDrawListPolylineContour(list, band, 4, PrDrawContour_Closed, PrPathFillRule_NonZero,
                                  {.color = PrColor{1, 0.7f, 0, 1}});
    }
    prDrawListPopClip(list);
    prDrawListPopClip(list);
    prDrawListPopTransform(list);
    CHECK(againstCpu(list, 64, 64, 8) == 0);
    prDrawListRelease(list);
}

TEST_CASE("blend2d draws stripes under a rotated clip inside a target-pixel clip, page-sized") {
    PrDrawList* list = prDrawListCreate();
    // The previous cell: a clip in a rotated space, with stripes drawn in the page's space again.
    {
        PrRect d{400, 30, 700, 330};
        PrAffine2 rotated =
            prAffine2Concat(prAffine2Rotate(0.3f), prAffine2Translate(PrVec2{550, 180}));
        PrAffine2 back{};
        prAffine2Invert(rotated, &back);
        prDrawListPushTransformAffine2(list, rotated);
        prDrawListPushClipRect(list, PrRect{-100, -100, 100, 100}, PrDrawEdges_Antialiased);
        prDrawListPushTransformAffine2(list, back);
        prDrawListPushClipRect(list, d, PrDrawEdges_Antialiased);
        for (float x = d.left - 200; x < d.right + 200; x += 24) {
            PrVec2 band[] = {
                {x, d.top}, {x + 12, d.top}, {x + 212, d.bottom}, {x + 200, d.bottom}};
            prDrawListPolylineContour(list, band, 4, PrDrawContour_Closed, PrPathFillRule_NonZero,
                                      {.color = PrColor{0, 0, 1, 1}});
        }
        prDrawListPopClip(list);
        prDrawListPopTransform(list);
        prDrawListPopClip(list);
        prDrawListPopTransform(list);
    }
    PrRect c{30, 430, 337, 754};
    prDrawListPushTransformAffine2(list, prAffine2Concat(prAffine2Rotate(0.6f),
                                                         prAffine2Translate(PrVec2{183.5f, 592})));
    prDrawListPushClipTargetRect(list, PrRect{c.left + 30, c.top + 30, c.right - 30,
                                               c.bottom - 30}, PrDrawEdges_Antialiased);
    prDrawListRect(list, PrRect{-200, -200, 200, 200}, {.color = PrColor{0, 0.6f, 0.3f, 1}});
    PrRect s{-200, -200, 200, 200};
    prDrawListPushClipRect(list, s, PrDrawEdges_Antialiased);
    for (float x = s.left - 200; x < s.right + 200; x += 24) {
        PrVec2 band[] = {
            {x, s.top}, {x + 12, s.top}, {x + 212, s.bottom}, {x + 200, s.bottom}};
        prDrawListPolylineContour(list, band, 4, PrDrawContour_Closed, PrPathFillRule_NonZero,
                                  {.color = PrColor{1, 0.7f, 0, 1}});
    }
    prDrawListPopClip(list);
    prDrawListPopClip(list);
    prDrawListPopTransform(list);
    CHECK(againstCpu(list, 720, 800, 8) == 0);
    prDrawListRelease(list);
}

TEST_CASE("blend2d clips to a rectangle off whole pixels through its coverage") {
    PrDrawList* list = prDrawListCreate();
    prDrawListPushClipTargetRect(list, PrRect{8.5f, 8, 40.25f, 24}, PrDrawEdges_Antialiased);
    prDrawListRect(list, PrRect{0, 0, 48, 32}, {.color = PrColor{0.1f, 0.6f, 0.3f, 1}});
    prDrawListPopClip(list);
    CHECK(againstCpu(list, 48, 32) == 0);
    prDrawListRelease(list);
}

TEST_CASE("blend2d draws no shadow, having no blur") {
    PrDrawList* list = prDrawListCreate();
    prDrawListRect(list, PrRect{0, 0, 96, 96}, {.color = PrColor{1, 1, 1, 1}});
    PrDrawList* plain = prDrawListCreate();
    prDrawListRect(plain, PrRect{0, 0, 96, 96}, {.color = PrColor{1, 1, 1, 1}});
    PrDrawShadow shadow{};
    shadow.shape.rect = PrRect{16, 16, 64, 72};
    shadow.offset = PrVec2{6, 6};
    shadow.sigma = 4;
    prDrawListDropShadow(list, &shadow, {.color = PrColor{0, 0, 0, 0.5f}});
    PrDrawRenderer* blend2d = prDrawBlend2dCreate(nullptr);
    CHECK(differing(render(blend2d, list, 96, 96), render(blend2d, plain, 96, 96), 0) == 0);
    prDrawRendererDestroy(blend2d);
    prDrawListRelease(plain);
    prDrawListRelease(list);
}

TEST_CASE("blend2d masks a group by a gradient's alpha, or by its inverse") {
    PrDrawColorStop stops[2] = {{.offset = 0, .color = PrColor{0, 0, 0, 1}},
                                {.offset = 1, .color = PrColor{0, 0, 0, 0}}};
    PrDrawBrush fade = prDrawBrushLinear(PrVec2{8, 0}, PrVec2{56, 0}, stops, 2);
    for (bool invert : {false, true}) {
        PrDrawMask mask = prDrawMaskDefault(&fade);
        mask.invert = invert;
        PrDrawGroup group = prDrawGroupDefault();
        group.mask = &mask;
        PrDrawList* list = prDrawListCreate();
        prDrawListPushGroup(list, &group);
        prDrawListRect(list, PrRect{8, 8, 56, 40}, {.color = PrColor{0.2f, 0.4f, 0.8f, 1}});
        prDrawListPopGroup(list);
        CHECK(againstCpu(list, 64, 48, 6) == 0);
        prDrawListRelease(list);
    }
}

TEST_CASE("blend2d composes transforms in doubles, so a shape far down and moved back keeps a quarter pixel") {
    PrDrawList* far = prDrawListCreate();
    prDrawListPushTransformDAffine2(far, prDAffine2Translate(0, 8000000.25));
    prDrawListPushTransformDAffine2(far, prDAffine2Translate(0, -8000000));
    prDrawListRect(far, PrRect{0, 0, 8, 4}, {.color = PrColor{0, 0, 0, 1}});
    prDrawListPopTransform(far);
    prDrawListPopTransform(far);
    PrDrawList* near = prDrawListCreate();
    prDrawListRect(near, PrRect{0, 0.25f, 8, 4.25f}, {.color = PrColor{0, 0, 0, 1}});
    PrDrawRenderer* blend2d = prDrawBlend2dCreate(nullptr);
    CHECK(differing(render(blend2d, far, 8, 6), render(blend2d, near, 8, 6), 0) == 0);
    prDrawRendererDestroy(blend2d);
    prDrawListRelease(far);
    prDrawListRelease(near);
}

TEST_CASE("blend2d draws a projected group through its projection's tangent, where cpu draws through the projection") {
    PrDrawList* list = prDrawListCreate();
    // A light tilt, which the tangent approximates within a fraction of a pixel.
    recordTilted(list, 0.35f, 300);
    // The tangent misses the projected near edge by most of a pixel, so that edge's last row differs from cpu's.
    CHECK(againstCpu(list, 64, 64, 48) <= 112);
    // An edge-on plane draws nothing.
    prDrawListClear(list);
    recordTilted(list, 1.5707964f, 300);
    PrDrawRenderer* blend2d = prDrawBlend2dCreate(nullptr);
    std::vector<uint8_t> drawn = render(blend2d, list, 64, 64);
    CHECK(differing(drawn, std::vector<uint8_t>(drawn.size(), 0), 0) == 0);
    prDrawRendererDestroy(blend2d);
    prDrawListRelease(list);
}
