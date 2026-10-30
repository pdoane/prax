#include "pr/draw_commands.h"
#include "pr/draw_filter.h"
#include "pr/draw_image.h"
#include "pr/draw_list.h"
#include "pr/draw_projection.h"
#include "pr/float.h"
#include "pr/vec.h"

#include "doctest/doctest.h"

#include <string>

namespace pr_impl {

namespace {

bool near(float a, float b, float tolerance = 1e-3f) {
    return absF32(a - b) <= tolerance;
}

bool nearPoint(PrVec2 a, PrVec2 b, float tolerance = 1e-3f) {
    return near(a.x, b.x, tolerance) && near(a.y, b.y, tolerance);
}

// A square of the plane tilted back about its horizontal center line, seen from `distance` in front, and placed at
// `center`.
PrMatrix4 tilted(float radians, float distance, PrVec2 center) {
    PrMatrix4 tilt = prMatrix4Rotate(PrVec3{1, 0, 0}, radians);
    PrMatrix4 eye = prMatrix4Perspective(distance);
    PrMatrix4 place = prMatrix4Translate(PrVec3{center.x, center.y, 0});
    PrMatrix4 seen = prMatrix4Concat(&tilt, &eye);
    return prMatrix4Concat(&seen, &place);
}

constexpr PrRect kEverywhere{-1e6f, -1e6f, 1e6f, 1e6f};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a projection's bounds are its plane's bounds, projected") {
    PrMatrix4 identity = prMatrix4Identity();
    PrRect flat = prDrawProjectionBounds(&identity, PrRect{1, 2, 3, 4});
    CHECK(nearPoint(PrVec2{flat.left, flat.top}, PrVec2{1, 2}));
    CHECK(nearPoint(PrVec2{flat.right, flat.bottom}, PrVec2{3, 4}));

    PrMatrix4 seen = tilted(0.8f, 100, PrVec2{50, 50});
    PrRect bounds{-20, -20, 20, 20};
    PrRect landed = prDrawProjectionBounds(&seen, bounds);
    PrVec2 farLeft{};
    PrVec2 nearLeft{};
    REQUIRE(prMatrix4MapPoint(&seen, PrVec2{-20, -20}, &farLeft));
    REQUIRE(prMatrix4MapPoint(&seen, PrVec2{-20, 20}, &nearLeft));
    CHECK(near(landed.top, farLeft.y));
    CHECK(near(landed.bottom, nearLeft.y));
    CHECK(near(landed.left, nearLeft.x));

