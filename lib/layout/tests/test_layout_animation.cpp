#include "pr/layout_animation.h"

#include "doctest/doctest.h"

#include <initializer_list>

namespace pr_impl {

namespace {

PrLayoutEasing bezier(double x1, double y1, double x2, double y2) {
    PrLayoutEasing easing = prLayoutEasingLinear();
    easing.kind = PrLayoutEasingKind_CubicBezier;
    easing.x1 = x1;
    easing.y1 = y1;
    easing.x2 = x2;
    easing.y2 = y2;
    return easing;
}

PrLayoutTransformFunction function(PrLayoutTransformKind kind, float x, float y = 0, float z = 0, float w = 0,
                                   float v = 0) {
    PrLayoutTransformFunction f{};
    f.kind = kind;
    f.values[0] = x;
    f.values[1] = y;
    f.values[2] = z;
    f.values[3] = w;
    f.values[4] = v;
    return f;
}

PrLayoutTransformList list(std::initializer_list<PrLayoutTransformFunction> functions) {
    PrLayoutTransformList out{};
    for (const PrLayoutTransformFunction& f : functions)
        out.functions[out.count++] = f;
    return out;
}

PrLayoutTransformFunction matrixOf(const PrMatrix4& m) {
    PrLayoutTransformFunction f{};
    f.kind = PrLayoutTransformKind_Matrix;
    for (uint32_t column = 0; column < 4; ++column) {
        for (uint32_t row = 0; row < 4; ++row)
            f.values[column * 4 + row] = m.m[row][column];
    }
    return f;
}

void checkNear(const PrMatrix4& a, const PrMatrix4& b) {
    for (uint32_t row = 0; row < 4; ++row) {
        for (uint32_t column = 0; column < 4; ++column) {
            CAPTURE(row);
            CAPTURE(column);
            CHECK(double(a.m[row][column]) == doctest::Approx(double(b.m[row][column])).epsilon(1e-4).scale(1));
        }
    }
}

constexpr float kQuarter = 1.5707963f;

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("easing functions map progress as css-easing-2 defines them") {
    PrLayoutEasing linear = prLayoutEasingLinear();
    CHECK(prLayoutEase(&linear, 0.3, false) == doctest::Approx(0.3));
    PrLayoutEasing ease = bezier(0.25, 0.1, 0.25, 1);
    CHECK(prLayoutEase(&ease, 0.5, false) == doctest::Approx(0.8024).epsilon(0.001));
    CHECK(prLayoutEase(&ease, 0, false) == doctest::Approx(0));
    CHECK(prLayoutEase(&ease, 1, false) == doctest::Approx(1));
    PrLayoutEasing steps = prLayoutEasingLinear();
    steps.kind = PrLayoutEasingKind_Steps;
    steps.steps = 4;
    CHECK(prLayoutEase(&steps, 0.3, false) == doctest::Approx(0.25));
    steps.position = PrLayoutStepPosition_JumpStart;
    CHECK(prLayoutEase(&steps, 0.3, false) == doctest::Approx(0.5));
    CHECK(prLayoutEase(&steps, 0, true) == doctest::Approx(0));
}

TEST_CASE("timing plays iterations in their direction, and fills where it says") {
    PrLayoutTiming timing = prLayoutTimingDefault();
    timing.start = 100;
    timing.delay = 50;
    timing.duration = 200;
    timing.iterations = 2;
    timing.direction = PrLayoutPlaybackDirection_Alternate;
    bool finished = false;
    CHECK(prLayoutTimingProgress(&timing, 120, &finished) < 0);
    CHECK(prLayoutTimingProgress(&timing, 200, &finished) == doctest::Approx(0.25));
    CHECK(prLayoutTimingProgress(&timing, 400, &finished) == doctest::Approx(0.75));
    CHECK_FALSE(finished);
    CHECK(prLayoutTimingProgress(&timing, 600, &finished) < 0);
    CHECK(finished);
    timing.fill = PrLayoutFill_Both;
    CHECK(prLayoutTimingProgress(&timing, 120, &finished) == doctest::Approx(0));
    CHECK(prLayoutTimingProgress(&timing, 600, &finished) == doctest::Approx(0));
    timing.paused = true;
    timing.hold = 150;
    CHECK(prLayoutTimingProgress(&timing, 10000, &finished) == doctest::Approx(0.5));
}

TEST_CASE("transforms interpolate function by function, and by matrix where their kinds differ") {
    PrLayoutTransformList none{};
    PrLayoutTransformList turn = list({function(PrLayoutTransformKind_Rotate, 0, 0, 1, 6.2831853f)});
    // A full turn interpolates through half of one, which a matrix could not tell from no turn at all.
    PrLayoutTransformList half = prLayoutTransformListInterpolate(&none, &turn, 0.5, PrVec2{0, 0});
    REQUIRE(half.count == 1);
    CHECK(half.functions[0].kind == PrLayoutTransformKind_Rotate);
    CHECK(double(half.functions[0].values[3]) == doctest::Approx(3.14159));

    PrLayoutTransformList moved = list({function(PrLayoutTransformKind_Translate, 100, 0)});
    PrLayoutTransformList scaled = list({function(PrLayoutTransformKind_Scale, 3, 3, 1)});
    PrLayoutTransformList between = prLayoutTransformListInterpolate(&moved, &scaled, 0.5, PrVec2{0, 0});
    PrMatrix4 m = prLayoutTransformListMatrix(&between, PrVec2{0, 0});
    CHECK(double(m.m[0][0]) == doctest::Approx(2));
    CHECK(double(m.m[1][1]) == doctest::Approx(2));
    CHECK(double(m.m[0][3]) == doctest::Approx(50));
    CHECK(double(m.m[1][0]) == doctest::Approx(0));

    // The functions before the first pair of different kinds interpolate one by one, and the rest as one matrix.
    PrLayoutTransformList a = list({function(PrLayoutTransformKind_Translate, 10), moved.functions[0]});
    PrLayoutTransformList b = list({function(PrLayoutTransformKind_Translate, 30), scaled.functions[0]});
    between = prLayoutTransformListInterpolate(&a, &b, 0.5, PrVec2{0, 0});
    REQUIRE(between.count == 2);
    CHECK(between.functions[0].kind == PrLayoutTransformKind_Translate);
    CHECK(double(between.functions[0].values[0]) == doctest::Approx(20));
    CHECK(between.functions[1].kind == PrLayoutTransformKind_Matrix);
}

TEST_CASE("transforms that cannot be decomposed interpolate only discretely") {
    PrLayoutTransformList moved = list({function(PrLayoutTransformKind_Translate, 100, 0)});
    PrLayoutTransformList scaled = list({function(PrLayoutTransformKind_Scale, 3, 3, 1)});
    CHECK(prLayoutTransformListInterpolates(&moved, &scaled, PrVec2{0, 0}));
    // A matrix3d() whose fourth row is zero has no decomposition.
    PrLayoutTransformFunction flat{.kind = PrLayoutTransformKind_Matrix};
    flat.values[0] = flat.values[5] = 2;
    flat.values[15] = 1;
    PrLayoutTransformList singular = list({flat});
    PrLayoutTransformList twice = list({function(PrLayoutTransformKind_Scale, 2, 2, 2)});
    CHECK_FALSE(prLayoutTransformListInterpolates(&singular, &twice, PrVec2{0, 0}));
    CHECK_FALSE(prLayoutTransformListInterpolates(&twice, &singular, PrVec2{0, 0}));
}

TEST_CASE("a list with percentages is affine in its reference box") {
    // translate(50%, 10px) rotate(90deg) translate(10%, 20%)
    PrLayoutTransformList functions = list({function(PrLayoutTransformKind_Translate, 0, 10, 0, 0.5f),
                                            function(PrLayoutTransformKind_Rotate, 0, 0, 1, kQuarter),
                                            function(PrLayoutTransformKind_Translate, 0, 0, 0, 0.1f, 0.2f)});
    PrLayoutTransform transform = prLayoutTransformListTransform(&functions);
    for (PrVec2 extent : {PrVec2{200, 100}, PrVec2{30, 70}}) {
        PrMatrix4 resolved = prLayoutTransformResolve(&transform, extent);
        checkNear(resolved, prLayoutTransformListMatrix(&functions, extent));
    }

    // A percentage to the right of a perspective stays exact. One to its left, which applies after it, counts as zero.
    PrLayoutTransformList right = list({function(PrLayoutTransformKind_Perspective, 100),
                                        function(PrLayoutTransformKind_Rotate, 0, 1, 0, 0.5f),
                                        function(PrLayoutTransformKind_Translate, 0, 0, 0, 0.5f)});
    transform = prLayoutTransformListTransform(&right);
    PrMatrix4 resolved = prLayoutTransformResolve(&transform, PrVec2{80, 40});
    checkNear(resolved, prLayoutTransformListMatrix(&right, PrVec2{80, 40}));
    PrLayoutTransformList left = list({function(PrLayoutTransformKind_Translate, 5, 0, 0, 0.5f),
                                       function(PrLayoutTransformKind_Perspective, 100)});
    transform = prLayoutTransformListTransform(&left);
    resolved = prLayoutTransformResolve(&transform, PrVec2{80, 40});
    CHECK(double(resolved.m[0][3]) == doctest::Approx(5));
    CHECK(double(resolved.m[3][2]) == doctest::Approx(-0.01));
}

TEST_CASE("3D transforms interpolate as css-transforms-2 decomposes them") {
    // Rotations about different axes interpolate as matrices, halfway between the two.
    PrLayoutTransformList x = list({function(PrLayoutTransformKind_Rotate, 1, 0, 0, kQuarter)});
    PrLayoutTransformList y = list({function(PrLayoutTransformKind_Rotate, 0, 1, 0, kQuarter)});
    PrMatrix4 start = prLayoutTransformListMatrix(&x, PrVec2{0, 0});
    PrLayoutTransformList at = prLayoutTransformListInterpolate(&x, &y, 0, PrVec2{0, 0});
    checkNear(prLayoutTransformListMatrix(&at, PrVec2{0, 0}), start);
    at = prLayoutTransformListInterpolate(&x, &y, 0.5, PrVec2{0, 0});
    REQUIRE(at.count == 1);
    CHECK(at.functions[0].kind == PrLayoutTransformKind_Matrix);
    PrMatrix4 m = prLayoutTransformListMatrix(&at, PrVec2{0, 0});
    CHECK(double(m.m[0][0]) == doctest::Approx(double(m.m[1][1])));
    // The result is a rotation: its columns stay of unit length.
    for (uint32_t column = 0; column < 3; ++column) {
        double length = 0;
        for (uint32_t row = 0; row < 3; ++row)
            length += double(m.m[row][column]) * double(m.m[row][column]);
        CHECK(length == doctest::Approx(1));
    }

    // Matrices that differ by the angle of a rotation about y interpolate the angle, and keep the translation.
    PrMatrix4 shift = prMatrix4Translate(PrVec3{10, 20, 30});
    auto turned = [&](float radians) {
        PrMatrix4 r = prMatrix4Rotate(PrVec3{0, 1, 0}, radians);
        return prMatrix4Concat(&r, &shift);
    };
    PrLayoutTransformList from = list({matrixOf(turned(kQuarter / 3))});
    PrLayoutTransformList to = list({matrixOf(turned(kQuarter * 2 / 3))});
    at = prLayoutTransformListInterpolate(&from, &to, 0.5, PrVec2{0, 0});
    checkNear(prLayoutTransformListMatrix(&at, PrVec2{0, 0}), turned(kQuarter / 2));

    // A matrix with every component decomposes and recomposes to itself, divided by its last element.
    PrMatrix4 general = turned(0.4f);
    PrMatrix4 skew = prMatrix4Identity();
    skew.m[0][1] = 0.3f;
    skew.m[1][2] = 0.2f;
    PrMatrix4 scale = prMatrix4Scale(PrVec3{2, 0.5f, 3});
    PrMatrix4 perspective = prMatrix4Perspective(400);
    general = prMatrix4Concat(&skew, &general);
    general = prMatrix4Concat(&scale, &general);
    general = prMatrix4Concat(&general, &perspective);
    float w = general.m[3][3];
    for (uint32_t row = 0; row < 4; ++row) {
        for (uint32_t column = 0; column < 4; ++column)
            general.m[row][column] /= w;
    }
    from = list({matrixOf(general)});
    at = prLayoutTransformListInterpolate(&from, &from, 0.3, PrVec2{0, 0});
    checkNear(prLayoutTransformListMatrix(&at, PrVec2{0, 0}), general);

    // A perspective interpolates its inverse distance, and none is an infinite distance.
    PrLayoutTransformList near = list({function(PrLayoutTransformKind_Perspective, 100)});
    PrLayoutTransformList flat = list({function(PrLayoutTransformKind_Perspective, 0)});
    at = prLayoutTransformListInterpolate(&near, &flat, 0.5, PrVec2{0, 0});
    REQUIRE(at.count == 1);
    CHECK(double(at.functions[0].values[0]) == doctest::Approx(200));
}

TEST_CASE("a track samples its keyframes at a time") {
    PrLayoutKeyframe frames[3] = {};
    frames[0].offset = 0;
    frames[0].opacity = 1;
    frames[0].easing = prLayoutEasingLinear();
    frames[1].offset = 0.5;
    frames[1].opacity = 0.2f;
    frames[1].easing = prLayoutEasingLinear();
    frames[2].offset = 1;
    frames[2].opacity = 1;
    frames[2].easing = prLayoutEasingLinear();
    PrLayoutTrack track{};
    track.node = 1;
    track.effect = PrLayoutEffect_Opacity;
    track.timing = prLayoutTimingDefault();
    track.timing.duration = 1000;
    track.keyframes = frames;
    track.keyframeCount = 3;
    float opacity = 0;
    PrMatrix4 transform = prMatrix4Identity();
    REQUIRE(prLayoutTrackSample(&track, 250, PrVec2{0, 0}, &opacity, &transform));
    CHECK(double(opacity) == doctest::Approx(0.6));
    REQUIRE(prLayoutTrackSample(&track, 750, PrVec2{0, 0}, &opacity, &transform));
    CHECK(double(opacity) == doctest::Approx(0.6));
    CHECK_FALSE(prLayoutTrackSample(&track, 1500, PrVec2{0, 0}, &opacity, &transform));

    // A transform's percentages resolve against the reference box that the sample gives, also where the keyframes
    // interpolate as matrices.
    frames[0].transform = list({function(PrLayoutTransformKind_Translate, 0, 0, 0, 0.5f)});
    frames[1].transform = list({function(PrLayoutTransformKind_Scale, 2, 2, 1)});
    frames[2].transform = frames[0].transform;
    track.effect = PrLayoutEffect_Transform;
    REQUIRE(prLayoutTrackSample(&track, 0, PrVec2{200, 100}, &opacity, &transform));
    CHECK(double(transform.m[0][3]) == doctest::Approx(100));
    REQUIRE(prLayoutTrackSample(&track, 250, PrVec2{200, 100}, &opacity, &transform));
    CHECK(double(transform.m[0][3]) == doctest::Approx(50));
    CHECK(double(transform.m[0][0]) == doctest::Approx(1.5));
}
