#include "pr/float.h"
#include "pr/geom.h"
#include "pr/vec.h"

#include "tools/harness/test.h"

extern "C" int prTestGeomCApi(void);

namespace pr_impl {

namespace {

bool near(float a, float b) {
    float difference = a - b;
    return difference < 1e-5f && difference > -1e-5f;
}

bool intRectEquals(PrIRect a, PrIRect b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

bool nearPoint(PrVec2 a, PrVec2 b) {
    return near(a.x, b.x) && near(a.y, b.y);
}

bool nearRect(PrRect a, PrRect b) {
    return near(a.left, b.left) && near(a.top, b.top) && near(a.right, b.right) && near(a.bottom, b.bottom);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("the public header compiles as C") {
    CHECK(prTestGeomCApi() == 0);
}

TEST_CASE("rectangles are empty unless they have area") {
    CHECK(!prRectIsEmpty(PrRect{0, 0, 1, 1}));
    CHECK(prRectIsEmpty(PrRect{0, 0, 0, 1}));
    CHECK(prRectIsEmpty(PrRect{2, 0, 1, 1}));
    CHECK(prRectIsEmpty(PrRect{0, 0, kPrF32Nan, 1}));
}

TEST_CASE("union ignores an empty rectangle and intersection can be empty") {
    PrRect a{0, 0, 10, 10};
    PrRect b{5, 5, 20, 8};
    CHECK(nearRect(prRectUnion(a, b), PrRect{0, 0, 20, 10}));
    CHECK(nearRect(prRectUnion(a, PrRect{30, 30, 30, 40}), a));
    CHECK(nearRect(prRectIntersect(a, b), PrRect{5, 5, 10, 8}));
    CHECK(prRectIsEmpty(prRectIntersect(a, PrRect{11, 0, 12, 1})));
}

TEST_CASE("a rectangle covers the pixels it reaches past a thousandth, and contains the centers inside it") {
    CHECK(intRectEquals(prIRectRoundOut(PrRect{0.5f, 1, 2.2f, 3.0004f}), PrIRect{0, 1, 3, 3}));
    CHECK(intRectEquals(prIRectRoundOut(PrRect{0.9996f, 0, 2, 1}), PrIRect{1, 0, 2, 1}));
    CHECK(intRectEquals(prIRectRound(PrRect{0.4f, 0.6f, 2.5f, 3}), PrIRect{0, 1, 2, 3}));
    CHECK(intRectEquals(prIRectRoundOut(PrRect{2, 0, 1, 1}), PrIRect{0, 0, 0, 0}));
    CHECK(intRectEquals(prIRectRoundOut(PrRect{0, 0, kPrF32Nan, 1}), PrIRect{0, 0, 0, 0}));
    PrIRect far = prIRectRoundOut(PrRect{-kPrF32Infinity, 0, kPrF32Infinity, 1});
    CHECK(prIRectWidth(prIRectOutset(far, 1)) == (1u << 29) + 2);
    CHECK(prRectIsOnPixels(PrRect{0.9996f, 1, 3.0004f, 4}));
    CHECK(!prRectIsOnPixels(PrRect{0.99f, 1, 3, 4}));
}

TEST_CASE("integer rectangles intersect to all zero where they do not overlap, and unite ignoring empties") {
    PrIRect a{0, 0, 4, 4};
    PrIRect b{2, 2, 6, 6};
    CHECK(intRectEquals(prIRectIntersect(a, b), PrIRect{2, 2, 4, 4}));
    CHECK(prIRectIsEmpty(prIRectIntersect(a, PrIRect{5, 5, 6, 6})));
    CHECK(intRectEquals(prIRectUnion(a, b), PrIRect{0, 0, 6, 6}));
    CHECK(intRectEquals(prIRectUnion(PrIRect{9, 9, 9, 10}, b), b));
    CHECK(prIRectContains(a, PrIRect{1, 1, 4, 4}));
    CHECK(!prIRectContains(a, b));
    CHECK(!prIRectContains(a, PrIRect{1, 1, 1, 1}));
    CHECK(prIRectHeight(PrIRect{0, 3, 1, 1}) == 0);
}

TEST_CASE("fitting round corners scales every radius by the tightest side, and squares an axis of zero or less") {
    PrCornerRect shape{
        PrRect{0, 0, 100, 50},
        {
            prCornerRound(PrVec2{60, 10}), prCornerRound(PrVec2{60, 10}), prCornerRound(PrVec2{10, 40}),
            prCornerRound(PrVec2{10, 40}),
        },
    };
    PrCornerRect fitted = prCornerRectFit(shape);
    // The right side needs 10 + 40 of 50, and the top 60 + 60 of 100. The top sets the factor.
    float factor = 100.0f / 120.0f;
    CHECK(nearPoint(fitted.corners[PrRectCorner_TopLeft].radii, PrVec2{60 * factor, 10 * factor}));
    CHECK(nearPoint(fitted.corners[PrRectCorner_BottomRight].radii, PrVec2{10 * factor, 40 * factor}));

    PrCornerRect square{
        PrRect{0, 0, 10, 10},
        {
            prCornerRound(PrVec2{0, 5}), prCornerRound(PrVec2{2, 2}), prCornerRound(PrVec2{-1, 3}),
            prCornerRound(PrVec2{2, 2}),
        },
    };
    PrCornerRect squared = prCornerRectFit(square);
    CHECK(nearPoint(squared.corners[PrRectCorner_TopLeft].radii, PrVec2{0, 0}));
    CHECK(nearPoint(squared.corners[PrRectCorner_TopRight].radii, PrVec2{2, 2}));
    CHECK(nearPoint(squared.corners[PrRectCorner_BottomRight].radii, PrVec2{0, 0}));
    CHECK(nearRect(prCornerRectRound(PrRect{1, 2, 3, 4}, 5).rect, PrRect{1, 2, 3, 4}));
    CHECK(prCornerRectRound(PrRect{1, 2, 3, 4}, 5).corners[PrRectCorner_BottomLeft].shape == 1.0f);
}

TEST_CASE("transforms concatenate first then second") {
    PrAffine2 scale = prAffine2Scale(PrVec2{2, 3});
    PrAffine2 move = prAffine2Translate(PrVec2{10, 20});
    PrVec2 p{1, 1};
    CHECK(nearPoint(prAffine2MapPoint(prAffine2Concat(scale, move), p), PrVec2{12, 23}));
    CHECK(nearPoint(prAffine2MapPoint(prAffine2Concat(move, scale), p), PrVec2{22, 63}));
    CHECK(nearPoint(prAffine2MapPoint(prAffine2Rotate(kPrF32Pi / 2), PrVec2{1, 0}), PrVec2{0, 1}));
}

TEST_CASE("a rotation by a multiple of a quarter turn is exact") {
    PrAffine2 whole = prAffine2Rotate(2 * kPrF32Pi);
    CHECK(whole.a == 1.0f);
    CHECK(whole.b == 0.0f);
    CHECK(whole.c == 0.0f);
    CHECK(whole.d == 1.0f);
    PrAffine2 quarter = prAffine2Rotate(kPrF32Pi / 2);
    CHECK(quarter.a == 0.0f);
    CHECK(quarter.b == 1.0f);
    PrAffine2 half = prAffine2Rotate(kPrF32Pi);
    CHECK(half.a == -1.0f);
    CHECK(half.b == 0.0f);
}

TEST_CASE("an inverse undoes the transform, and a singular one has none") {
    PrAffine2 t = prAffine2Concat(prAffine2Rotate(0.5f), prAffine2Translate(PrVec2{3, -4}));
    t = prAffine2Concat(prAffine2Scale(PrVec2{2, 0.5f}), t);
    PrAffine2 inverse{};
    REQUIRE(prAffine2Invert(t, &inverse));
    PrVec2 p{7, -2};
    CHECK(nearPoint(prAffine2MapPoint(inverse, prAffine2MapPoint(t, p)), p));

    PrAffine2 unchanged = prAffine2Identity();
    CHECK(!prAffine2Invert(prAffine2Scale(PrVec2{0, 1}), &unchanged));
    CHECK(nearPoint(prAffine2MapPoint(unchanged, p), p));
}

TEST_CASE("a mapped rectangle is the bounds of its corners") {
    PrRect r{0, 0, 2, 1};
    PrRect mapped = prAffine2MapRect(prAffine2Rotate(kPrF32Pi / 2), r);
    CHECK(nearRect(mapped, PrRect{-1, 0, 0, 2}));
    CHECK(prRectIsEmpty(prAffine2MapRect(prAffine2Scale(PrVec2{2, 2}), PrRect{1, 1, 1, 1})));
}

TEST_CASE("a corner's curve runs from the side before it to the side after") {
    PrVec2 round = prCornerCurvePoint(1, 0.5f);
    CHECK(nearPoint(round, PrVec2{0.70710678f, 1 - 0.70710678f}));
    CHECK(nearPoint(prCornerCurvePoint(0, 0.3f), PrVec2{prCornerCurvePoint(0, 0.3f).x, prCornerCurvePoint(0, 0.3f).x}));
    CHECK(nearPoint(prCornerCurvePoint(-1, 0.5f), PrVec2{1 - 0.70710678f, 0.70710678f}));
    CHECK(nearPoint(prCornerCurvePoint(kPrF32Infinity, 0.25f), PrVec2{0.5f, 0}));
    CHECK(nearPoint(prCornerCurvePoint(-kPrF32Infinity, 0.25f), PrVec2{0, 0.5f}));
    const float shapes[] = {-3.0f, -1.0f, 0.0f, 0.5f, 1.0f, 2.0f, 5.0f};
    for (float shape : shapes) {
        CHECK(nearPoint(prCornerCurvePoint(shape, 0), PrVec2{0, 0}));
        CHECK(nearPoint(prCornerCurvePoint(shape, 1), PrVec2{1, 1}));
    }
    // Every convex curve leaves along the side before and arrives along the side after.
    CHECK(nearPoint(prCornerCurveTangent(2, 0), PrVec2{1, 0}));
    CHECK(nearPoint(prCornerCurveTangent(2, 1), PrVec2{0, 1}));
    CHECK(nearPoint(prCornerCurveTangent(-2, 0), PrVec2{0, 1}));
}

TEST_CASE("fitting corners scales every radius by the tightest side, then concave ones apart") {
    PrRect rect{0, 0, 100, 50};
    PrCornerRect in{rect, {{{60, 10}, 1}, {{60, 10}, 2}, {{10, 40}, 0}, {{10, 40}, 1}}};
    PrCornerRect fitted = prCornerRectFit(in);
    const PrCorner* out = fitted.corners;
    float factor = 100.0f / 120.0f;
    CHECK(nearPoint(out[PrRectCorner_TopLeft].radii, PrVec2{60 * factor, 10 * factor}));
    CHECK(out[PrRectCorner_TopRight].shape == 2.0f);

    // Opposite scoops that each reach past the middle overlap until scaled apart.
    PrCornerRect scoops{rect, {{{80, 40}, -1}, {{0, 0}, 1}, {{80, 40}, -1}, {{0, 0}, 1}}};
    fitted = prCornerRectFit(scoops);
    CHECK(out[PrRectCorner_TopLeft].radii.x < 80.0f);
    CHECK(out[PrRectCorner_TopLeft].radii.x > 40.0f);
    CHECK(near(out[PrRectCorner_TopLeft].radii.x / 80, out[PrRectCorner_BottomRight].radii.y / 40));
    // Round corners of the same size are unchanged.
    PrCornerRect rounds{rect, {{{80, 40}, 1}, {{0, 0}, 1}, {{80, 40}, 1}, {{0, 0}, 1}}};
    fitted = prCornerRectFit(rounds);
    CHECK(nearPoint(out[PrRectCorner_TopLeft].radii, PrVec2{80, 40}));
}

TEST_CASE("insetting moves convex corners' midpoints inward by the insets and moves concave ones whole") {
    PrRect rect{0, 0, 100, 60};
    PrCornerRect shape{rect, {{{20, 20}, 2}, {{10, 30}, 1}, {{4, 4}, -1}, {{0, 0}, 1}}};
    const float insets[4] = {5, 6, 7, 8};
    PrCornerRect inset = prCornerRectInset(shape, insets);
    const PrCorner* out = inset.corners;
    CHECK(nearRect(inset.rect, PrRect{8, 5, 94, 53}));
    // A squircle's midpoint is 1 - 2^(-1/4) of its radii from the corner.
    float squircle = (1 - prF32Sqrt(0.5f)) / (1 - prF32Pow(2, -0.25f));
    CHECK(nearPoint(out[PrRectCorner_TopLeft].radii, PrVec2{20 - 8 * squircle, 20 - 5 * squircle}));
    CHECK(out[PrRectCorner_TopLeft].shape == 2.0f);
    CHECK(nearPoint(out[PrRectCorner_TopRight].radii, PrVec2{4, 25}));
    CHECK(nearPoint(out[PrRectCorner_BottomRight].radii, PrVec2{4, 4}));
    CHECK(out[PrRectCorner_BottomRight].shape == -1.0f);
    CHECK(prCornerIsSquare(out[PrRectCorner_BottomLeft]));
    // A bevel stays parallel to itself, so a band between the two keeps its width across the corner.
    PrCornerRect bevels{rect, {{{15, 15}, 0}, {{15, 15}, 0}, {{15, 15}, 0}, {{15, 15}, 0}}};
    const float even[4] = {4, 4, 4, 4};
    inset = prCornerRectInset(bevels, even);
    float outerLine = 15;
    float innerLine = 4 + 4 + out[PrRectCorner_TopLeft].radii.x;
    CHECK(near((innerLine - outerLine) / prF32Sqrt(2), 4));
}

TEST_CASE("outsetting grows radii by the outsets, less for a radius smaller than its outset, and keeps square corners") {
    PrRect rect{10, 10, 110, 70};
    PrCornerRect shape{rect, {{{20, 20}, 1}, {{5, 10}, 2}, {{0, 0}, 1}, {{20, 20}, 1}}};
    const float outsets[4] = {10, 10, 10, 0};
    PrCornerRect grown = prCornerRectOutset(shape, outsets);
    const PrCorner* out = grown.corners;
    CHECK(nearRect(grown.rect, PrRect{10, 0, 120, 80}));
    CHECK(nearPoint(out[PrRectCorner_TopLeft].radii, PrVec2{20, 30}));
    // 5 grows by 10 * (1 + (0.5 - 1)^3) and 10 by the whole outset.
    CHECK(nearPoint(out[PrRectCorner_TopRight].radii, PrVec2{5 + 10 * 0.875f, 20}));
    CHECK(out[PrRectCorner_TopRight].shape == 2.0f);
    CHECK(prCornerIsSquare(out[PrRectCorner_BottomRight]));
}

TEST_CASE("a shape holds the points inside its corners' curves") {
    PrRect rect{0, 0, 100, 60};
    PrCornerRect round{rect, {{{20, 20}, 1}, {{20, 20}, 1}, {{20, 20}, 1}, {{20, 20}, 1}}};
    CHECK(prCornerRectContains(round, PrVec2{50, 30}));
    CHECK(prCornerRectContains(round, PrVec2{50, 0}));
    CHECK(!prCornerRectContains(round, PrVec2{2, 2}));
    CHECK(prCornerRectContains(round, PrVec2{8, 8}));          // inside the circle about (20, 20)
    CHECK(!prCornerRectContains(round, PrVec2{97, 58}));
    CHECK(!prCornerRectContains(round, PrVec2{101, 30}));
    PrCornerRect bevel{rect, {{{20, 20}, 0}, {{0, 0}, 1}, {{0, 0}, 1}, {{0, 0}, 1}}};
    CHECK(prCornerRectContains(bevel, PrVec2{10, 11}));
    CHECK(!prCornerRectContains(bevel, PrVec2{9, 10}));
    CHECK(prCornerRectContains(bevel, PrVec2{99, 1}));
    // A scoop holds what lies beyond the circle about its corner.
    PrCornerRect scoop{rect, {{{20, 20}, -1}, {{0, 0}, 1}, {{0, 0}, 1}, {{0, 0}, 1}}};
    CHECK(!prCornerRectContains(scoop, PrVec2{5, 5}));
    CHECK(prCornerRectContains(scoop, PrVec2{19, 19}));
    PrCornerRect notch{rect, {{{20, 20}, -kPrF32Infinity}, {{0, 0}, 1}, {{0, 0}, 1}, {{0, 0}, 1}}};
    CHECK(!prCornerRectContains(notch, PrVec2{19, 19}));
    CHECK(prCornerRectContains(notch, PrVec2{21, 19}));
}

TEST_CASE("only the identity is the identity") {
    CHECK(prAffine2IsIdentity(prAffine2Identity()));
    CHECK(prAffine2IsIdentity(prAffine2Translate(PrVec2{0, 0})));
    CHECK(!prAffine2IsIdentity(prAffine2Translate(PrVec2{0, 1})));
    CHECK(!prAffine2IsIdentity(prAffine2Scale(PrVec2{-1, 1})));
    CHECK(!prAffine2IsIdentity(PrAffine2{1, 0, 0.5f, 1, 0, 0}));
}

TEST_CASE("the largest scale and similarity of a transform") {
    CHECK(near(prAffine2MaxScale(prAffine2Scale(PrVec2{2, -3})), 3));
    CHECK(prAffine2IsSimilarity(prAffine2Concat(prAffine2Scale(PrVec2{2, 2}), prAffine2Rotate(1))));
    CHECK(prAffine2IsSimilarity(prAffine2Scale(PrVec2{-2, 2})));
    CHECK(!prAffine2IsSimilarity(prAffine2Scale(PrVec2{2, 1})));
    CHECK(!prAffine2IsSimilarity(PrAffine2{1, 0, 0.5f, 1, 0, 0}));
}

TEST_CASE("an affine transform is axis-aligned if it scales, translates or swaps the axes") {
    CHECK(prAffine2IsAxisAligned(prAffine2Concat(prAffine2Scale(PrVec2{-2, 3}), prAffine2Translate(PrVec2{5, 6}))));
    CHECK(prAffine2IsAxisAligned(PrAffine2{0, 1, -1, 0, 0, 0}));
    CHECK(!prAffine2IsAxisAligned(prAffine2Rotate(0.5f)));
    CHECK(!prAffine2IsAxisAligned(PrAffine2{1, 0, 0.5f, 1, 0, 0}));
}

TEST_CASE("4x4 transforms concatenate, invert and map the plane with perspective") {
    PrMatrix4 move = prMatrix4Translate(PrVec3{10, 20, 0});
    PrMatrix4 turn = prMatrix4Rotate(PrVec3{0, 0, 1}, kPrF32Pi / 2);
    PrMatrix4 both = prMatrix4Concat(&turn, &move);
    PrVec2 out{};
    REQUIRE(prMatrix4MapPoint(&both, PrVec2{1, 0}, &out));
    CHECK(nearPoint(out, PrVec2{10, 21}));
    PrAffine2 flat{};
    REQUIRE(prAffine2FromMatrix4(&both, &flat));
    CHECK(nearPoint(prAffine2MapPoint(flat, PrVec2{1, 0}), PrVec2{10, 21}));
    PrMatrix4 inverse{};
    REQUIRE(prMatrix4Invert(&both, &inverse));
    PrMatrix4 identity = prMatrix4Concat(&both, &inverse);
    for (uint32_t i = 0; i < 4; ++i)
        CHECK(near(identity.m[i][i], 1));

    // A rectangle tilted away about x under a perspective of 100: the far edge moves nearer the center.
    PrMatrix4 tilt = prMatrix4Rotate(PrVec3{1, 0, 0}, kPrF32Pi / 4);
    PrMatrix4 eye = prMatrix4Perspective(100);
    PrMatrix4 seen = prMatrix4Concat(&tilt, &eye);
    CHECK(!prAffine2FromMatrix4(&seen, &flat));
    PrVec2 far{};
    PrVec2 close{};
    REQUIRE(prMatrix4MapPoint(&seen, PrVec2{10, -10}, &far));
    REQUIRE(prMatrix4MapPoint(&seen, PrVec2{10, 10}, &close));
    CHECK(far.x < 10.0f);
    CHECK(close.x > 10.0f);
    // A point at the eye has no image.
    CHECK(!prMatrix4MapPoint(&seen, PrVec2{0, 200}, &out));
    PrMatrix4 flatZ = prMatrix4Scale(PrVec3{1, 1, 0});
    CHECK(!prMatrix4Invert(&flatZ, &inverse));
}

TEST_CASE("4x4 transforms in doubles concatenate, invert, round to floats and tell which side of the plane shows") {
    PrMatrix4 tilt = prMatrix4Rotate(PrVec3{1, 0, 0}, kPrF32Pi / 4);
    PrMatrix4 eye = prMatrix4Perspective(100);
    PrDMatrix4 a = prDMatrix4FromMatrix4(&tilt);
    PrDMatrix4 b = prDMatrix4FromMatrix4(&eye);
    PrDMatrix4 both = prDMatrix4Concat(&a, &b);
    PrMatrix4 single = prMatrix4Concat(&tilt, &eye);
    PrMatrix4 rounded = prMatrix4FromDMatrix4(&both);
    for (uint32_t row = 0; row < 4; ++row) {
        for (uint32_t column = 0; column < 4; ++column)
            CHECK(near(rounded.m[row][column], single.m[row][column]));
    }
    PrDMatrix4 inverse{};
    REQUIRE(prDMatrix4Invert(&both, &inverse));
    PrDMatrix4 identity = prDMatrix4Concat(&both, &inverse);
    PrDMatrix4 expected = prDMatrix4Identity();
    for (uint32_t row = 0; row < 4; ++row) {
        for (uint32_t column = 0; column < 4; ++column)
            CHECK(prF64Abs(identity.m[row][column] - expected.m[row][column]) < 1e-12);
    }
    PrMatrix4 flatZ = prMatrix4Scale(PrVec3{1, 1, 0});
    PrDMatrix4 singular = prDMatrix4FromMatrix4(&flatZ);
    CHECK(!prDMatrix4Invert(&singular, &inverse));

    // A half turn about y shows the plane's back, with or without a perspective. A mirror within the plane does not,
    // nor does a tilt short of a quarter turn under a perspective.
    PrMatrix4 half = prMatrix4Rotate(PrVec3{0, 1, 0}, kPrF32Pi);
    PrDMatrix4 turned = prDMatrix4FromMatrix4(&half);
    CHECK(prDMatrix4ShowsBack(&turned));
    PrDMatrix4 seenTurned = prDMatrix4Concat(&turned, &b);
    CHECK(prDMatrix4ShowsBack(&seenTurned));
    PrMatrix4 mirror = prMatrix4Scale(PrVec3{-1, 1, 1});
    PrDMatrix4 mirrored = prDMatrix4FromMatrix4(&mirror);
    CHECK(!prDMatrix4ShowsBack(&mirrored));
    CHECK(!prDMatrix4ShowsBack(&both));
    CHECK(!prDMatrix4ShowsBack(&singular));
}

TEST_CASE("projective transforms in doubles extend affine ones, concatenate, invert and map points") {
    PrDAffine2 turn = prDAffine2FromAffine2(prAffine2Rotate(kPrF32Pi / 2));
    PrDAffine2 move = prDAffine2Translate(1e7, 20);
    PrDMatrix3 h = prDMatrix3FromDAffine2(turn);
    PrDMatrix3 m = prDMatrix3FromDAffine2(move);
    PrDMatrix3 both = prDMatrix3Concat(&h, &m);
    PrDAffine2 affine{};
    REQUIRE(prDAffine2FromDMatrix3(&both, &affine));
    PrDAffine2 composed = prDAffine2Concat(turn, move);
    CHECK(prF64Abs(affine.e - composed.e) < 1e-9);
    PrDVec2 out{};
    REQUIRE(prDMatrix3MapPoint(&both, PrDVec2{3, 4}, &out));
    // A large translation keeps the small offsets that floats would lose.
    CHECK(prF64Abs(out.x - (1e7 - 4)) < 1e-6);
    CHECK(prF64Abs(out.y - 23) < 1e-6);

    // w varies across the plane: a point maps if w is above zero, and has no image otherwise.
    const PrDMatrix3 seen{{{1, 0, 0}, {0, 1, 0}, {0.01, 0, 1}}};
    CHECK(!prDAffine2FromDMatrix3(&seen, &affine));
    REQUIRE(prDMatrix3MapPoint(&seen, PrDVec2{100, 50}, &out));
    CHECK(prF64Abs(out.x - 50) < 1e-12);
    CHECK(!prDMatrix3MapPoint(&seen, PrDVec2{-100, 50}, &out));
    PrDMatrix3 inverse{};
    REQUIRE(prDMatrix3Invert(&seen, &inverse));
    PrDVec2 back{};
    REQUIRE(prDMatrix3MapPoint(&inverse, PrDVec2{50, 25}, &back));
    CHECK(prF64Abs(back.x - 100) < 1e-9);
    CHECK(prF64Abs(back.y - 50) < 1e-9);
    const PrDMatrix3 flat{{{1, 2, 3}, {2, 4, 6}, {0, 0, 1}}};
    CHECK(!prDMatrix3Invert(&flat, &inverse));

    // The plane's transform of a 4x4 matrix, and the round trip through floats.
    PrMatrix4 tilt = prMatrix4Rotate(PrVec3{1, 1, 0}, 0.6f);
    PrDMatrix4 tilted = prDMatrix4FromMatrix4(&tilt);
    PrDMatrix3 plane = prDMatrix3FromDMatrix4(&tilted);
    PrMatrix3 rounded = prMatrix3FromDMatrix3(&plane);
    PrMatrix3 single = prMatrix3FromMatrix4(&tilt);
    PrDMatrix3 again = prDMatrix3FromMatrix3(&rounded);
    for (uint32_t row = 0; row < 3; ++row) {
        for (uint32_t column = 0; column < 3; ++column) {
            CHECK(rounded.m[row][column] == single.m[row][column]);
            CHECK(again.m[row][column] == double(single.m[row][column]));
        }
    }
    PrDMatrix3 one = prDMatrix3Identity();
    PrDMatrix3 same = prDMatrix3Concat(&plane, &one);
    CHECK(same.m[1][2] == plane.m[1][2]);
}

TEST_CASE("a projective transform's inverse undoes it, and a singular one has none") {
    const PrMatrix3 h{{{2, 0.5f, 3}, {-1, 1.5f, 4}, {0.001f, 0.002f, 1}}};
    PrMatrix3 inverse{};
    REQUIRE(prMatrix3Invert(&h, &inverse));
    for (uint32_t row = 0; row < 3; ++row) {
        for (uint32_t column = 0; column < 3; ++column) {
            float sum = 0;
            for (uint32_t k = 0; k < 3; ++k)
                sum += h.m[row][k] * inverse.m[k][column];
            CHECK(near(sum, row == column ? 1 : 0));
        }
    }
    const PrMatrix3 flat{{{1, 2, 3}, {2, 4, 6}, {0, 0, 1}}};
    CHECK(!prMatrix3Invert(&flat, &inverse));
}

TEST_CASE("projective transforms extend affine ones, concatenate and map points") {
    PrAffine2 turn = prAffine2Rotate(kPrF32Pi / 2);
    PrAffine2 move = prAffine2Translate(PrVec2{10, 20});
    PrMatrix3 h = prMatrix3FromAffine2(turn);
    PrMatrix3 m = prMatrix3FromAffine2(move);
    PrMatrix3 both = prMatrix3Concat(&h, &m);
    PrVec2 out{};
    REQUIRE(prMatrix3MapPoint(&both, PrVec2{3, 4}, &out));
    CHECK(nearPoint(out, prAffine2MapPoint(prAffine2Concat(turn, move), PrVec2{3, 4})));

    // w varies across the plane: a point maps if w is above zero, and has no image if w is zero or less.
    const PrMatrix3 seen{{{1, 0, 0}, {0, 1, 0}, {0.01f, 0, 1}}};
    REQUIRE(prMatrix3MapPoint(&seen, PrVec2{100, 50}, &out));
    CHECK(nearPoint(out, PrVec2{50, 25}));
    out = PrVec2{7, 8};
    CHECK(!prMatrix3MapPoint(&seen, PrVec2{-200, 50}, &out));
    CHECK(!prMatrix3MapPoint(&seen, PrVec2{-100, 50}, &out));
    CHECK(nearPoint(out, PrVec2{7, 8}));
}

TEST_CASE("the plane's projective transform agrees with the 4x4 matrix it comes from") {
    PrMatrix4 tilt = prMatrix4Rotate(PrVec3{1, 1, 0}, 0.6f);
    PrMatrix4 eye = prMatrix4Perspective(300);
    PrMatrix4 move = prMatrix4Translate(PrVec3{40, -20, 15});
    PrMatrix4 tilted = prMatrix4Concat(&tilt, &move);
    PrMatrix4 seen = prMatrix4Concat(&tilted, &eye);
    PrMatrix3 plane = prMatrix3FromMatrix4(&seen);
    const PrVec2 points[] = {{0, 0}, {100, -50}, {-80, 120}, {33, 7}};
    for (PrVec2 point : points) {
        PrVec2 expected{};
        PrVec2 out{};
        REQUIRE(prMatrix4MapPoint(&seen, point, &expected));
        REQUIRE(prMatrix3MapPoint(&plane, point, &out));
        CHECK(nearPoint(out, expected));
    }
}

TEST_CASE("an affine projective transform converts back, divided by its w, and a projective one does not") {
    PrAffine2 turn = prAffine2Concat(prAffine2Rotate(0.3f), prAffine2Translate(PrVec2{5, 6}));
    PrMatrix3 h = prMatrix3FromAffine2(turn);
    CHECK(prMatrix3IsAffine(&h));
    PrAffine2 out{};
    REQUIRE(prAffine2FromMatrix3(&h, &out));
    CHECK(nearPoint(prAffine2MapPoint(out, PrVec2{3, 4}), prAffine2MapPoint(turn, PrVec2{3, 4})));

    const PrMatrix3 doubled{{{2, 0, 4}, {0, 2, 6}, {0, 0, 2}}};
    REQUIRE(prAffine2FromMatrix3(&doubled, &out));
    CHECK(nearPoint(prAffine2MapPoint(out, PrVec2{1, 1}), PrVec2{3, 4}));

    const PrMatrix3 seen{{{1, 0, 0}, {0, 1, 0}, {0.01f, 0, 1}}};
    const PrMatrix3 behind{{{1, 0, 0}, {0, 1, 0}, {0, 0, -1}}};
    CHECK(!prMatrix3IsAffine(&seen));
    CHECK(!prMatrix3IsAffine(&behind));
    out = PrAffine2{7, 0, 0, 7, 0, 0};
    CHECK(!prAffine2FromMatrix3(&seen, &out));
    CHECK(out.a == 7.0f);
}

TEST_CASE("the affine tangent of a projection agrees with it to first order") {
    const PrMatrix3 h{{{2, 0.5f, 3}, {-1, 1.5f, 4}, {0.001f, 0.002f, 1}}};
    const PrVec2 at{40, 25};
    PrAffine2 tangent = prMatrix3AffineNear(&h, at);
    PrVec2 exact{};
    REQUIRE(prMatrix3MapPoint(&h, at, &exact));
    CHECK(nearPoint(prAffine2MapPoint(tangent, at), exact));
    // A small step maps through the tangent as through the projection, up to the step's square.
    const PrVec2 step{0.01f, -0.02f};
    PrVec2 moved{};
    REQUIRE(prMatrix3MapPoint(&h, at + step, &moved));
    PrVec2 approximated = prAffine2MapPoint(tangent, at + step);
    CHECK(prF32Abs(approximated.x - moved.x) < 1e-3f);
    CHECK(prF32Abs(approximated.y - moved.y) < 1e-3f);

    // An affine transform is its own tangent everywhere.
    PrAffine2 turn = prAffine2Concat(prAffine2Rotate(0.3f), prAffine2Translate(PrVec2{5, 6}));
    PrMatrix3 flat = prMatrix3FromAffine2(turn);
    PrAffine2 same = prMatrix3AffineNear(&flat, PrVec2{100, -30});
    CHECK(nearPoint(prAffine2MapPoint(same, PrVec2{1, 2}), prAffine2MapPoint(turn, PrVec2{1, 2})));

    const PrMatrix3 seen{{{1, 0, 0}, {0, 1, 0}, {0.01f, 0, 1}}};
    PrAffine2 none = prMatrix3AffineNear(&seen, PrVec2{-200, 0});
    CHECK(none.a == 0.0f);
    CHECK(none.d == 0.0f);
}

TEST_CASE("a projection's largest scale bounds its scale everywhere in the visible part of a rectangle") {
    // An affine transform's bound is its largest scale: the spectral norm of its linear part.
    PrAffine2 stretch = prAffine2Concat(prAffine2Scale(PrVec2{3, 0.5f}), prAffine2Rotate(0.7f));
    PrMatrix3 flat = prMatrix3FromAffine2(stretch);
    CHECK(near(prMatrix3MaxScale(&flat, PrRect{0, 0, 10, 10}, 1e-3f), 3));

    // A plane tilted under a perspective: the bound is at least the scale of the tangent at sampled points, in every
    // direction, and not far above the largest of them.
    PrMatrix4 tilt = prMatrix4Rotate(PrVec3{1, 0.3f, 0}, 1.1f);
    PrMatrix4 eye = prMatrix4Perspective(200);
    PrMatrix4 seen4 = prMatrix4Concat(&tilt, &eye);
    PrMatrix3 seen = prMatrix3FromMatrix4(&seen4);
    const PrRect rect{-100, -100, 100, 100};
    const float minimumW = 0.05f;
    float bound = prMatrix3MaxScale(&seen, rect, minimumW);
    float largest = 0;
    for (uint32_t i = 0; i <= 40; ++i) {
        for (uint32_t j = 0; j <= 40; ++j) {
            PrVec2 p{
                rect.left + (rect.right - rect.left) * float(i) / 40,
                rect.top + (rect.bottom - rect.top) * float(j) / 40,
            };
            float w = seen.m[2][0] * p.x + seen.m[2][1] * p.y + seen.m[2][2];
            if (w < minimumW)
                continue;
            PrAffine2 t = prMatrix3AffineNear(&seen, p);
            for (uint32_t k = 0; k < 16; ++k) {
                float angle = kPrF32Pi * float(k) / 16;
                PrVec2 u{prF32Cos(angle), prF32Sin(angle)};
                float x = t.a * u.x + t.c * u.y;
                float y = t.b * u.x + t.d * u.y;
                largest = prF32Max(largest, prF32Sqrt(x * x + y * y));
            }
        }
    }
    CHECK(largest > 1.0f);
    CHECK(bound >= largest);
    CHECK(bound < largest * 3);

    // No part of the rectangle is in front of the eye.
    const PrMatrix3 behind{{{1, 0, 0}, {0, 1, 0}, {0, 0, -1}}};
    CHECK(prMatrix3MaxScale(&behind, rect, minimumW) == 0.0f);
}
