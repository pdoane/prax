#include "pr/draw_commands.h"
#include "pr/draw_list.h"
#include "pr/draw_lower.h"
#include "pr/draw_transform.h"
#include "pr/path_rasterizer.h"
#include "pr/vec.h"

#include "doctest/doctest.h"

#include <cmath>
#include <vector>

namespace pr_impl {

namespace {

constexpr double kPi = 3.14159265358979;

// Returns the area that a nonzero or evenodd fill of the path covers within 128 by 128, measured at four times that
// resolution.
double areaOf(const PrPath* path, PrPathFillRule rule = PrPathFillRule_NonZero) {
    constexpr uint32_t kScale = 4;
    constexpr uint32_t kSize = 128 * kScale;
    std::vector<uint8_t> coverage(kSize * kSize);
    PrPixels mask{coverage.data(), kSize, {kSize, kSize}};
    PrPathRasterizer* rasterizer = prPathRasterizerCreate();
    PrPathPolyline polyline{};
    PrAffine2 scale = prAffine2Scale(PrVec2{float(kScale), float(kScale)});
    prPathFlatten(path, scale, 0.05f, PrPathFlattening_Balanced, &polyline);
    prPathRasterizerFill(rasterizer, &polyline, rule, PrPathCoverage_Area, PrIVec2{0, 0}, mask);
    prPathPolylineReset(&polyline);
    prPathRasterizerDestroy(rasterizer);
    double sum = 0;
    for (uint8_t c : coverage)
        sum += c / 255.0;
    return sum / (kScale * kScale);
}

double areaOf(const PrPathPolyline& polyline, PrPathFillRule rule = PrPathFillRule_NonZero) {
    PrPath* path = prPathCreate();
    prPathAddPolyline(path, &polyline, prAffine2Identity());
    double area = areaOf(path, rule);
    prPathDestroy(path);
    return area;
}

PrDrawBrush brushOf(PrDrawBrushKind kind, const PrDrawColorStop* stops, uint32_t count) {
    PrDrawBrush brush = prDrawBrushDefault(kind);
    brush.stops = stops;
    brush.stopCount = count;
    return brush;
}

bool near(float a, float b, float within = 1e-3f) {
    return std::fabs(a - b) <= within;
}

// Collects a border's pieces: the area that each shape covers before any cut, and its side and color.
struct Pieces {
    std::vector<double> areas;
    std::vector<PrDrawBorderPiece> pieces;
};

void collect(void* context, const PrDrawBorderPiece* piece, const PrPathPolyline* shape, const PrPathPolyline* cut) {
    Pieces& p = *static_cast<Pieces*>(context);
    p.pieces.push_back(*piece);
    p.areas.push_back(areaOf(*shape, piece->rule));
    (void)cut;
}

// Writes the ramp's colors in sRGB at each of `count` points of its parameter, in [-1, 2].
void rampAt(const PrDrawBrush* brush, PrColor color, const float* at, uint32_t count, PrColor* out) {
    std::vector<PrDrawRampPiece> pieces(prDrawGradientRampMaxPieces(brush->stopCount));
    std::vector<PrColor> colors(prDrawGradientRampMaxColors(brush->stopCount, 1024));
    pieces.resize(prDrawGradientRamp(brush, color, PrColorSpace_Srgb, 1024, pieces.data(), colors.data()));
    for (uint32_t i = 0; i < count; ++i)
        out[i] = prDrawRampColor(&pieces[prDrawRampPieceAt(pieces.data(), at[i], 0)], colors.data(), at[i]);
}

// Writes the ramp's colors in sRGB at `count` even steps of its parameter from 0 to 1.
void evenRamp(const PrDrawBrush* brush, PrColor color, uint32_t count, PrColor* out) {
    std::vector<float> at(count);
    for (uint32_t i = 0; i < count; ++i)
        at[i] = float(i) / float(count - 1);
    rampAt(brush, color, at.data(), count, out);
}

// A rectangle whose edges lie between target pixels at a pixel ratio of 1.5.
constexpr PrRect kSnapRect{0.3f, 0.3f, 10.3f, 10.3f};

// Plays the list's commands under a pixel ratio of 1.5 and returns the rectangle of its last Rect command as it plays,
// snapped.
PrRect playedRect(const PrDrawList* list) {
    PrDrawTransform stack[8];
    uint32_t depth = 0;
    stack[0] = prDrawTransformFromAffine2(prAffine2Scale(PrVec2{1.5f, 1.5f}));
    const PrDrawCommand* commands = prDrawListCommands(list);
    PrRect played{};
    PrDrawSnapped storage{};
    for (uint32_t i = 0; i < prDrawListCommandCount(list); ++i) {
        const PrDrawCommand* c = &commands[i];
        if (c->kind == PrDrawCommandKind_PushTransformAffine2) {
            stack[depth + 1] = prDrawTransformPush(&stack[depth], c);
            ++depth;
        } else if (c->kind == PrDrawCommandKind_PopTransform) {
            --depth;
        } else if (c->kind == PrDrawCommandKind_Rect) {
            played = prDrawTransformSnap(&stack[depth], c, &storage)->rect;
        }
    }
    return played;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("gradient parameters follow each kind's geometry") {
    PrDrawColorStop stops[] = {
        {.offset = 0, .color = PrColor{0, 0, 0, 1}},
        {.offset = 1, .color = PrColor{1, 1, 1, 1}},
    };
    PrDrawBrush linear = brushOf(PrDrawBrushKind_Linear, stops, 2);
    linear.linear.start = PrVec2{10, 0};
    linear.linear.end = PrVec2{20, 0};
    float t = 0;
    REQUIRE(prDrawGradientParameter(&linear, PrVec2{15, 99}, &t));
    CHECK(near(t, 0.5f));
    linear.linear.end = linear.linear.start;
    CHECK(!prDrawGradientParameter(&linear, PrVec2{15, 0}, &t));

    PrDrawBrush radial = brushOf(PrDrawBrushKind_Radial, stops, 2);
    radial.radial.end = PrVec2{0, 0};
    radial.radial.endRadius = 10;
    radial.radial.start = PrVec2{0, 0};
    radial.radial.startRadius = 0;
    REQUIRE(prDrawGradientParameter(&radial, PrVec2{3, 4}, &t));
    CHECK(near(t, 0.5f));
    // A cone between two circles paints nothing outside the cone.
    radial.radial.start = PrVec2{-50, 0};
    radial.radial.startRadius = 5;
    CHECK(!prDrawGradientParameter(&radial, PrVec2{-50, 40}, &t));

    PrDrawBrush conic = brushOf(PrDrawBrushKind_Conic, stops, 2);
    conic.conic.center = PrVec2{0, 0};
    conic.conic.angle = 0;
    REQUIRE(prDrawGradientParameter(&conic, PrVec2{0, 5}, &t));
    CHECK(near(t, 0.25f));

    PrDrawBrush diamond = brushOf(PrDrawBrushKind_Diamond, stops, 2);
    diamond.diamond.center = PrVec2{0, 0};
    diamond.diamond.radii = PrVec2{10, 20};
    REQUIRE(prDrawGradientParameter(&diamond, PrVec2{5, -10}, &t));
    CHECK(near(t, 1));
}

TEST_CASE("extending maps the parameter back into the unit interval, except for None") {
    auto extended = [](PrDrawExtend extend, float t) {
        REQUIRE(prDrawGradientExtend(extend, &t));
        return t;
    };
    CHECK(near(extended(PrDrawExtend_Pad, 1.4f), 1.4f));
    CHECK(near(extended(PrDrawExtend_Pad, 5), 2));
    CHECK(near(extended(PrDrawExtend_Pad, -5), -1));
    CHECK(near(extended(PrDrawExtend_Repeat, 1.25f), 0.25f));
    CHECK(near(extended(PrDrawExtend_Repeat, -0.25f), 0.75f));
    CHECK(near(extended(PrDrawExtend_Reflect, 1.25f), 0.75f));
    CHECK(near(extended(PrDrawExtend_Reflect, -0.25f), 0.25f));
    CHECK(near(extended(PrDrawExtend_None, 0.5f), 0.5f));
    float t = 1.25f;
    CHECK(!prDrawGradientExtend(PrDrawExtend_None, &t));
    t = -0.25f;
    CHECK(!prDrawGradientExtend(PrDrawExtend_None, &t));
}

TEST_CASE("a ramp's pieces meet at its stops, and a piece that mixes linearly is its two ends") {
    PrDrawColorStop stops[] = {
        {.offset = 0.25f, .color = PrColor{1, 0, 0, 1}},
        {.offset = 0.5f, .color = PrColor{1, 0, 0, 1}},
        {.offset = 0.5f, .color = PrColor{0, 0, 1, 1}},
        {.offset = 0.75f, .color = PrColor{0, 1, 0, 1}},
    };
    PrDrawBrush brush = brushOf(PrDrawBrushKind_Linear, stops, 4);
    PrDrawRampPiece pieces[7];
    std::vector<PrColor> colors(prDrawGradientRampMaxColors(4, 1024));
    REQUIRE(prDrawGradientRamp(&brush, PrColor{1, 1, 1, 1}, PrColorSpace_Srgb, 1024, pieces, colors.data()) == 6);
    // Red before 0, before the first stop and from it to the hard stop, blue to green, then green to 1 and past it.
    CHECK(pieces[0].start == -1.0f);
    CHECK(pieces[0].end == 0.0f);
    CHECK(pieces[1].start == 0.0f);
    CHECK(pieces[1].end == 0.25f);
    CHECK(pieces[1].count == 1);
    CHECK(pieces[2].end == 0.5f);
    CHECK(pieces[2].count == 1);
    CHECK(pieces[3].start == 0.5f);
    CHECK(pieces[3].count == 2);
    CHECK(pieces[4].end == 1.0f);
    CHECK(pieces[5].end == 2.0f);
    CHECK(prDrawRampColorCount(pieces, 6) == 7);

    // The hard stop is exact on both sides: the later color at its offset, and only the earlier just before it.
    float before = std::nextafter(0.5f, 0.0f);
    PrColor red = prDrawRampColor(&pieces[prDrawRampPieceAt(pieces, before, 0)], colors.data(), before);
    PrColor blue = prDrawRampColor(&pieces[prDrawRampPieceAt(pieces, 0.5f, 0)], colors.data(), 0.5f);
    CHECK(red.r == 1.0f);
    CHECK(red.b == 0.0f);
    CHECK(blue.b == 1.0f);
    CHECK(blue.r == 0.0f);
    PrColor middle = prDrawRampColor(&pieces[prDrawRampPieceAt(pieces, 0.625f, 4)], colors.data(), 0.625f);
    CHECK(near(middle.b, 0.5f));
    CHECK(near(middle.g, 0.5f));
}

TEST_CASE("a ramp holds 1 in the gradient's colors, and its end stops' colors before 0 and past 1") {
    PrDrawColorStop stops[] = {
        {.offset = 0, .color = PrColor{1, 0, 0, 1}},
        {.offset = 0, .color = PrColor{0, 0, 1, 1}},
        {.offset = 1, .color = PrColor{0, 0, 1, 1}},
        {.offset = 1, .color = PrColor{0, 1, 0, 1}},
    };
    PrDrawBrush brush = brushOf(PrDrawBrushKind_Radial, stops, 4);
    const float at[] = {-0.5f, 0, 1, 1.5f};
    PrColor ramp[4];
    rampAt(&brush, PrColor{1, 1, 1, 1}, at, 4, ramp);
    CHECK(ramp[0].r == 1.0f);
    CHECK(ramp[1].b == 1.0f);
    CHECK(ramp[2].b == 1.0f);
    CHECK(ramp[3].g == 1.0f);
    CHECK(ramp[3].b == 0.0f);
}

TEST_CASE("a ramp's piece that mixes in another space has colors at the resolution asked") {
    PrDrawColorStop stops[] = {
        {.offset = 0, .color = PrColor{0, 0, 0, 1}},
        {.offset = 0.5f, .color = PrColor{1, 1, 1, 1}},
    };
    PrDrawBrush brush = brushOf(PrDrawBrushKind_Linear, stops, 2);
    brush.interpolationSpace = PrColorSpace_SrgbLinear;
    PrDrawRampPiece pieces[5];
    std::vector<PrColor> colors(prDrawGradientRampMaxColors(2, 64));
    REQUIRE(prDrawGradientRamp(&brush, PrColor{1, 1, 1, 1}, PrColorSpace_Srgb, 64, pieces, colors.data()) == 4);
    CHECK(pieces[1].count == 33);
    CHECK(pieces[2].count == 1);
}

TEST_CASE("a ramp mixes stops premultiplied, in their space, and takes the color") {
    PrDrawColorStop stops[] = {
        {.offset = 0, .color = PrColor{1, 0, 0, 1}},
        {.offset = 1, .color = PrColor{0, 0, 1, 0}},
    };
    PrDrawBrush brush = brushOf(PrDrawBrushKind_Linear, stops, 2);
    PrColor ramp[3];
    evenRamp(&brush, PrColor{1, 1, 1, 1}, 3, ramp);
    // Because the mix is premultiplied, the transparent blue adds nothing to the color.
    CHECK(near(ramp[1].r, 0.5f));
    CHECK(near(ramp[1].b, 0));
    CHECK(near(ramp[1].a, 0.5f));
    evenRamp(&brush, PrColor{1, 1, 1, 0.5f}, 3, ramp);
    CHECK(near(ramp[0].a, 0.5f));
    CHECK(near(ramp[0].r, 0.5f));

    // A hint of a quarter makes the middle of the gap mostly the second stop's color.
    PrDrawColorStop hinted[] = {
        {.offset = 0, .color = PrColor{0, 0, 0, 1}, .hint = 0.25f},
        {.offset = 1, .color = PrColor{1, 1, 1, 1}},
    };
    brush = brushOf(PrDrawBrushKind_Linear, hinted, 2);
    evenRamp(&brush, PrColor{1, 1, 1, 1}, 3, ramp);
    CHECK(near(ramp[1].r, std::pow(0.5f, std::log(0.5f) / std::log(0.25f))));

    // Equal offsets make a hard stop, with the later stop's color at the offset.
    PrDrawColorStop hard[] = {
        {.offset = 0, .color = PrColor{1, 0, 0, 1}},
        {.offset = 0.5f, .color = PrColor{1, 0, 0, 1}},
        {.offset = 0.5f, .color = PrColor{0, 0, 1, 1}},
        {.offset = 1, .color = PrColor{0, 0, 1, 1}},
    };
    brush = brushOf(PrDrawBrushKind_Linear, hard, 4);
    evenRamp(&brush, PrColor{1, 1, 1, 1}, 3, ramp);
    CHECK(near(ramp[1].b, 1));

    // In linear light, the middle of black and white is brighter than in sRGB.
    PrDrawColorStop grays[] = {
        {.offset = 0, .color = PrColor{0, 0, 0, 1}},
        {.offset = 1, .color = PrColor{1, 1, 1, 1}},
    };
    brush = brushOf(PrDrawBrushKind_Linear, grays, 2);
    brush.interpolationSpace = PrColorSpace_SrgbLinear;
    evenRamp(&brush, PrColor{1, 1, 1, 1}, 3, ramp);
    CHECK(near(ramp[1].r, 0.7354f, 1e-3f));
}

TEST_CASE("a ramp that mixes straight mixes color and alpha apart, toward the color of a transparent stop") {
    PrDrawColorStop stops[] = {
        {.offset = 0, .color = PrColor{1, 0.5f, 0, 1}},
        {.offset = 1, .color = PrColor{0, 0, 0, 0}},
    };
    PrDrawBrush brush = brushOf(PrDrawBrushKind_Linear, stops, 2);
    brush.interpolationAlpha = PrDrawAlpha_Straight;
    PrColor ramp[3];
    evenRamp(&brush, PrColor{1, 1, 1, 1}, 3, ramp);
    // Half way, the color is half way to black at half alpha: premultiplied, a quarter of the first stop's.
    CHECK(near(ramp[1].a, 0.5f));
    CHECK(near(ramp[1].r, 0.25f, 1e-2f));
    CHECK(near(ramp[1].g, 0.125f, 1e-2f));
    // The piece is not linear in the premultiplied output, so it has a color at each step.
    PrDrawRampPiece pieces[5];
    std::vector<PrColor> colors(prDrawGradientRampMaxColors(2, 64));
    REQUIRE(prDrawGradientRamp(&brush, PrColor{1, 1, 1, 1}, PrColorSpace_Srgb, 64, pieces, colors.data()) == 3);
    CHECK(pieces[1].count == 65);
    // Two stops of one alpha mix linearly, as premultiplied stops do.
    stops[1].color = PrColor{0, 0, 0, 1};
    REQUIRE(prDrawGradientRamp(&brush, PrColor{1, 1, 1, 1}, PrColorSpace_Srgb, 64, pieces, colors.data()) == 3);
    CHECK(pieces[1].count == 2);
    // The way stops mix decides the ramp, so it keys a cached ramp.
    PrDrawBrush premultiplied = brush;
    premultiplied.interpolationAlpha = PrDrawAlpha_Premultiplied;
    CHECK(!prDrawGradientRampEquals(&brush, PrColor{1, 1, 1, 1}, &premultiplied, PrColor{1, 1, 1, 1}));
}

TEST_CASE("hues interpolate the way asked, and a gray takes the hue of each stop it mixes with") {
    PrDrawColorStop stops[] = {
        {.offset = 0, .color = PrColor{1, 0, 0, 1}},
        {.offset = 1, .color = PrColor{0, 0, 1, 1}},
    };
    PrDrawBrush brush = brushOf(PrDrawBrushKind_Linear, stops, 2);
    brush.interpolationSpace = PrColorSpace_Hsl;
    PrColor ramp[3];
    // Red (0) to blue (240): the shorter way passes magenta (300), the longer way green (120).
    evenRamp(&brush, PrColor{1, 1, 1, 1}, 3, ramp);
    CHECK(near(ramp[1].r, 1));
    CHECK(near(ramp[1].b, 1));
    brush.hueMethod = PrColorHueMethod_Longer;
    evenRamp(&brush, PrColor{1, 1, 1, 1}, 3, ramp);
    CHECK(near(ramp[1].g, 1));
    PrDrawColorStop fromGray[] = {
        {.offset = 0, .color = PrColor{0.5f, 0.5f, 0.5f, 1}},
        {.offset = 1, .color = PrColor{0, 0, 1, 1}},
    };
    brush = brushOf(PrDrawBrushKind_Linear, fromGray, 2);
    brush.interpolationSpace = PrColorSpace_Hsl;
    evenRamp(&brush, PrColor{1, 1, 1, 1}, 3, ramp);
    CHECK(ramp[1].b > ramp[1].r + 0.1f);
    CHECK(near(ramp[1].r, ramp[1].g));
    // Between red and gray, the gray takes red's hue. Between gray and green, it takes green's hue.
    PrDrawColorStop throughGray[] = {
        {.offset = 0, .color = PrColor{1, 0, 0, 1}},
        {.offset = 0.5f, .color = PrColor{0.5f, 0.5f, 0.5f, 1}},
        {.offset = 1, .color = PrColor{0, 1, 0, 1}},
    };
    brush = brushOf(PrDrawBrushKind_Linear, throughGray, 3);
    brush.interpolationSpace = PrColorSpace_Hsl;
    PrColor five[5];
    evenRamp(&brush, PrColor{1, 1, 1, 1}, 5, five);
    CHECK(five[1].r > five[1].g + 0.1f);
    CHECK(near(five[1].g, five[1].b));
    CHECK(five[3].g > five[3].r + 0.1f);
    CHECK(near(five[3].r, five[3].b));

    // A gray with a stated hue keeps it: from a gray of hue 0 (red) to blue (240), the shorter way passes magenta.
    fromGray[0].hue = 0;
    fromGray[0].hasHue = true;
    brush = brushOf(PrDrawBrushKind_Linear, fromGray, 2);
    brush.interpolationSpace = PrColorSpace_Hsl;
    evenRamp(&brush, PrColor{1, 1, 1, 1}, 3, ramp);
    CHECK(ramp[1].r > ramp[1].g + 0.1f);
    CHECK(near(ramp[1].r, ramp[1].b));
}

TEST_CASE("gradients share a ramp when their stops, interpolation and color agree, whatever their geometry") {
    PrDrawColorStop stops[] = {
        {.offset = 0, .color = PrColor{1, 0, 0, 1}},
        {.offset = 1, .color = PrColor{0, 0, 1, 1}},
    };
    PrDrawColorStop copy[] = {{.offset = 0, .color = PrColor{1, 0, 0, 1}}, {.offset = 1, .color = PrColor{0, 0, 1, 1}}};
    PrDrawBrush linear = brushOf(PrDrawBrushKind_Linear, stops, 2);
    linear.linear.end = PrVec2{10, 0};
    PrDrawBrush radial = brushOf(PrDrawBrushKind_Radial, copy, 2);
    radial.radial.endRadius = 5;
    radial.extend = PrDrawExtend_Repeat;
    const PrColor white{1, 1, 1, 1};
    CHECK(prDrawGradientRampHash(&linear, white) == prDrawGradientRampHash(&radial, white));
    CHECK(prDrawGradientRampEquals(&linear, white, &radial, white));
    CHECK(!prDrawGradientRampEquals(&linear, white, &radial, PrColor{1, 1, 1, 0.5f}));
    radial.interpolationSpace = PrColorSpace_Oklab;
    CHECK(!prDrawGradientRampEquals(&linear, white, &radial, white));
    radial.interpolationSpace = linear.interpolationSpace;
    copy[1].offset = 0.5f;
    CHECK(!prDrawGradientRampEquals(&linear, white, &radial, white));
}

TEST_CASE("an aligned stroke is twice the width cut to the shape's inside or outside") {
    PrPath* square = prPathCreate();
    prPathAddRect(square, PrRect{20, 20, 60, 60});
    PrDrawStroke stroke = prDrawStrokeDefault();
    stroke.shape.width = 4;
    stroke.align = PrDrawStrokeAlign_Inside;
    PrPathPolyline fill{};
    PrPathPolyline aligned{};
    PrPathPolyline region{};
    const PrAffine2 identity = prAffine2Identity();
    const PrAffine2 doubled = prAffine2Scale(PrVec2{2, 2});
    prPathPolylineClear(&fill);
    prPathPolylineClear(&aligned);
    prPathPolylineClear(&region);
    prDrawLowerStroke(square, &stroke, identity, 0.01f, &fill, &aligned, &region);
    CHECK(fill.pointCount == 0);
    CHECK(areaOf(aligned) == doctest::Approx(48 * 48 - 32 * 32).epsilon(0.002));
    CHECK(areaOf(region) == doctest::Approx(40 * 40).epsilon(0.002));
    // Under a similarity, a centered stroke's width scales with the shape.
    stroke.align = PrDrawStrokeAlign_Center;
    prPathPolylineClear(&fill);
    prPathPolylineClear(&aligned);
    prPathPolylineClear(&region);
    prDrawLowerStroke(square, &stroke, doubled, 0.01f, &fill, &aligned, &region);
    CHECK(areaOf(fill) == doctest::Approx(88 * 88 - 72 * 72).epsilon(0.002));
    // A non-scaling stroke keeps its width in target pixels, whatever the transform.
    stroke.shape.width = 1;
    stroke.nonScaling = true;
    prPathPolylineClear(&fill);
    prPathPolylineClear(&aligned);
    prPathPolylineClear(&region);
    prDrawLowerStroke(square, &stroke, doubled, 0.01f, &fill, &aligned, &region);
    CHECK(areaOf(fill) == doctest::Approx(81 * 81 - 79 * 79).epsilon(0.002));
    prPathDestroy(square);
    prPathPolylineReset(&fill);
    prPathPolylineReset(&aligned);
    prPathPolylineReset(&region);
}

TEST_CASE("the caps of a contour of no length turn with the transform") {
    PrPath* dot = prPathCreate();
    prPathMoveTo(dot, PrVec2{50, 50});
    prPathLineTo(dot, PrVec2{50, 50});
    PrDrawStroke stroke = prDrawStrokeDefault();
    stroke.shape.width = 20;
    stroke.shape.startCap = PrPathCap_Square;
    stroke.shape.endCap = PrPathCap_Square;
    PrPathPolyline fill{};
    PrPathPolyline aligned{};
    PrPathPolyline region{};
    // A rotation is a similarity, which strokes in target space: the square is turned 45° there and spans its diagonal.
    PrAffine2 turned = prAffine2Concat(prAffine2Rotate(0.78539816f), prAffine2Translate(PrVec2{20, -20}));
    prDrawLowerStroke(dot, &stroke, turned, 0.01f, &fill, &aligned, &region);
    CHECK(prRectWidth(prPathPolylineBounds(&fill)) == doctest::Approx(20 * 1.41421356).epsilon(0.001));
    CHECK(areaOf(fill) == doctest::Approx(400).epsilon(0.002));
    // A skew strokes in the path's space and maps the outline.
    prPathPolylineClear(&fill);
    PrAffine2 skew = prAffine2Identity();
    skew.c = 1;
    prDrawLowerStroke(dot, &stroke, skew, 0.01f, &fill, &aligned, &region);
    CHECK(prRectWidth(prPathPolylineBounds(&fill)) == doctest::Approx(40).epsilon(0.001));
    prPathDestroy(dot);
    prPathPolylineReset(&fill);
    prPathPolylineReset(&aligned);
    prPathPolylineReset(&region);
}

TEST_CASE("a stroke's reach is a multiple of its width, twice as far when aligned") {
    PrDrawStroke stroke = prDrawStrokeDefault();
    stroke.shape.width = 4;
    stroke.shape.miterLimit = 3;
    stroke.shape.join = PrPathJoin_Round;
    CHECK(prDrawStrokeReach(&stroke) == doctest::Approx(0.5));
    stroke.shape.join = PrPathJoin_Miter;
    CHECK(prDrawStrokeReach(&stroke) == doctest::Approx(1.5));
    stroke.align = PrDrawStrokeAlign_Outside;
    CHECK(prDrawStrokeReach(&stroke) == doctest::Approx(3));
}

TEST_CASE("fill bounds exclude the stroke and hold a turned ellipse") {
    PrDrawCommand command{};
    command.kind = PrDrawCommandKind_Ellipse;
    command.ellipse.center = PrVec2{50, 50};
    command.ellipse.radii = PrVec2{20, 10};
    command.ellipse.rotation = float(kPi / 2);
    PrRect bounds = prDrawFillBounds(&command);
    CHECK(near(bounds.left, 40));
    CHECK(near(bounds.top, 30));
    CHECK(near(bounds.right, 60));
    CHECK(near(bounds.bottom, 70));
}

TEST_CASE("a shadow grows by its spread, and a small radius grows by less") {
    PrDrawShadow shadow{};
    shadow.shape.rect = PrRect{10, 10, 50, 50};
    shadow.offset = PrVec2{3, 4};
    shadow.sigma = 5;
    shadow.spread = 8;
    shadow.shape.corners[0] = PrCorner{{16, 16}, 1};
    shadow.shape.corners[1] = PrCorner{{4, 4}, 1};
    PrDrawShadowGeometry g = prDrawShadowGeometry(&shadow, PrDrawShadowKind_Drop);
    CHECK(near(g.shape.rect.left, 5));
    CHECK(near(g.shape.rect.top, 6));
    CHECK(near(g.shape.rect.right, 61));
    CHECK(near(g.sigma, 5));
    CHECK(near(g.shape.corners[0].radii.x, 24));
    CHECK(near(g.shape.corners[1].radii.x, 4 + 8 * (1 + std::pow(0.5f - 1, 3.0f))));
    CHECK(near(g.shape.corners[2].radii.x, 0));
    g = prDrawShadowGeometry(&shadow, PrDrawShadowKind_Inner);
    CHECK(near(g.shape.rect.left, 21));
    CHECK(near(g.shape.corners[0].radii.x, 8));
    CHECK(near(g.shape.corners[1].radii.x, 0));
}

TEST_CASE("decorations lower to their bands, dots and dashes") {
    PrDrawDecoration d{};
    d.origin = PrVec2{10, 50};
    d.length = 100;
    d.thickness = 4;
    PrPathPolyline outline{};
    d.style = PrDrawDecorationStyle_Solid;
    prPathPolylineClear(&outline);
    prDrawDecorationOutline(&d, 0.01f, &outline);
    CHECK(areaOf(outline) == doctest::Approx(400).epsilon(0.002));
    d.style = PrDrawDecorationStyle_Double;
    prPathPolylineClear(&outline);
    prDrawDecorationOutline(&d, 0.01f, &outline);
    CHECK(areaOf(outline) == doctest::Approx(800).epsilon(0.002));
    d.style = PrDrawDecorationStyle_Dashed;
    prPathPolylineClear(&outline);
    prDrawDecorationOutline(&d, 0.01f, &outline);
    // Five periods of 20, each with a dash of 12.
    CHECK(areaOf(outline) == doctest::Approx(5 * 12 * 4).epsilon(0.002));
    d.style = PrDrawDecorationStyle_Dotted;
    prPathPolylineClear(&outline);
    prDrawDecorationOutline(&d, 0.01f, &outline);
    CHECK(areaOf(outline) == doctest::Approx(13 * kPi * 4).epsilon(0.01));
    prPathPolylineReset(&outline);
}

TEST_CASE("a wavy decoration is its thickness along the sine, in points that follow the tolerance") {
    PrDrawDecoration d{};
    d.origin = PrVec2{10, 50};
    d.length = 100;
    d.thickness = 4;
    d.style = PrDrawDecorationStyle_Wavy;
    d.wavelength = 20;
    d.amplitude = 5;
    // The length of the sine over the decoration. Its slope is the amplitude times k times the cosine.
    const double k = 2 * kPi / 20;
    double arc = 0;
    for (int i = 0; i < 100000; ++i) {
        double slope = 5 * k * std::cos(k * (double(i) + 0.5) / 1000);
        arc += std::sqrt(1 + slope * slope) / 1000;
    }
    PrPathPolyline outline{};
    prDrawDecorationOutline(&d, 0.01f, &outline);
    CHECK(areaOf(outline) == doctest::Approx(4 * arc).epsilon(0.01));
    uint32_t fine = outline.pointCount;
    prPathPolylineClear(&outline);
    prDrawDecorationOutline(&d, 0.25f, &outline);
    CHECK(areaOf(outline) == doctest::Approx(4 * arc).epsilon(0.02));
    // A coarser tolerance uses fewer points: five periods at a quarter pixel need fewer than forty points per period.
    CHECK(outline.pointCount < fine);
    CHECK(outline.pointCount < 5 * 40);
    prPathPolylineReset(&outline);
}

TEST_CASE("a decoration too long for its pattern draws a bounded number of dots and dashes") {
    PrPathPolyline outline{};
    for (float length : {1e30f, INFINITY}) {
        PrDrawDecoration d{};
        d.length = length;
        d.thickness = 1;
        d.wavelength = 4;
        d.amplitude = 1;
        for (PrDrawDecorationStyle style : {PrDrawDecorationStyle_Dotted, PrDrawDecorationStyle_Dashed,
            PrDrawDecorationStyle_Wavy}) {
            d.style = style;
            prPathPolylineClear(&outline);
            prDrawDecorationOutline(&d, 0.25f, &outline);
            // The outline has at most 4096 dots or dashes, or a wave of at most 65536 lines stroked on both sides.
            CHECK(outline.pointCount <= (style == PrDrawDecorationStyle_Wavy ? 4 * 65537 : 4096 * 16));
        }
    }
    prPathPolylineReset(&outline);
}

TEST_CASE("a nine-slice maps corners whole and tiles its edges by their modes") {
    PrDrawNineSlice n{};
    for (uint32_t i = 0; i < 4; ++i) {
        n.slices[i] = 10;
        n.widths[i] = 10;
    }
    n.center = true;
    PrDrawImageRect rects[64];
    uint32_t count = prDrawNineSliceRects(&n, PrVec2{30, 30}, PrRect{0, 0, 100, 50}, rects, 64);
    CHECK(count == 9);
    CHECK(near(rects[0].destination.right, 10));
    CHECK(near(rects[0].source.right, 10));
    // Round fits whole copies: the top edge, 80 long, fits eight copies of 10.
    n.horizontal = PrDrawSliceMode_Round;
    n.vertical = PrDrawSliceMode_Stretch;
    n.center = false;
    count = prDrawNineSliceRects(&n, PrVec2{30, 30}, PrRect{0, 0, 100, 50}, rects, 64);
    CHECK(count == 4 + 8 + 8 + 2);
    // Space leaves even gaps: 85 fits eight copies, with 5 / 9 between them.
    n.horizontal = PrDrawSliceMode_Space;
    count = prDrawNineSliceRects(&n, PrVec2{30, 30}, PrRect{0, 0, 105, 50}, rects, 64);
    CHECK(count == 4 + 8 + 8 + 2);
    CHECK(near(rects[4].destination.left, 10 + 5.0f / 9));
    // Widths that exceed the destination scale down together.
    for (uint32_t i = 0; i < 4; ++i)
        n.widths[i] = 40;
    n.horizontal = PrDrawSliceMode_Stretch;
    count = prDrawNineSliceRects(&n, PrVec2{30, 30}, PrRect{0, 0, 60, 60}, rects, 64);
    CHECK(near(rects[0].destination.right, 30));
    CHECK(prDrawNineSliceRects(&n, PrVec2{30, 30}, PrRect{0, 0, 60, 60}, nullptr, 0) == count);
}

TEST_CASE("a uniform border is one area, and sides that differ are pieces of their own") {
    PrDrawBorder border{};
    for (PrDrawBorderSide& side : border.sides)
        side = PrDrawBorderSide{10, PrDrawBorderStyle_Solid, PrColor{0, 0, 1, 1}, nullptr, 0};
    Pieces p;
    border.shape.rect = PrRect{10, 10, 110, 70};
    prDrawBorderLower(&border, 0.01f, collect, &p);
    REQUIRE(p.pieces.size() == 1);
    CHECK(p.areas[0] == doctest::Approx(100 * 60 - 80 * 40).epsilon(0.002));

    border.sides[1].color = PrColor{1, 0, 0, 1};
    border.sides[3].width = 20;
    p = Pieces{};
    border.shape.rect = PrRect{10, 10, 110, 70};
    prDrawBorderLower(&border, 0.01f, collect, &p);
    REQUIRE(p.pieces.size() == 4);
    double total = 0;
    for (double area : p.areas)
        total += area;
    CHECK(total == doctest::Approx(100 * 60 - 70 * 40).epsilon(0.002));
    CHECK(p.pieces[1].side == PrRectSide_Right);

    // Inset shades the top and left: a quarter of the area has 0.55 of the color.
    for (PrDrawBorderSide& side : border.sides)
        side = PrDrawBorderSide{10, PrDrawBorderStyle_Inset, PrColor{1, 1, 1, 1}, nullptr, 0};
    p = Pieces{};
    border.shape.rect = PrRect{10, 10, 110, 70};
    prDrawBorderLower(&border, 0.01f, collect, &p);
    REQUIRE(p.pieces.size() == 4);
    CHECK(near(p.pieces[0].color.r, kPrDrawBorderShade));
    CHECK(near(p.pieces[2].color.r, 1));
}

TEST_CASE("sides whose brushes are the same record, with stops of the same values, are uniform") {
    PrDrawColorStop stops[2][2] = {
        {{.offset = 0, .color = PrColor{1, 0, 0, 1}}, {.offset = 1, .color = PrColor{0, 0, 1, 1}}},
        {{.offset = 0, .color = PrColor{1, 0, 0, 1}}, {.offset = 1, .color = PrColor{0, 0, 1, 1}}},
    };
    PrDrawBrush brushes[2] = {
        brushOf(PrDrawBrushKind_Linear, stops[0], 2),
        brushOf(PrDrawBrushKind_Linear, stops[1], 2),
    };
    for (PrDrawBrush& brush : brushes)
        brush.linear = {{0, 0}, {10, 0}};
    PrDrawBorder border{};
    for (uint32_t i = 0; i < 4; ++i)
        border.sides[i] = PrDrawBorderSide{10, PrDrawBorderStyle_Solid, PrColor{1, 1, 1, 1}, &brushes[i % 2], 0};
    CHECK(prDrawBorderUniformSide(&border) != nullptr);
    stops[1][1].color.g = prFloat16FromFloat(1);
    CHECK(prDrawBorderUniformSide(&border) == nullptr);
    stops[1][1].color.g = prFloat16FromFloat(0);
    brushes[1].linear.end.x = 20;
    CHECK(prDrawBorderUniformSide(&border) == nullptr);
}

TEST_CASE("a border with a side that paints nothing is not one area, whether the side has no width or no style") {
    PrDrawBorder border{};
    for (PrDrawBorderSide& side : border.sides)
        side = PrDrawBorderSide{1, PrDrawBorderStyle_Solid, PrColor{0, 1, 0, 1}, nullptr, 0};
    CHECK(prDrawBorderUniformSide(&border) == &border.sides[0]);
    // A solid side of no width draws as a side of no style does.
    border.sides[PrRectSide_Top].width = 0;
    CHECK(prDrawBorderUniformSide(&border) == nullptr);
    border.sides[PrRectSide_Top] = PrDrawBorderSide{1, PrDrawBorderStyle_None, PrColor{0, 1, 0, 1}, nullptr, 0};
    CHECK(prDrawBorderUniformSide(&border) == nullptr);
}

TEST_CASE("a transform is degenerate as the pushes compose, and stays so inside") {
    PrDrawTransform root = prDrawTransformFromAffine2(prAffine2Identity());
    CHECK(!root.degenerate);
    PrDrawCommand push{};
    push.kind = PrDrawCommandKind_PushTransformAffine2;
    push.affine = prDAffine2FromAffine2(prAffine2Scale(PrVec2{1e-30f, 1e-30f}));
    PrDrawTransform tiny = prDrawTransformPush(&root, &push);
    CHECK(!tiny.degenerate);
    // Each push is invertible, and their composition underflows to nothing.
    PrDrawTransform under = prDrawTransformPush(&tiny, &push);
    CHECK(under.degenerate);
    push.affine = prDAffine2FromAffine2(prAffine2Scale(PrVec2{1e30f, 1e30f}));
    CHECK(prDrawTransformPush(&under, &push).degenerate);
    push.affine = PrDAffine2{1, 2, 2, 4, 0, 0};
    CHECK(prDrawTransformPush(&root, &push).degenerate);

}

TEST_CASE("a list's texture is its area at its scale in target pixels, fitted within the limit with its aspect") {
    PrUVec2 layer = prDrawLayerExtent(PrVec2{10, 5}, PrVec2{2, 2}, 8192);
    CHECK(layer.x == 20);
    CHECK(layer.y == 10);
    layer = prDrawLayerExtent(PrVec2{100, 50}, PrVec2{200, 200}, 8192);
    CHECK(layer.x == 8192);
    CHECK(layer.y == 4096);
    layer = prDrawLayerExtent(PrVec2{0, 5}, PrVec2{2, 2}, 8192);
    CHECK(layer.x == 0);
    CHECK(layer.y == 0);
}

TEST_CASE("a wide shadow blurs at a lower resolution, close to the full blur, whatever its sigma") {
    PrPathRasterizer* rasterizer = prPathRasterizerCreate();
    PrPath* shape = prPathCreate();
    prPathAddRect(shape, PrRect{100, 100, 200, 160});
    PrPathPolyline outline{};
    prPathFlatten(shape, prAffine2Identity(), 0.05f, PrPathFlattening_Balanced, &outline);
    PrPathPolyline caster{};
    const PrIRect box{0, 0, 300, 260};
    std::vector<uint8_t> reduced(300 * 260);
    prDrawShadowCoverage(rasterizer, &outline, &caster, PrDrawShadowKind_Drop, 40, PrPathCoverage_Area,
        PrIVec2{box.left, box.top}, PrPixels{reduced.data(), 300, {300, 260}});
    // The same blur at full resolution, over enough room that nothing is cut off.
    const uint32_t margin = 200;
    const uint32_t width = 300 + 2 * margin;
    const uint32_t height = 260 + 2 * margin;
    std::vector<uint8_t> coverage(size_t(width) * height);
    PrPixels mask{coverage.data(), width, {width, height}};
    prPathRasterizerFill(rasterizer, &outline, PrPathFillRule_NonZero, PrPathCoverage_Area,
        PrIVec2{-int32_t(margin), -int32_t(margin)}, mask);
    std::vector<float> values(coverage.begin(), coverage.end());
    PrPixels blurred{reinterpret_cast<uint8_t*>(values.data()), width * sizeof(float), {width, height}};
    prImageBlur(blurred, PrPixelFormat_RF32, PrVec2{40, 40}, PrImageEdgeMode_None);
    int worst = 0;
    for (uint32_t y = 0; y < 260; ++y) {
        for (uint32_t x = 0; x < 300; ++x) {
            int exact = int(values[(y + margin) * width + x + margin] + 0.5f);
            int d = std::abs(int(reduced[y * 300 + x]) - exact);
            worst = d > worst ? d : worst;
        }
    }
    CHECK(worst <= 4);
    // A sigma far wider than the box blurs to a faint wash over it. The blur is computed over an area no larger than
    // the box.
    std::vector<uint8_t> wash(64 * 64);
    prPathClear(shape);
    prPathAddRect(shape, PrRect{0, 0, 64, 64});
    prPathPolylineClear(&outline);
    prPathFlatten(shape, prAffine2Identity(), 0.05f, PrPathFlattening_Balanced, &outline);
    prDrawShadowCoverage(rasterizer, &outline, &caster, PrDrawShadowKind_Drop, 1e6f, PrPathCoverage_Area,
        PrIVec2{0, 0}, PrPixels{wash.data(), 64, {64, 64}});
    CHECK(wash[0] <= 1);
    CHECK(wash[64 * 32 + 32] <= 1);
    prPathDestroy(shape);
    prPathPolylineReset(&outline);
    prPathPolylineReset(&caster);
    prPathRasterizerDestroy(rasterizer);
}

TEST_CASE("snapped edges round to target pixels where they land, apart from a transform pushed apart") {
    // At 1.5 target pixels per unit, 0.3 lands at 0.45 and rounds to 0, and 10.3 lands at 15.45 and rounds to 15.
    PrDrawList* list = prDrawListCreate();
    prDrawListRect(list, kSnapRect, {.color = PrColor{1, 1, 1, 1}, .edges = PrDrawEdges_Snapped});
    PrRect r = playedRect(list);
    CHECK(near(r.left, 0));
    CHECK(near(r.right, 10));
    prDrawListRelease(list);
    // Inside a translation by 0.2 pushed apart, the edges round where they would lie without it, and then move with it.
    list = prDrawListCreate();
    prDrawListPushTransformSnapped(list, prDAffine2Translate(0.2, 0), PrDrawSnapFlag_Apart);
    prDrawListRect(list, kSnapRect, {.color = PrColor{1, 1, 1, 1}, .edges = PrDrawEdges_Snapped});
    prDrawListPopTransform(list);
    r = playedRect(list);
    CHECK(near(r.left, 0));
    CHECK(near(r.right, 10));
    prDrawListRelease(list);
}
