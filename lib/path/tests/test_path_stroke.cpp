#include "pr/path_rasterizer.h"
#include "pr/path_stroke.h"
#include "pr/vec.h"

#include "tools/harness/test.h"

#include <cmath>
#include <vector>

namespace pr_impl {

namespace {

// Replaces the contents of `out` with the outline of the stroked path, as a path of its contours.
void strokeTo(const PrPath* path, const PrPathStroke* stroke, float tolerance, PrPath* out) {
    PrPathPolyline outline{};
    prPathStrokeOutline(path, stroke, tolerance, &outline);
    prPathClear(out);
    prPathAddPolyline(out, &outline, prAffine2Identity());
    prPathPolylineReset(&outline);
}

// Returns the area that the stroked path covers within 64 by 64, computed from its coverage at eight times that
// resolution. The pieces of the outline overlap. If two pieces partly cover one pixel, their coverage adds before it
// saturates. The finer grid makes that error negligible.
double strokedArea(const PrPath* path, const PrPathStroke& stroke) {
    constexpr uint32_t kScale = 8;
    constexpr uint32_t kSize = 64 * kScale;
    PrPathPolyline outline{};
    prPathStrokeOutline(path, &stroke, 0.001f, &outline);
    prPathPolylineTransform(&outline, prAffine2Scale(PrVec2{float(kScale), float(kScale)}));
    std::vector<uint8_t> coverage(kSize * kSize);
    PrPixels mask{coverage.data(), kSize, {kSize, kSize}};
    PrPathRasterizer* rasterizer = prPathRasterizerCreate();
    prPathRasterizerFill(rasterizer, &outline, PrPathFillRule_NonZero, PrPathCoverage_Area, PrIVec2{0, 0}, mask);
    prPathRasterizerDestroy(rasterizer);
    prPathPolylineReset(&outline);
    double sum = 0;
    for (uint8_t c : coverage)
        sum += c / 255.0;
    return sum / (kScale * kScale);
}

PrPathStroke style(float width, PrPathJoin join = PrPathJoin_Miter, PrPathCap cap = PrPathCap_Butt) {
    return PrPathStroke{
        .width = width,
        .miterLimit = 4,
        .join = join,
        .startCap = cap,
        .endCap = cap,
        .dashCap = cap,
        .zeroLengthDirection = PrVec2{1, 0},
    };
}

PrPath* polyline(std::initializer_list<PrVec2> points, bool closed = false) {
    PrPath* path = prPathCreate();
    bool first = true;
    for (PrVec2 p : points) {
        if (first)
            prPathMoveTo(path, p);
        else
            prPathLineTo(path, p);
        first = false;
    }
    if (closed)
        prPathClose(path);
    return path;
}

constexpr double kPi = 3.14159265358979;

uint32_t contourCount(const PrPath* path) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < prPathVerbCount(path); ++i)
        count += prPathVerbs(path)[i] == PrPathVerb_Move ? 1 : 0;
    return count;
}