    PrMatrix4 broken = identity;
    broken.m[0][0] = kInfinityF32;
    CHECK(prRectIsEmpty(prDrawProjectionBounds(&broken, bounds)));
}

TEST_CASE("a flat projection places its layer at the target's scale") {
    PrMatrix4 identity = prMatrix4Identity();
    PrAffine2 toTarget = prAffine2Concat(prAffine2Scale(PrVec2{2, 2}), prAffine2Translate(PrVec2{10, 0}));
    PrDrawProjection p{};
    REQUIRE(prDrawProjectionPlace(&identity, toTarget, PrRect{0, 0, 50, 50}, PrRect{0, 0, 60, 60}, 4096, &p));
    CHECK(near(p.scale, 2));
    // Only the part of the bounds that lands in the visible box is drawn.
    CHECK(nearPoint(PrVec2{p.area.left, p.area.top}, PrVec2{0, 0}));
    CHECK(nearPoint(PrVec2{p.area.right, p.area.bottom}, PrVec2{25, 30}));
    CHECK(nearPoint(PrVec2{p.targetBounds.left, p.targetBounds.top}, PrVec2{10, 0}));
    CHECK(nearPoint(PrVec2{p.targetBounds.right, p.targetBounds.bottom}, PrVec2{60, 60}));
    PrVec2 out{};
    REQUIRE(prMatrix3MapPoint(&p.layerToTarget, PrVec2{8, 6}, &out));
    CHECK(nearPoint(out, PrVec2{18, 6}));
    REQUIRE(prMatrix3MapPoint(&p.targetToLayer, PrVec2{18, 6}, &out));
    CHECK(nearPoint(out, PrVec2{8, 6}));

    // The layer's side is limited.
    REQUIRE(prDrawProjectionPlace(&identity, toTarget, PrRect{0, 0, 50, 50}, kEverywhere, 25, &p));
    CHECK(near(p.scale, 0.5f));

    // A projection that is affine on the plane keeps its exact scale, finer than the enclosing transform's.
    PrMatrix4 magnified = prMatrix4Scale(PrVec3{4, 4, 1});
    REQUIRE(prDrawProjectionPlace(&magnified, toTarget, PrRect{0, 0, 10, 10}, kEverywhere, 4096, &p));
    CHECK(near(p.scale, 8));
}

TEST_CASE("a tilted plane's layer is at the scale of the enclosing transform") {
    PrMatrix4 seen = tilted(0.8f, 100, PrVec2{50, 50});
    PrDrawProjection p{};
    PrRect bounds{-20, -20, 20, 20};
    // The plane's nearest part is magnified, but the layer is no finer than the device.
    PrMatrix3 plane = prMatrix3FromMatrix4(&seen);
    CHECK(prMatrix3MaxScale(&plane, bounds, 0.1f) > 1.1f);
    REQUIRE(prDrawProjectionPlace(&seen, prAffine2Scale(PrVec2{2, 2}), bounds, kEverywhere, 4096, &p));
    CHECK(near(p.scale, 2));
    REQUIRE(prDrawProjectionPlace(&seen, prAffine2Identity(), bounds, kEverywhere, 4096, &p));
    CHECK(near(p.scale, 1));
    // The layer's pixels land where the plane's points do.
    PrVec2 expected{};
    PrVec2 out{};
    REQUIRE(prMatrix4MapPoint(&seen, PrVec2{-20, 20}, &expected));
    REQUIRE(prMatrix3MapPoint(&p.layerToTarget, PrVec2{-20 * p.scale, 20 * p.scale}, &out));
    CHECK(nearPoint(out, expected, 1e-2f));

    // The quad of the layer's area is the projected bounds.
    PrPathPolyline quad{};
    prDrawProjectionQuad(&p, PrRect{-20 * p.scale, -20 * p.scale, 20 * p.scale, 20 * p.scale}, &quad);
    REQUIRE(quad.pointCount == 4u);
    CHECK(nearPoint(quad.points[3], expected, 1e-2f));
    prPathPolylineReset(&quad);
}

TEST_CASE("a steep plane's layer holds at most twice the target pixels that it covers") {
    // Tilted nearly edge-on and reaching far back: its nearest part would need a layer many times its target area.
    PrMatrix4 steep = tilted(1.45f, 400, PrVec2{100, 100});
    PrRect bounds{-300, -300, 300, 300};
    PrRect visible{0, 0, 200, 200};
    PrDrawProjection p{};
    REQUIRE(prDrawProjectionPlace(&steep, prAffine2Identity(), bounds, visible, 4096, &p));
    // The visible part covers no more than its target bounds.
    float target = prRectWidth(p.targetBounds) * prRectHeight(p.targetBounds);
    float layer = prRectWidth(p.area) * prRectHeight(p.area) * p.scale * p.scale;
    CHECK(target > 0.0f);
    CHECK(layer <= 2 * target);
    // The cap lowers the scale below the device's.
    CHECK(p.scale < 0.9f);
}

TEST_CASE("a projection's tangent agrees with it at the center of the placed area") {
    PrMatrix4 seen = tilted(0.8f, 100, PrVec2{50, 50});
    PrAffine2 toTarget = prAffine2Concat(prAffine2Scale(PrVec2{2, 2}), prAffine2Translate(PrVec2{5, 7}));
    PrDrawProjection p{};
    REQUIRE(prDrawProjectionPlace(&seen, toTarget, PrRect{-20, -20, 20, 10}, kEverywhere, 4096, &p));
    PrAffine2 tangent = prDrawProjectionTangent(&p);
    // The center of the area lands where the projection puts it, and a nearby point lands near it.
    PrMatrix3 plane = prMatrix3FromMatrix4(&seen);
    PrMatrix3 target = prMatrix3FromAffine2(toTarget);
    PrMatrix3 h = prMatrix3Concat(&plane, &target);
    for (PrVec2 at : {PrVec2{0, -5}, PrVec2{0.1f, -5.1f}}) {
        PrVec2 expected{};
        REQUIRE(prMatrix3MapPoint(&h, at, &expected));
        CHECK(nearPoint(prAffine2MapPoint(tangent, at), expected, 1e-2f));
    }
    // An affine projection's tangent is the projection.
    PrMatrix4 identity = prMatrix4Identity();
    REQUIRE(prDrawProjectionPlace(&identity, toTarget, PrRect{0, 0, 10, 10}, kEverywhere, 4096, &p));
    tangent = prDrawProjectionTangent(&p);
    CHECK(nearPoint(prAffine2MapPoint(tangent, PrVec2{3, 4}), prAffine2MapPoint(toTarget, PrVec2{3, 4})));
}

TEST_CASE("a plane partly behind the eye is cut, and one wholly behind it or edge-on draws nothing") {
    // Tilted steeply under a near eye: the far half of the plane passes behind the eye.
    PrMatrix4 steep = tilted(1.2f, 10, PrVec2{50, 50});
    PrRect bounds{-40, -40, 40, 40};
    PrDrawProjection p{};
    REQUIRE(prDrawProjectionPlace(&steep, prAffine2Identity(), bounds, PrRect{0, 0, 100, 100}, 4096, &p));
    CHECK(p.area.top > bounds.top);
    CHECK(p.targetBounds.left > -1e-3f);
    CHECK(p.targetBounds.bottom < 100.001f);
    PrPathPolyline cut{};
    prDrawProjectionQuad(&p, PrRect{-40 * p.scale, -40 * p.scale, 40 * p.scale, 40 * p.scale}, &cut);
    CHECK(cut.pointCount >= 4u);
    prPathPolylineReset(&cut);

    PrMatrix4 behind = prMatrix4Identity();
    behind.m[3][3] = -1;
    CHECK(!prDrawProjectionPlace(&behind, prAffine2Identity(), bounds, kEverywhere, 4096, &p));

    PrMatrix4 edge = prMatrix4Rotate(PrVec3{0, 1, 0}, kPiF32 / 2);
    PrMatrix4 eye = prMatrix4Perspective(100);
    PrMatrix4 edgeOn = prMatrix4Concat(&edge, &eye);
    CHECK(!prDrawProjectionPlace(&edgeOn, prAffine2Identity(), bounds, kEverywhere, 4096, &p));

    PrMatrix4 broken = prMatrix4Identity();
    broken.m[3][0] = kInfinityF32;
    CHECK(!prDrawProjectionPlace(&broken, prAffine2Identity(), bounds, kEverywhere, 4096, &p));

    // No part of the plane lands in the visible box.
    PrMatrix4 identity = prMatrix4Identity();
    CHECK(!prDrawProjectionPlace(&identity, prAffine2Identity(), bounds, PrRect{50, 50, 60, 60}, 4096, &p));
}

TEST_CASE("a projected group is copied, dumped and bounded by its projected plane") {
    PrDrawList* list = prDrawListCreate();
    PrMatrix4 seen = tilted(0.8f, 100, PrVec2{50, 50});
    PrDrawGroup group = prDrawGroupDefault();
    group.bounds = PrRect{-20, -20, 20, 20};
    group.projection = &seen;
    CHECK(prDrawGroupHasEffect(&group));
    prDrawListPushGroup(list, &group);
    // Contents far outside the plane's bounds, and pixels written into the layer, do not reach the list's bounds.
    prDrawListRect(list, PrRect{-1000, -1000, 1000, 1000}, {.color = PrColor{1, 0, 0, 1}});
    const uint8_t texels[4] = {255, 0, 0, 255};
    PrDrawImageDesc desc{
        PrPixelView{texels, 4, {1, 1}}, PrPixelFormat_Rgba8, PrDrawAlpha_Premultiplied,
        PrColorSpace_Srgb,
    };
    const PrDrawImage* image = nullptr;
    REQUIRE(prDrawImageCreate(&desc, &image) == PrStatus_Ok);
    prDrawListWritePixels(list, image, PrIVec2{500, 500});
    prDrawImageRelease(image);
    prDrawListPopGroup(list);
    seen.m[0][0] = 0;
    prDrawListFreeze(list);

    const PrDrawCommand* commands = prDrawListCommands(list);
    REQUIRE(commands[0].group->projection != nullptr);
    CHECK(commands[0].group->projection != &seen);
    CHECK(commands[0].group->projection->m[0][0] == 1.0f);
    PrRect bounds = prDrawListBounds(list);
    PrMatrix4 original = tilted(0.8f, 100, PrVec2{50, 50});
    PrRect landed = prDrawProjectionBounds(&original, PrRect{-20, -20, 20, 20});
    CHECK(near(bounds.left, landed.left));
    CHECK(near(bounds.bottom, landed.bottom));
    CHECK(!prDrawListWritesPixels(list));

    PrString text = prDrawListDump(list);
    PrStringView view = prStringView(&text);
    std::string dumped(view.data, view.size);
    prStringReset(&text);
    CHECK(dumped.find("push-group -20 -20 20 20 opacity 1 projection 1 -0.358") == 0);
    prDrawListRelease(list);
}

TEST_CASE("a backdrop in or on a projected group reads beneath the list") {
    PrDrawFilterBuilder* builder = prDrawFilterBuilderCreate();
    PrDrawFilterNode node{};
    node.kind = PrDrawFilterKind_Offset;
    node.inputs[0] = PrDrawFilterInput_Backdrop;
    prDrawFilterBuilderAdd(builder, &node);
    const PrDrawFilter* filter = prDrawFilterCreate(builder);
    prDrawFilterBuilderDestroy(builder);

    PrMatrix4 identity = prMatrix4Identity();
    PrDrawGroup projected = prDrawGroupDefault();
    projected.bounds = PrRect{0, 0, 10, 10};
    projected.projection = &identity;
    projected.backdrop = filter;
    PrDrawGroup inner = prDrawGroupDefault();
    inner.backdrop = filter;
    PrDrawList* list = prDrawListCreate();
    prDrawListPushGroup(list, &projected);
    prDrawListPopGroup(list);
    prDrawListFreeze(list);
    CHECK(prDrawListReadsBackdrop(list));
    prDrawListRelease(list);

    // A projection alone does not make a backdrop root, and a backdrop does.
    CHECK(prDrawGroupIsBackdropRoot(&projected));
    projected.backdrop = nullptr;
    CHECK(!prDrawGroupIsBackdropRoot(&projected));
    projected.opacity = 0.5f;
    CHECK(prDrawGroupIsBackdropRoot(&projected));
    projected.opacity = 1;
    list = prDrawListCreate();
    prDrawListPushGroup(list, &projected);
    prDrawListPushGroup(list, &inner);
    prDrawListPopGroup(list);
    prDrawListPopGroup(list);
    prDrawListFreeze(list);
    CHECK(prDrawListReadsBackdrop(list));
    prDrawListRelease(list);
    prDrawFilterRelease(filter);
}