uint32_t strokedContours(const PrPath* path, const PrPathStroke& stroke) {
    PrPath* outline = prPathCreate();
    strokeTo(path, &stroke, 0.01f, outline);
    uint32_t count = contourCount(outline);
    prPathDestroy(outline);
    return count;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("caps extend a line by nothing, a half square or a half circle") {
    PrPath* line = polyline({{10, 10}, {30, 10}});
    CHECK(strokedArea(line, style(4)) == doctest::Approx(80).epsilon(0.001));
    CHECK(strokedArea(line, style(4, PrPathJoin_Miter, PrPathCap_Square)) == doctest::Approx(96).epsilon(0.001));
    CHECK(strokedArea(line, style(4, PrPathJoin_Miter, PrPathCap_Round)) ==
        doctest::Approx(80 + kPi * 4).epsilon(0.002));
    prPathDestroy(line);
}

TEST_CASE("joins fill the outer corner with a point, a triangle or an arc") {
    PrPath* corner = polyline({{10, 10}, {30, 10}, {30, 30}});
    // The two segments cover 156 together, with the corner square overlapping.
    CHECK(strokedArea(corner, style(4, PrPathJoin_Miter)) == doctest::Approx(160).epsilon(0.001));
    CHECK(strokedArea(corner, style(4, PrPathJoin_Bevel)) == doctest::Approx(158).epsilon(0.001));
    CHECK(strokedArea(corner, style(4, PrPathJoin_Round)) == doctest::Approx(156 + kPi).epsilon(0.002));
    prPathDestroy(corner);
}

TEST_CASE("a miter past the limit is beveled") {
    // Segments 10 degrees apart make a miter 11.5 widths long.
    PrPath* sharp = polyline({{5, 30}, {35, 30}, {5, 35.3f}});
    PrPathStroke limited = style(2, PrPathJoin_Miter);
    PrPathStroke beveled = style(2, PrPathJoin_Bevel);
    CHECK(strokedArea(sharp, limited) == doctest::Approx(strokedArea(sharp, beveled)));
    limited.miterLimit = 20;
    CHECK(strokedArea(sharp, limited) > strokedArea(sharp, beveled) + 1);
    prPathDestroy(sharp);
}

TEST_CASE("a closed contour joins its ends and takes no caps") {
    PrPath* square = polyline({{10, 10}, {30, 10}, {30, 30}, {10, 30}}, true);
    CHECK(strokedArea(square, style(2, PrPathJoin_Miter, PrPathCap_Round)) ==
        doctest::Approx(22 * 22 - 18 * 18).epsilon(0.001));
    prPathDestroy(square);
}

TEST_CASE("dashes cut the line from the offset into the pattern") {
    PrPath* line = polyline({{10, 10}, {27, 10}});
    const float pattern[] = {4, 6};
    PrPathStroke dashed = style(2);
    dashed.dashes = pattern;
    dashed.dashCount = 2;
    // Dashes over 0-4 and 10-14.
    CHECK(strokedArea(line, dashed) == doctest::Approx(16).epsilon(0.001));
    // From 5 into the pattern: dashes over 5-9 and 15-17.
    dashed.dashOffset = 5;
    CHECK(strokedArea(line, dashed) == doctest::Approx(12).epsilon(0.001));
    dashed.dashOffset = -5;
    CHECK(strokedArea(line, dashed) == doctest::Approx(12).epsilon(0.001));
    prPathDestroy(line);
}

TEST_CASE("an odd pattern repeats and a negative one strokes solid") {
    PrPath* line = polyline({{10, 10}, {30, 10}});
    const float odd[] = {5};
    PrPathStroke dashed = style(2);
    dashed.dashes = odd;
    dashed.dashCount = 1;
    CHECK(strokedArea(line, dashed) == doctest::Approx(20).epsilon(0.001));
    const float negative[] = {5, -1};
    dashed.dashes = negative;
    dashed.dashCount = 2;
    CHECK(strokedArea(line, dashed) == doctest::Approx(40).epsilon(0.001));
    prPathDestroy(line);
}

TEST_CASE("a segment of no length is a dot where its caps give it area") {
    PrPath* dot = polyline({{20, 20}, {20, 20}});
    CHECK(strokedArea(dot, style(6)) == doctest::Approx(0));
    CHECK(strokedArea(dot, style(6, PrPathJoin_Miter, PrPathCap_Square)) == doctest::Approx(36).epsilon(0.001));
    CHECK(strokedArea(dot, style(6, PrPathJoin_Miter, PrPathCap_Round)) == doctest::Approx(kPi * 9).epsilon(0.002));
    prPathDestroy(dot);

    PrPath* move = prPathCreate();
    prPathMoveTo(move, PrVec2{20, 20});
    CHECK(strokedArea(move, style(6, PrPathJoin_Miter, PrPathCap_Round)) == doctest::Approx(0));
    prPathDestroy(move);
}

TEST_CASE("a contour that closes at once is a segment of no length") {
    PrPath* path = polyline({{20, 20}}, true);
    CHECK(strokedArea(path, style(6)) == doctest::Approx(0));
    CHECK(strokedArea(path, style(6, PrPathJoin_Miter, PrPathCap_Square)) == doctest::Approx(36).epsilon(0.001));
    CHECK(strokedArea(path, style(6, PrPathJoin_Miter, PrPathCap_Round)) == doctest::Approx(kPi * 9).epsilon(0.002));
    prPathDestroy(path);
}

TEST_CASE("the caps of a contour of no length face the stroke's direction for it") {
    PrPath* dot = polyline({{20, 20}, {20, 20}});
    PrPathStroke turned = style(6, PrPathJoin_Miter, PrPathCap_Square);
    PrPath* outline = prPathCreate();
    strokeTo(dot, &turned, 0.01f, outline);
    CHECK(prRectWidth(prPathTightBounds(outline)) == doctest::Approx(6).epsilon(0.001));

    // A square turned 45° spans its diagonal.
    turned.zeroLengthDirection = PrVec2{2, 2};
    strokeTo(dot, &turned, 0.01f, outline);
    CHECK(prRectWidth(prPathTightBounds(outline)) == doctest::Approx(6 * 1.41421356).epsilon(0.001));
    CHECK(strokedArea(dot, turned) == doctest::Approx(36).epsilon(0.002));

    // A zero vector faces +x.
    turned.zeroLengthDirection = PrVec2{0, 0};
    strokeTo(dot, &turned, 0.01f, outline);
    CHECK(prRectWidth(prPathTightBounds(outline)) == doctest::Approx(6).epsilon(0.001));
    prPathDestroy(outline);
    prPathDestroy(dot);
}

TEST_CASE("dashes of no length are dots, including one at the contour's start") {
    PrPath* line = polyline({{10, 20}, {50, 20}});
    const float dashes[2] = {0, 10};
    PrPathStroke dotted = style(4, PrPathJoin_Miter, PrPathCap_Square);
    dotted.dashes = dashes;
    dotted.dashCount = 2;
    // Dots at 0, 10, 20, 30 and 40 along the line, each a square of the width.
    CHECK(strokedArea(line, dotted) == doctest::Approx(5 * 16).epsilon(0.002));
    prPathDestroy(line);
}

TEST_CASE("curves stroke to within the tolerance") {
    PrPath* circle = prPathCreate();
    prPathAddEllipse(circle, PrVec2{32, 32}, PrVec2{20, 20}, 0);
    // An annulus from radius 19 to 21.
    CHECK(strokedArea(circle, style(2)) == doctest::Approx(kPi * (21 * 21 - 19 * 19)).epsilon(0.005));
    prPathDestroy(circle);
}

TEST_CASE("a stroke with no width is nothing") {
    PrPath* line = polyline({{10, 10}, {30, 10}});
    PrPath* out = prPathCreate();
    prPathAddRect(out, PrRect{0, 0, 1, 1});
    PrPathStroke none = style(0);
    strokeTo(line, &none, 0.1f, out);
    CHECK(prPathVerbCount(out) == 0);
    prPathDestroy(out);
    prPathDestroy(line);
}

TEST_CASE("an open contour strokes as one loop and a closed one as two") {
    PrPath* open = polyline({{10, 10}, {30, 10}, {30, 30}, {12, 40}});
    CHECK(strokedContours(open, style(4, PrPathJoin_Round, PrPathCap_Round)) == 1);
    PrPath* closed = polyline({{10, 10}, {30, 10}, {30, 30}}, true);
    CHECK(strokedContours(closed, style(4)) == 2);
    prPathDestroy(open);
    prPathDestroy(closed);
}

TEST_CASE("a triangle cap is a point half the width past the end") {
    PrPath* line = polyline({{10, 10}, {30, 10}});
    CHECK(strokedArea(line, style(4, PrPathJoin_Miter, PrPathCap_Triangle)) == doctest::Approx(88).epsilon(0.001));
    prPathDestroy(line);
}

TEST_CASE("a clipped miter lies between the bevel and the whole miter") {
    PrPath* sharp = polyline({{5, 30}, {35, 30}, {5, 35.3f}});
    double bevel = strokedArea(sharp, style(2, PrPathJoin_Bevel));
    PrPathStroke miter = style(2, PrPathJoin_Miter);
    miter.miterLimit = 20;
    double whole = strokedArea(sharp, miter);
    PrPathStroke clipped = style(2, PrPathJoin_MiterClip);
    double cut = strokedArea(sharp, clipped);
    CHECK(cut > bevel + 1);
    CHECK(cut < whole - 1);
    clipped.miterLimit = 20;
    CHECK(strokedArea(sharp, clipped) == doctest::Approx(whole).epsilon(0.001));
    prPathDestroy(sharp);
}

TEST_CASE("an arcs join between lines is a miter") {
    PrPath* corner = polyline({{10, 10}, {30, 10}, {20, 30}});
    CHECK(strokedArea(corner, style(4, PrPathJoin_Arcs)) ==
        doctest::Approx(strokedArea(corner, style(4, PrPathJoin_Miter))).epsilon(0.001));
    prPathDestroy(corner);
}

TEST_CASE("an arcs join between curves bending away falls between the bevel and the miter") {
    PrPath* arch = prPathCreate();
    prPathMoveTo(arch, PrVec2{10, 50});
    prPathQuadTo(arch, PrVec2{10, 20}, PrVec2{32, 10});
    prPathQuadTo(arch, PrVec2{54, 20}, PrVec2{54, 50});
    double bevel = strokedArea(arch, style(6, PrPathJoin_Bevel));
    double arcs = strokedArea(arch, style(6, PrPathJoin_Arcs));
    double miter = strokedArea(arch, style(6, PrPathJoin_Miter));
    CHECK(arcs > bevel + 0.02);
    CHECK(arcs < miter - 0.02);
    prPathDestroy(arch);
}

TEST_CASE("a width profile varies each side's half width along the contour") {
    PrPath* line = polyline({{10, 20}, {50, 20}});
    const PrPathWidthPoint taper[] = {{0, 0, 0}, {1, 0.5f, 0.5f}};
    PrPathStroke tapered = style(4);
    tapered.profile = taper;
    tapered.profileCount = 2;
    CHECK(strokedArea(line, tapered) == doctest::Approx(80).epsilon(0.001));

    const PrPathWidthPoint oneSided[] = {{0, 1, 0}};
    PrPathStroke left = style(4);
    left.profile = oneSided;
    left.profileCount = 1;
    PrPath* outline = prPathCreate();
    strokeTo(line, &left, 0.01f, outline);
    PrRect bounds = prPathBounds(outline);
    CHECK(bounds.top == doctest::Approx(16));
    CHECK(bounds.bottom == doctest::Approx(20));
    CHECK(strokedArea(line, left) == doctest::Approx(160).epsilon(0.001));
    prPathDestroy(outline);

    // A bulge in the middle, held constant beyond its points.
    const PrPathWidthPoint bulge[] = {{0.25f, 0.5f, 0.5f}, {0.5f, 1, 1}, {0.75f, 0.5f, 0.5f}};
    PrPathStroke bulged = style(2);
    bulged.profile = bulge;
    bulged.profileCount = 3;
    // 20 at width 2, and 20 rising to width 4 and back: 40 + 60.
    CHECK(strokedArea(line, bulged) == doctest::Approx(100).epsilon(0.001));
    prPathDestroy(line);
}

TEST_CASE("the last dash of a closed contour runs into the first") {
    PrPath* square = polyline({{10, 10}, {30, 10}, {30, 30}, {10, 30}}, true);
    const float pattern[] = {15, 5};
    PrPathStroke dashed = style(2);
    dashed.dashes = pattern;
    dashed.dashCount = 2;
    dashed.dashOffset = 10;
    // Dashes over 0-5, 10-25, 30-45, 50-65 and 70-80. The last joins the first.
    CHECK(strokedContours(square, dashed) == 4);
    // A dash covering the whole contour strokes it closed.
    const float long_[] = {100, 5};
    dashed.dashes = long_;
    dashed.dashOffset = 0;
    CHECK(strokedContours(square, dashed) == 2);
    prPathDestroy(square);
}

TEST_CASE("a stroke that turns back on itself stays within its width of the path") {
    // A sliver that runs out a tenth of a unit and straight back, under a slight rotation. Its offsets meet at vertices
    // at which the stroke nearly reverses.
    PrVec2 a{110.28155517578125f, 6.6706695556640625f};
    PrVec2 b{110.32177734375f, 6.6713409423828125f};
    PrVec2 c{110.36199951171875f, 6.672004699707031f};
    for (PrPathJoin join : {PrPathJoin_Miter, PrPathJoin_MiterClip, PrPathJoin_Bevel, PrPathJoin_Round}) {
        PrPath* path = prPathCreate();
        prPathMoveTo(path, a);
        prPathCubicTo(path, a, b, b);
        prPathCubicTo(path, b, c, c);
        prPathCubicTo(path, c, a, a);
        prPathCubicTo(path, a, a, a);
        prPathClose(path);
        prPathTransform(path, PrAffine2{
            0.9999831914901733f, 0.005801171530038118f, -0.005801171530038118f,
            0.9999831914901733f, 39.26332092285156f, 94.71268463134766f,
        });
        PrPathStroke stroke = prPathStrokeDefault();
        stroke.width = 5;
        stroke.join = join;
        PrPath* out = prPathCreate();
        strokeTo(path, &stroke, 0.1f, out);
        // No further than the miter limit, plus the half width of the clipped miter beyond it.
        float reach = stroke.miterLimit * stroke.width / 2 + stroke.width / 2;
        PrRect around = prPathBounds(path);
        PrRect bounds = prPathBounds(out);
        CHECK(bounds.left >= around.left - reach);
        CHECK(bounds.right <= around.right + reach);
        CHECK(bounds.top >= around.top - reach);
        CHECK(bounds.bottom <= around.bottom + reach);
        prPathDestroy(out);
        prPathDestroy(path);
    }
}

TEST_CASE("a stroke's outline lies within its reach of the path") {
    // The tip turns through a miter 1 / sin(atan(1 / 2)) = sqrt(5) widths long.
    PrPath* path = polyline({{10, 50}, {30, 10}, {50, 50}});
    PrPath* outline = prPathCreate();
    PrPathJoin joins[] = {PrPathJoin_Miter, PrPathJoin_MiterClip, PrPathJoin_Arcs, PrPathJoin_Round, PrPathJoin_Bevel};
    PrPathCap caps[] = {PrPathCap_Butt, PrPathCap_Round, PrPathCap_Square, PrPathCap_Triangle};
    for (PrPathJoin join : joins) {
        for (PrPathCap cap : caps) {
            PrPathStroke stroke = style(4, join, cap);
            stroke.miterLimit = 2.25f;
            float reach = prPathStrokeReach(&stroke);
            strokeTo(path, &stroke, 0.01f, outline);
            PrRect bounds = prPathTightBounds(outline);
            CHECK(bounds.left >= 10 - reach - 0.01f);
            CHECK(bounds.top >= 10 - reach - 0.01f);
            CHECK(bounds.right <= 50 + reach + 0.01f);
            CHECK(bounds.bottom <= 50 + reach + 0.01f);
        }
    }
    // A miter within its limit reaches almost that far.
    PrPathStroke miter = style(4);
    miter.miterLimit = 2.25f;
    strokeTo(path, &miter, 0.01f, outline);
    CHECK(prPathStrokeReach(&miter) == doctest::Approx(4.5));
    CHECK(prPathTightBounds(outline).top == doctest::Approx(10 - 2 * std::sqrt(5.0)).epsilon(0.001));
    PrPathStroke none = style(0);
    CHECK(prPathStrokeReach(&none) == 0.0f);
    prPathDestroy(outline);
    prPathDestroy(path);
}

TEST_CASE("a path with points that are not finite strokes without failing") {
    for (float bad : {INFINITY, -INFINITY, NAN}) {
        PrPath* path = prPathCreate();
        prPathMoveTo(path, PrVec2{0, 0});
        prPathLineTo(path, PrVec2{bad, 5});
        prPathQuadTo(path, PrVec2{5, bad}, PrVec2{5, 10});
        prPathCubicTo(path, PrVec2{bad, bad}, PrVec2{1, 2}, PrVec2{3, 4});
        prPathClose(path);
        const float dashes[2] = {3, 2};
        const PrPathWidthPoint profile[2] = {{0, 0.5f, 0.5f}, {1, 1, 1}};
        for (PrPathJoin join : {PrPathJoin_Miter, PrPathJoin_Round, PrPathJoin_Arcs}) {
            PrPathStroke dashed = style(4, join, PrPathCap_Round);
            dashed.dashes = dashes;
            dashed.dashCount = 2;
            dashed.profile = profile;
            dashed.profileCount = 2;
            PrPath* out = prPathCreate();
            strokeTo(path, &dashed, 0.1f, out);
            PrPathStroke square = style(4, join, PrPathCap_Square);
            strokeTo(path, &square, 0.1f, out);
            prPathDestroy(out);
        }
        prPathDestroy(path);
    }
}
