// Effects on boxes (css-masking-1, filter-effects-1, filter-effects-2, compositing-1): basic shapes, clip paths, the
// clip rectangle, mask layers and borders, filters, backdrop filters and blending, what they make of a box, and where
// it is hit.

#include "pr/draw_list.h"
#include "pr/layout_query.h"
#include "pr/layout_scene.h"
#include "pr/layout_style.h"
#include "pr/layout_tree.h"

#include "layout/tests/layout_fixture.h"

#include "doctest/doctest.h"

#include <cmath>
#include <string>
#include <vector>

namespace pr_impl {

namespace {

// A stage 200 by 200 at the top left, with boxes in it styled by records that may name a compositing group.
struct Stage {
    Fixture f;
    PrLayoutNode stage = kPrLayoutNodeNone;
    float radius = 0;               // the radius of every corner of the boxes that follow

    Stage() {
        PrLayoutBoxStyle box = block();
        box.width = prLayoutLengthPx(200);
        box.height = prLayoutLengthPx(200);
        box.position = PrLayoutPosition_Relative;
        stage = f.element(kPrLayoutNodeNone, box);
    }

    // Adds a box under `parent` styled by `box`, a background of `color`, corners of `radius`, and `compositing`, if
    // given.
    PrLayoutNode add(PrLayoutNode parent, PrLayoutBoxStyle box, PrColor color,
            const PrLayoutCompositingStyle* PR_NULLABLE compositing = nullptr,
            const PrLayoutEffectStyle* PR_NULLABLE effects = nullptr) {
        PrLayoutStyle record = prLayoutStyleDefault();
        record.groups[PrLayoutGroup_Box] = prLayoutBoxStyleIntern(f.tree, &box);
        PrLayoutDecorationStyle decoration = background(color);
        for (PrLayoutLength& length : decoration.radii)
            length = prLayoutLengthPx(radius);
        record.groups[PrLayoutGroup_Decoration] = prLayoutDecorationStyleIntern(f.tree, &decoration);
        if (compositing != nullptr)
            record.groups[PrLayoutGroup_Compositing] = prLayoutCompositingStyleIntern(f.tree, compositing);
        if (effects != nullptr)
            record.groups[PrLayoutGroup_Effect] = prLayoutEffectStyleIntern(f.tree, effects);
        return prLayoutNodeCreateElement(f.tree, parent, prLayoutStyleIntern(f.tree, &record));
    }

    std::vector<uint8_t> pixels() {
        f.layout(px(200, 200));
        return render(f.tree, 200, 200);
    }
};

// Returns a block `width` by `height`.
PrLayoutBoxStyle sized(float width, float height) {
    PrLayoutBoxStyle box = block();
    box.width = prLayoutLengthPx(width);
    box.height = prLayoutLengthPx(height);
    return box;
}

// Returns an absolutely positioned box `width` by `height` at `left`, `top`.
PrLayoutBoxStyle absolute(float left, float top, float width, float height,
        PrLayoutPosition position = PrLayoutPosition_Absolute) {
    PrLayoutBoxStyle box = sized(width, height);
    box.position = position;
    box.inset[PrRectSide_Left] = prLayoutLengthPx(left);
    box.inset[PrRectSide_Top] = prLayoutLengthPx(top);
    return box;
}

constexpr PrColor kGreen{0, 1, 0, 1};
constexpr PrColor kRed{1, 0, 0, 1};
constexpr PrColor kBlue{0, 0, 1, 1};

const std::string kClear = "0 0 0 0";
const std::string kGreenPixel = "0 255 0 255";
const std::string kRedPixel = "255 0 0 255";
const std::string kBluePixel = "0 0 255 255";

// Returns the bounds of a shape's outline in `box`.
PrRect outlineOf(const PrLayoutShape& shape, PrRect box) {
    PrPath* path = prPathCreate();
    prLayoutShapePath(&shape, nullptr, box, path);
    PrRect bounds = prPathVerbCount(path) != 0 ? prPathTightBounds(path) : PrRect{};
    prPathDestroy(path);
    return bounds;
}

bool near(PrRect a, PrRect b) {
    bool same = std::fabs(a.left - b.left) < 0.05f && std::fabs(a.top - b.top) < 0.05f &&
        std::fabs(a.right - b.right) < 0.05f && std::fabs(a.bottom - b.bottom) < 0.05f;
    if (!same)
        MESSAGE("{" << a.left << ", " << a.top << ", " << a.right << ", " << a.bottom << "}");
    return same;
}

// Returns a group of a basic shape interned in `tree`, clipping in the border box.
PrLayoutCompositingStyle clipShape(PrLayoutTree* tree, const PrLayoutShape& shape) {
    PrLayoutCompositingStyle c = prLayoutCompositingStyleDefault();
    c.clipKind = PrLayoutClipKind_Shape;
    c.clipShape = prLayoutShapeIntern(tree, &shape);
    return c;
}

// Returns the nodes of a filter list of one function.
std::vector<PrDrawFilterNode> nodesOf(const PrLayoutFilter& filter) {
    PrDrawFilterBuilder* builder = prDrawFilterBuilderCreate();
    prLayoutFilterAppend(&filter, PrDrawFilterInput_Source, PrImageEdgeMode_None, builder);
    const PrDrawFilter* made = prDrawFilterCreate(builder);
    prDrawFilterBuilderDestroy(builder);
    const PrDrawFilterNode* first = prDrawFilterNodes(made);
    std::vector<PrDrawFilterNode> nodes(first, first + prDrawFilterNodeCount(made));
    prDrawFilterRelease(made);
    return nodes;
}

// A source that clips to the left half of the reference box, and is hit there.
bool leftHalf(void*, PrRect box, PrDrawList* list, PrLayoutSourceMask* out) {
    PrRect half{box.left, box.top, (box.left + box.right) / 2, box.bottom};
    prDrawListRect(list, half, PrDrawPaint{.color = PR_COLOR16(1, 1, 1, 1)});
    *out = PrLayoutSourceMask{.area = half, .mode = PrDrawMaskMode_Alpha, .space = PrColorSpace_Srgb};
    return true;
}

bool inLeftHalf(void*, PrRect box, PrVec2 point) {
    return point.x < (box.left + box.right) / 2;
}

constexpr PrLayoutSourceFunctions kLeftHalf = {.draw = leftHalf, .contains = inLeftHalf};

const PrLayoutColorStop kRedStops[] = {
    {.position = prLayoutLengthAuto(), .color = PR_COLOR16(1, 0, 0, 1)},
    {.position = prLayoutLengthAuto(), .color = PR_COLOR16(1, 0, 0, 1)},
};

// Returns a layer of a red gradient `width` by `height` at the top left, drawn once.
PrLayoutBackgroundLayer redLayer(float width, float height) {
    PrLayoutBackgroundLayer layer = prLayoutBackgroundLayerDefault();
    layer.kind = PrLayoutImageKind_Gradient;
    layer.gradient = prLayoutGradientDefault();
    layer.gradient.stops = kRedStops;
    layer.gradient.stopCount = 2;
    layer.repeatX = PrLayoutBackgroundRepeat_NoRepeat;
    layer.repeatY = PrLayoutBackgroundRepeat_NoRepeat;
    layer.sizeX = prLayoutLengthPx(width);
    layer.sizeY = prLayoutLengthPx(height);
    return layer;
}

// Adds a box under `parent` styled by `box`, with a background of `color` under `count` layers, top layer first.
PrLayoutNode addLayered(Stage& s, PrLayoutNode parent, PrLayoutBoxStyle box, PrColor color,
        const PrLayoutBackgroundLayer* layers, uint32_t count) {
    PrLayoutStyle record = prLayoutStyleDefault();
    record.groups[PrLayoutGroup_Box] = prLayoutBoxStyleIntern(s.f.tree, &box);
    PrLayoutDecorationStyle decoration = background(color);
    decoration.backgroundLayers = prLayoutBackgroundIntern(s.f.tree, layers, count);
    record.groups[PrLayoutGroup_Decoration] = prLayoutDecorationStyleIntern(s.f.tree, &decoration);
    return prLayoutNodeCreateElement(s.f.tree, parent, prLayoutStyleIntern(s.f.tree, &record));
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("an inset's insets and corners, and a circle's extent, resolve in the reference box") {
    PrRect box{10, 20, 110, 80};
    PrLayoutShape inset = prLayoutShapeDefault();
    for (PrLayoutLength& side : inset.inset)
        side = prLayoutLengthPercent(10);
    CHECK(near(outlineOf(inset, box), PrRect{20, 26, 100, 74}));
    // A pair of insets that meet leaves no shape.
    inset.inset[PrRectSide_Left] = prLayoutLengthPx(150);
    inset.inset[PrRectSide_Right] = prLayoutLengthPx(50);
    CHECK(near(outlineOf(inset, box), PrRect{}));
    PrLayoutShape circle = prLayoutShapeDefault();
    circle.kind = PrLayoutShapeKind_Circle;
    circle.extentX = PrLayoutRadialExtent_FarthestSide;
    circle.centerX = prLayoutLengthPx(20);
    CHECK(near(outlineOf(circle, box), PrRect{-50, -30, 110, 130}));
    PrLayoutShape ellipse = prLayoutShapeDefault();
    ellipse.kind = PrLayoutShapeKind_Ellipse;
    ellipse.extentX = PrLayoutRadialExtent_Radii;
    ellipse.radiusX = prLayoutLengthPercent(50);
    CHECK(near(outlineOf(ellipse, box), PrRect{10, 20, 110, 80}));
}

TEST_CASE("a polygon's round arcs each vertex, at most half of its shorter side") {
    PrLayoutLength points[] = {
        prLayoutLengthPx(0),   prLayoutLengthPx(0),   prLayoutLengthPx(100), prLayoutLengthPx(0),
        prLayoutLengthPx(100), prLayoutLengthPx(100), prLayoutLengthPx(0),   prLayoutLengthPx(100),
    };
    PrLayoutShape square = prLayoutShapeDefault();
    square.kind = PrLayoutShapeKind_Polygon;
    square.pointCount = 4;
    square.points = points;
    PrPath* path = prPathCreate();
    square.round = 20;
    prLayoutShapePath(&square, nullptr, PrRect{0, 0, 100, 100}, path);
    // The corner's arc keeps it 20 - 20 / sqrt(2) in from the vertex along the diagonal.
    CHECK_FALSE(prPathContains(path, PrVec2{2, 2}, PrPathFillRule_NonZero));
    CHECK(prPathContains(path, PrVec2{8, 8}, PrPathFillRule_NonZero));
    prPathClear(path);
    square.round = 500;
    prLayoutShapePath(&square, nullptr, PrRect{0, 0, 100, 100}, path);
    // Clamped to half a side, the square becomes a circle of radius 50.
    CHECK_FALSE(prPathContains(path, PrVec2{14, 14}, PrPathFillRule_NonZero));
    CHECK(prPathContains(path, PrVec2{50, 2}, PrPathFillRule_NonZero));
    prPathDestroy(path);
}

TEST_CASE("filter functions become nodes that compute in sRGB, as their filter-effects-1 equivalents do") {
    PrLayoutFilter gray{.kind = PrLayoutFilterKind_Grayscale, .amount = 1};
    std::vector<PrDrawFilterNode> nodes = nodesOf(gray);
    REQUIRE(nodes.size() == 1);
    CHECK(nodes[0].kind == PrDrawFilterKind_ColorMatrix);
    CHECK(nodes[0].space == PrColorSpace_Srgb);
    CHECK(nodes[0].matrix.m[0][0] == doctest::Approx(0.2126f));
    PrLayoutFilter invert{.kind = PrLayoutFilterKind_Invert, .amount = 1};
    nodes = nodesOf(invert);
    CHECK(nodes[0].transfer[PrDrawChannel_R].slope == -1);
    CHECK(nodes[0].transfer[PrDrawChannel_R].intercept == 1);
    CHECK(nodes[0].transfer[PrDrawChannel_A].kind == PrDrawTransferKind_Identity);
    PrLayoutFilter opacity{.kind = PrLayoutFilterKind_Opacity, .amount = 0.25f};
    nodes = nodesOf(opacity);
    CHECK(nodes[0].transfer[PrDrawChannel_A].slope == 0.25f);
    PrLayoutFilter contrast{.kind = PrLayoutFilterKind_Contrast, .amount = 2};
    nodes = nodesOf(contrast);
    CHECK(nodes[0].transfer[PrDrawChannel_G].intercept == -0.5f);
    PrLayoutFilter blur{.kind = PrLayoutFilterKind_Blur, .amount = 3};
    nodes = nodesOf(blur);
    CHECK(nodes[0].kind == PrDrawFilterKind_Blur);
    CHECK(nodes[0].blur.sigma.x == 3);
    CHECK(nodes[0].blur.edge == PrImageEdgeMode_None);
    PrLayoutFilter shadow{.kind = PrLayoutFilterKind_DropShadow, .offset = {1, 2}, .sigma = 3};
    nodes = nodesOf(shadow);
    CHECK(nodes[0].kind == PrDrawFilterKind_DropShadow);
    CHECK(nodes[0].shadow.offset.y == 2);
}

TEST_CASE("a clip path of a basic shape clips the box and what it holds, without moving either") {
    Stage s;
    PrLayoutShape inset = prLayoutShapeDefault();
    for (PrLayoutLength& side : inset.inset)
        side = prLayoutLengthPx(10);
    PrLayoutCompositingStyle c = clipShape(s.f.tree, inset);
    PrLayoutNode box = s.add(s.stage, sized(100, 100), kGreen, &c);
    PrLayoutNode child = s.add(box, sized(150, 20), kBlue);
    std::vector<uint8_t> pixels = s.pixels();
    CHECK(pixel(pixels, 200, 5, 50) == kClear);
    CHECK(pixel(pixels, 200, 50, 50) == kGreenPixel);
    CHECK(pixel(pixels, 200, 50, 15) == kBluePixel);
    CHECK(pixel(pixels, 200, 120, 15) == kClear);
    CHECK(near(s.f.bounds(child), PrRect{0, 0, 150, 20}));
    CHECK(verified(s.f.tree));
}

TEST_CASE("a clip path of a reference box takes the box's corners, from the border box's radii") {
    Stage s;
    PrLayoutCompositingStyle c = prLayoutCompositingStyleDefault();
    c.clipKind = PrLayoutClipKind_Box;
    c.clipBox = PrLayoutGeometryBox_BorderBox;
    s.radius = 40;
    s.add(s.stage, sized(100, 100), kGreen, &c);
    std::vector<uint8_t> pixels = s.pixels();
    CHECK(pixel(pixels, 200, 3, 3) == kClear);
    CHECK(pixel(pixels, 200, 50, 3) == kGreenPixel);
    // The padding box's corners are the border box's, less the border.
    PrLayoutBoxStyle bordered = sized(100, 100);
    for (PrLayoutUnit& width : bordered.border)
        width = prLayoutUnitFromPx(10);
    c.clipBox = PrLayoutGeometryBox_PaddingBox;
    Stage t;
    t.radius = 40;
    t.add(t.stage, bordered, kGreen, &c);
    pixels = t.pixels();
    CHECK(pixel(pixels, 200, 5, 50) == kClear);
    CHECK(pixel(pixels, 200, 15, 50) == kGreenPixel);
    CHECK(pixel(pixels, 200, 13, 13) == kClear);
}

TEST_CASE("a box's background and border follow its corners, and it is hit only within them") {
    Stage s;
    s.radius = 40;
    PrLayoutNode box = s.add(s.stage, sized(100, 100), kGreen);
    std::vector<uint8_t> pixels = s.pixels();
    CHECK(pixel(pixels, 200, 3, 3) == kClear);
    CHECK(pixel(pixels, 200, 96, 96) == kClear);
    CHECK(pixel(pixels, 200, 50, 3) == kGreenPixel);
    CHECK(pixel(pixels, 200, 15, 15) == kGreenPixel);
    CHECK(s.f.hit(PrVec2{3, 3}) == s.stage);
    CHECK(s.f.hit(PrVec2{15, 15}) == box);
    CHECK(verified(s.f.tree));
}

TEST_CASE("a container clips its content to the curves of its padding box") {
    Stage s;
    s.radius = 40;
    PrLayoutBoxStyle clip = sized(100, 100);
    clip.overflowX = PrLayoutOverflow_Hidden;
    clip.overflowY = PrLayoutOverflow_Hidden;
    for (PrLayoutUnit& width : clip.border)
        width = prLayoutUnitFromPx(10);
    PrLayoutNode container = s.add(s.stage, clip, PrColor{0, 0, 0, 0});
    s.radius = 0;
    PrLayoutNode child = s.add(container, sized(150, 150), kBlue);
    std::vector<uint8_t> pixels = s.pixels();
    // The padding box's corners are the border box's less the border: 30px about (40, 40).
    CHECK(pixel(pixels, 200, 13, 13) == kClear);
    CHECK(pixel(pixels, 200, 50, 12) == kBluePixel);
    CHECK(pixel(pixels, 200, 22, 22) == kBluePixel);
    CHECK(s.f.hit(PrVec2{13, 13}) == container);
    CHECK(s.f.hit(PrVec2{22, 22}) == child);
    CHECK(verified(s.f.tree));
}

TEST_CASE("a change of corners paints the box again, and its clip path of the border box with it") {
    Stage s;
    PrLayoutCompositingStyle c = prLayoutCompositingStyleDefault();
    c.clipKind = PrLayoutClipKind_Box;
    c.clipBox = PrLayoutGeometryBox_BorderBox;
    PrLayoutNode box = s.add(s.stage, sized(100, 100), kGreen, &c);
    PrLayoutNode child = s.add(box, sized(100, 100), kBlue);
    std::vector<uint8_t> pixels = s.pixels();
    CHECK(pixel(pixels, 200, 3, 3) == kBluePixel);
    PrLayoutDecorationStyle decoration = background(kGreen);
    for (PrLayoutLength& length : decoration.radii)
        length = prLayoutLengthPx(40);
    PrLayoutStyle record = prLayoutStyleRecord(s.f.tree, prLayoutNodeStyle(s.f.tree, box));
    record.groups[PrLayoutGroup_Decoration] = prLayoutDecorationStyleIntern(s.f.tree, &decoration);
    prLayoutNodeSetStyle(s.f.tree, box, prLayoutStyleIntern(s.f.tree, &record));
    pixels = s.pixels();
    CHECK(pixel(pixels, 200, 3, 3) == kClear);
    CHECK(pixel(pixels, 200, 50, 50) == kBluePixel);
    CHECK(s.f.hit(PrVec2{3, 3}) == s.stage);
    CHECK(s.f.hit(PrVec2{50, 50}) == child);
    CHECK(verified(s.f.tree));
}

TEST_CASE("the clip rectangle clips a box and what it holds, as a container that makes no stacking context") {
    PrLayoutCompositingStyle c = prLayoutCompositingStyleDefault();
    c.clips = true;
    c.clip[PrRectSide_Top] = prLayoutLengthPx(10);
    c.clip[PrRectSide_Right] = prLayoutLengthPx(60);
    c.clip[PrRectSide_Bottom] = prLayoutLengthAuto();
    c.clip[PrRectSide_Left] = prLayoutLengthPx(-20);
    Stage s;
    PrLayoutBoxStyle box = absolute(0, 0, 100, 100);
    box.clipped = 1;
    PrLayoutNode clipped = s.add(s.stage, box, kGreen, &c);
    // The child at z-index 3 stacks in the stage's context, over the red box at 2 that follows, and is still clipped.
    PrLayoutBoxStyle above = absolute(0, 0, 150, 100);
    above.zIndexed = 1;
    PrLayoutEffectStyle level = prLayoutEffectStyleDefault();
    level.zIndex = 3;
    s.add(clipped, above, kBlue, nullptr, &level);
    PrLayoutBoxStyle middle = absolute(20, 40, 20, 20);
    middle.zIndexed = 1;
    PrLayoutEffectStyle two = prLayoutEffectStyleDefault();
    two.zIndex = 2;
    s.add(s.stage, middle, kRed, nullptr, &two);
    std::vector<uint8_t> pixels = s.pixels();
    CHECK(pixel(pixels, 200, 30, 5) == kClear);
    CHECK(pixel(pixels, 200, 30, 95) == kBluePixel);
    CHECK(pixel(pixels, 200, 30, 50) == kBluePixel);
    CHECK(pixel(pixels, 200, 70, 50) == kClear);
    CHECK(s.f.hit(PrVec2{30, 50}) != clipped);
    CHECK(s.f.hit(PrVec2{70, 50}) == s.stage);
    const PrLayoutScene* scene = prLayoutTreeScene(s.f.tree);
    CHECK_FALSE(prLayoutSceneHasLayer(scene, clipped));
    prLayoutSceneRelease(scene);
    CHECK(verified(s.f.tree));
}

TEST_CASE("a clip path clips where a box is hit, and a mask does not") {
    Stage s;
    PrLayoutShape circle = prLayoutShapeDefault();
    circle.kind = PrLayoutShapeKind_Circle;
    PrLayoutCompositingStyle c = clipShape(s.f.tree, circle);
    PrLayoutNode clipped = s.add(s.stage, sized(100, 100), kGreen, &c);
    PrLayoutMaskLayer layer = prLayoutMaskLayerDefault();
    PrLayoutMask mask{.layerCount = 1, .layers = &layer, .border = prLayoutMaskBorderDefault()};
    PrLayoutCompositingStyle m = prLayoutCompositingStyleDefault();
    m.mask = prLayoutMaskIntern(s.f.tree, &mask);
    PrLayoutNode masked = s.add(s.stage, sized(100, 100), kGreen, &m);
    s.f.layout(px(200, 200));
    CHECK(s.f.hit(PrVec2{50, 50}) == clipped);
    CHECK(s.f.hit(PrVec2{3, 3}) == s.stage);
    // A transparent mask layer leaves nothing drawn, and the box is hit all the same.
    CHECK(s.f.hit(PrVec2{3, 103}) == masked);
}

TEST_CASE("a clip path from a source clips the box to what the source draws, and to where it is hit") {
    Stage s;
    PrLayoutCompositingStyle c = prLayoutCompositingStyleDefault();
    c.clipKind = PrLayoutClipKind_Source;
    c.clipSource = PrLayoutSource{.functions = &kLeftHalf};
    PrLayoutNode box = s.add(s.stage, sized(100, 100), kGreen, &c);
    std::vector<uint8_t> pixels = s.pixels();
    CHECK(pixel(pixels, 200, 25, 50) == kGreenPixel);
    CHECK(pixel(pixels, 200, 75, 50) == kClear);
    CHECK(s.f.hit(PrVec2{25, 50}) == box);
    CHECK(s.f.hit(PrVec2{75, 50}) == s.stage);
}

TEST_CASE("mask layers place their images as backgrounds do, and composite from the bottom layer up") {
    Stage s;
    PrLayoutColorStop stops[] = {
        {.position = prLayoutLengthAuto(), .color = PR_COLOR16(0, 0, 0, 1)},
        {.position = prLayoutLengthAuto(), .color = PR_COLOR16(0, 0, 0, 0)},
    };
    PrLayoutMaskLayer layers[2] = {prLayoutMaskLayerDefault(), prLayoutMaskLayerDefault()};
    // The top layer, a gradient from opaque at the left to transparent at the right, in the left half alone.
    layers[0].image.kind = PrLayoutImageKind_Gradient;
    layers[0].image.gradient = prLayoutGradientDefault();
    layers[0].image.gradient.angle = 90;
    layers[0].image.gradient.stops = stops;
    layers[0].image.gradient.stopCount = 2;
    layers[0].image.repeatX = PrLayoutBackgroundRepeat_NoRepeat;
    layers[0].image.sizeX = prLayoutLengthPercent(50);
    // The bottom layer, an opaque gradient over the bottom half, which the top layer subtracts from.
    PrLayoutColorStop opaque[] = {
        {.position = prLayoutLengthAuto(), .color = PR_COLOR16(0, 0, 0, 1)},
        {.position = prLayoutLengthAuto(), .color = PR_COLOR16(0, 0, 0, 1)},
    };
    layers[1].image.kind = PrLayoutImageKind_Gradient;
    layers[1].image.gradient = prLayoutGradientDefault();
    layers[1].image.gradient.stops = opaque;
    layers[1].image.gradient.stopCount = 2;
    layers[1].image.repeatY = PrLayoutBackgroundRepeat_NoRepeat;
    layers[1].image.sizeY = prLayoutLengthPercent(50);
    layers[1].image.positionY = prLayoutLengthPercent(100);
    layers[0].composite = PrDrawComposite_Xor;
    PrLayoutMask mask{.layerCount = 2, .layers = layers, .border = prLayoutMaskBorderDefault()};
    PrLayoutCompositingStyle c = prLayoutCompositingStyleDefault();
    c.mask = prLayoutMaskIntern(s.f.tree, &mask);
    s.add(s.stage, sized(100, 100), kGreen, &c);
    std::vector<uint8_t> pixels = s.pixels();
    // Exclusion: the top layer alone at the top left, the bottom layer alone at the bottom right, and nearly neither
    // where both are nearly opaque. Pixels are premultiplied.
    CHECK(pixel(pixels, 200, 1, 25) == "0 247 0 247");
    CHECK(pixel(pixels, 200, 75, 25) == kClear);
    CHECK(pixel(pixels, 200, 75, 75) == kGreenPixel);
    CHECK(pixel(pixels, 200, 1, 75) == "0 8 0 8");
}

TEST_CASE("a mask border masks the border image area by its nine pieces, and the middle masks nothing without fill") {
    // An image 30 by 30, opaque in its 10px frame and transparent in its middle.
    PrDrawList* image = prDrawListCreate();
    prDrawListRect(image, PrRect{0, 0, 30, 10}, PrDrawPaint{.color = PR_COLOR16(0, 0, 0, 1)});
    prDrawListRect(image, PrRect{0, 20, 30, 30}, PrDrawPaint{.color = PR_COLOR16(0, 0, 0, 1)});
    prDrawListRect(image, PrRect{0, 10, 10, 20}, PrDrawPaint{.color = PR_COLOR16(0, 0, 0, 1)});
    prDrawListRect(image, PrRect{20, 10, 30, 20}, PrDrawPaint{.color = PR_COLOR16(0, 0, 0, 1)});
    for (bool fill : {false, true}) {
        Stage s;
        PrLayoutMask mask{.border = prLayoutMaskBorderDefault()};
        mask.border.image.kind = PrLayoutImageKind_List;
        mask.border.image.list = image;
        mask.border.image.natural = natural(30, 30);
        for (PrLayoutLength& slice : mask.border.slice)
            slice = prLayoutLengthPx(10);
        mask.border.fill = fill;
        PrLayoutCompositingStyle c = prLayoutCompositingStyleDefault();
        c.mask = prLayoutMaskIntern(s.f.tree, &mask);
        s.add(s.stage, sized(100, 100), kGreen, &c);
        std::vector<uint8_t> pixels = s.pixels();
        CHECK(pixel(pixels, 200, 5, 50) == kGreenPixel);
        CHECK(pixel(pixels, 200, 50, 50) == (fill ? kClear : kGreenPixel));
    }
    prDrawListRelease(image);
}

TEST_CASE("a filter applies to the box and what it holds, and holds the boxes out of flow in it") {
    Stage s;
    PrLayoutFilter gray{.kind = PrLayoutFilterKind_Grayscale, .amount = 1};
    PrLayoutCompositingStyle c = prLayoutCompositingStyleDefault();
    c.filter = prLayoutFilterIntern(s.f.tree, &gray, 1);
    PrLayoutBoxStyle box = sized(100, 100);
    box.margin[PrRectSide_Top] = prLayoutLengthPx(50);
    box.filtered = 1;
    PrLayoutNode filtered = s.add(s.stage, box, kRed, &c);
    PrLayoutNode fixed = s.add(filtered, absolute(10, 10, 20, 20, PrLayoutPosition_Fixed), kBlue);
    std::vector<uint8_t> pixels = s.pixels();
    CHECK(pixel(pixels, 200, 80, 80) == "54 54 54 255");
    CHECK(near(s.f.bounds(fixed), PrRect{10, 60, 30, 80}));
    CHECK(pixel(pixels, 200, 20, 70) == "18 18 18 255");
}

TEST_CASE("a backdrop filter filters what lies beneath the box, within its border box") {
    Stage s;
    PrLayoutNode under = s.add(s.stage, sized(200, 100), kRed);
    PrLayoutFilter gray{.kind = PrLayoutFilterKind_Grayscale, .amount = 1};
    PrLayoutCompositingStyle c = prLayoutCompositingStyleDefault();
    c.backdrop = prLayoutFilterIntern(s.f.tree, &gray, 1);
    PrLayoutBoxStyle box = absolute(50, 0, 50, 50);
    box.filtered = 1;
    s.add(s.stage, box, PrColor{0, 0, 0, 0}, &c);
    (void)under;
    std::vector<uint8_t> pixels = s.pixels();
    CHECK(pixel(pixels, 200, 75, 25) == "54 54 54 255");
    CHECK(pixel(pixels, 200, 25, 25) == kRedPixel);
    CHECK(pixel(pixels, 200, 75, 75) == kRedPixel);
}

TEST_CASE("a box that blends multiplies what lies beneath it, up to the group of the box around it") {
    Stage s;
    PrLayoutCompositingStyle multiply = prLayoutCompositingStyleDefault();
    multiply.blendMode = PrDrawBlendMode_Multiply;
    s.add(s.stage, sized(200, 50), PrColor{1, 1, 0, 1});
    PrLayoutBoxStyle over = sized(100, 50);
    over.margin[PrRectSide_Top] = prLayoutLengthPx(-50);
    s.add(s.stage, over, PrColor{0, 1, 1, 1}, &multiply);
    // Inside a box with opacity, which isolates it, nothing lies beneath it in its group.
    PrLayoutEffectStyle faded = prLayoutEffectStyleDefault();
    faded.opacity = 0.999f;
    PrLayoutNode group = s.add(s.stage, sized(200, 50), PrColor{0, 0, 0, 0}, nullptr, &faded);
    s.add(group, sized(100, 50), PrColor{0, 1, 1, 1}, &multiply);
    std::vector<uint8_t> pixels = s.pixels();
    CHECK(pixel(pixels, 200, 50, 25) == kGreenPixel);
    CHECK(pixel(pixels, 200, 150, 25) == "255 255 0 255");
    CHECK(pixel(pixels, 200, 50, 75) == "0 255 255 255");
}

TEST_CASE("every stacking context isolates what blends in it, and takes a layer only while something does") {
    Stage s;
    // Yellow under a positioned box at z-index 0, a stacking context with no effect of its own.
    s.add(s.stage, absolute(0, 0, 200, 50), PrColor{1, 1, 0, 1});
    PrLayoutBoxStyle context = absolute(0, 0, 200, 50);
    context.zIndexed = 1;
    PrLayoutNode holder = s.add(s.stage, context, PrColor{0, 0, 0, 0});
    PrLayoutCompositingStyle multiply = prLayoutCompositingStyleDefault();
    multiply.blendMode = PrDrawBlendMode_Multiply;
    PrLayoutNode child = s.add(holder, sized(100, 50), PrColor{0, 1, 1, 1}, &multiply);
    std::vector<uint8_t> pixels = s.pixels();
    // Cyan multiplies nothing in the context's group, so it stays cyan over the yellow.
    CHECK(pixel(pixels, 200, 50, 25) == "0 255 255 255");
    const PrLayoutScene* scene = prLayoutTreeScene(s.f.tree);
    CHECK(prLayoutSceneHasLayer(scene, holder));
    prLayoutSceneRelease(scene);
    // Once nothing blends, the context paints in the recording around it.
    PrLayoutStyle record = prLayoutStyleDefault();
    PrLayoutBoxStyle box = sized(100, 50);
    record.groups[PrLayoutGroup_Box] = prLayoutBoxStyleIntern(s.f.tree, &box);
    PrLayoutDecorationStyle decoration = background(PrColor{0, 1, 1, 1});
    record.groups[PrLayoutGroup_Decoration] = prLayoutDecorationStyleIntern(s.f.tree, &decoration);
    prLayoutNodeSetStyle(s.f.tree, child, prLayoutStyleIntern(s.f.tree, &record));
    pixels = s.pixels();
    CHECK(pixel(pixels, 200, 50, 25) == "0 255 255 255");
    scene = prLayoutTreeScene(s.f.tree);
    CHECK_FALSE(prLayoutSceneHasLayer(scene, holder));
    prLayoutSceneRelease(scene);
    CHECK(verified(s.f.tree));
}

TEST_CASE("a compositing group makes a stacking context, and a change to it records no layer again") {
    Stage s;
    PrLayoutCompositingStyle isolated = prLayoutCompositingStyleDefault();
    isolated.isolated = true;
    PrLayoutBoxStyle parent = sized(100, 100);
    PrLayoutNode box = s.add(s.stage, parent, kRed, &isolated);
    PrLayoutBoxStyle under = absolute(0, 0, 50, 50, PrLayoutPosition_Absolute);
    under.zIndexed = 1;
    PrLayoutEffectStyle level = prLayoutEffectStyleDefault();
    level.zIndex = -1;
    s.add(box, under, kBlue, nullptr, &level);
    std::vector<uint8_t> pixels = s.pixels();
    // The child at z-index -1 stacks in the box's context, over its background.
    CHECK(pixel(pixels, 200, 25, 25) == kBluePixel);
    const PrLayoutScene* scene = prLayoutTreeScene(s.f.tree);
    CHECK(prLayoutSceneHasLayer(scene, box));
    prLayoutSceneRelease(scene);
    CHECK(verified(s.f.tree));
}

TEST_CASE("a backdrop root bounds what a backdrop filter in it reads, though it applies nothing itself") {
    for (bool root : {false, true}) {
        Stage s;
        s.add(s.stage, absolute(0, 0, 200, 50), kRed);
        PrLayoutCompositingStyle anticipated = prLayoutCompositingStyleDefault();
        anticipated.backdropRoot = root;
        PrLayoutBoxStyle parent = absolute(0, 0, 100, 50);
        PrLayoutNode holder = s.add(s.stage, parent, PrColor{0, 0, 0, 0}, &anticipated);
        PrLayoutFilter gray{.kind = PrLayoutFilterKind_Grayscale, .amount = 1};
        PrLayoutCompositingStyle c = prLayoutCompositingStyleDefault();
        c.backdrop = prLayoutFilterIntern(s.f.tree, &gray, 1);
        PrLayoutBoxStyle box = absolute(100, 0, 50, 50);
        box.filtered = 1;
        s.add(holder, box, PrColor{0, 0, 0, 0}, &c);
        std::vector<uint8_t> pixels = s.pixels();
        // Within the root, nothing lies beneath the box, so the red beneath it shows unfiltered.
        CHECK(pixel(pixels, 200, 125, 25) == (root ? kRedPixel : "54 54 54 255"));
    }
}

TEST_CASE("a filtered root is no containing block, so its fixed boxes stay the viewport's") {
    Fixture f;
    PrLayoutBoxStyle rootBox = block();
    for (PrLayoutUnit& width : rootBox.border)
        width = prLayoutUnitFromPx(10);
    rootBox.filtered = 1;
    PrLayoutFilter gray{.kind = PrLayoutFilterKind_Grayscale, .amount = 1};
    PrLayoutCompositingStyle c = prLayoutCompositingStyleDefault();
    c.filter = prLayoutFilterIntern(f.tree, &gray, 1);
    PrLayoutStyle record = prLayoutStyleDefault();
    record.groups[PrLayoutGroup_Box] = prLayoutBoxStyleIntern(f.tree, &rootBox);
    record.groups[PrLayoutGroup_Compositing] = prLayoutCompositingStyleIntern(f.tree, &c);
    f.root = prLayoutNodeCreateElement(f.tree, kPrLayoutNodeNone, prLayoutStyleIntern(f.tree, &record));
    PrLayoutNode fixed = f.element(f.root, absolute(0, 0, 20, 20, PrLayoutPosition_Fixed));
    f.layout(px(200, 200));
    // A containing block would place it in its padding box, inside the border.
    CHECK(near(f.bounds(fixed), PrRect{0, 0, 20, 20}));
}

TEST_CASE("background layers that blend do so with the background color, in a group apart from what lies beneath") {
    Stage s;
    PrLayoutBackgroundLayer layer = redLayer(50, 50);
    layer.blendMode = PrDrawBlendMode_Multiply;
    // Over a blue background color, red times blue is black. Over no color, the red blends with the group's transparent
    // start and stays red, though green lies beneath the box.
    addLayered(s, s.stage, sized(100, 50), kBlue, &layer, 1);
    PrLayoutNode under = s.add(s.stage, sized(100, 50), kGreen);
    addLayered(s, under, sized(100, 50), PrColor{0, 0, 0, 0}, &layer, 1);
    std::vector<uint8_t> pixels = s.pixels();
    CHECK(pixel(pixels, 200, 25, 25) == "0 0 0 255");
    CHECK(pixel(pixels, 200, 75, 25) == kBluePixel);
    CHECK(pixel(pixels, 200, 25, 75) == kRedPixel);
    CHECK(pixel(pixels, 200, 75, 75) == kGreenPixel);
}

TEST_CASE("a filtered image is filtered at its concrete object size, within that size") {
    Stage s;
    PrLayoutBackgroundLayer layer = redLayer(50, 50);
    PrLayoutFilter filters[] = {
        {.kind = PrLayoutFilterKind_Grayscale, .amount = 1},
        {.kind = PrLayoutFilterKind_DropShadow, .offset = PrVec2{20, 0}, .color = PR_COLOR16(0, 0, 1, 1)},
    };
    layer.filters = filters;
    layer.filterCount = 2;
    addLayered(s, s.stage, sized(100, 50), kGreen, &layer, 1);
    std::vector<uint8_t> pixels = s.pixels();
    // The gray of red, and no shadow past the image's 50 by 50, which is the filter's region.
    CHECK(pixel(pixels, 200, 25, 25) == "54 54 54 255");
    CHECK(pixel(pixels, 200, 60, 25) == kGreenPixel);
}

TEST_CASE("background layers intern by their blending and their filter lists' items") {
    Stage s;
    PrLayoutBackgroundLayer plain = redLayer(50, 50);
    PrLayoutBackgroundLayer blended = plain;
    blended.blendMode = PrDrawBlendMode_Screen;
    PrLayoutFilter half{.kind = PrLayoutFilterKind_Grayscale, .amount = 0.5f};
    PrLayoutFilter whole{.kind = PrLayoutFilterKind_Grayscale, .amount = 1};
    PrLayoutFilter again = half;
    PrLayoutBackgroundLayer halfGray = plain;
    halfGray.filters = &half;
    halfGray.filterCount = 1;
    PrLayoutBackgroundLayer wholeGray = halfGray;
    wholeGray.filters = &whole;
    PrLayoutBackgroundLayer halfAgain = halfGray;
    halfAgain.filters = &again;
    PrLayoutBackgroundIndex a = prLayoutBackgroundIntern(s.f.tree, &plain, 1);
    CHECK(prLayoutBackgroundIntern(s.f.tree, &blended, 1) != a);
    PrLayoutBackgroundIndex b = prLayoutBackgroundIntern(s.f.tree, &halfGray, 1);
    CHECK(b != a);
    CHECK(prLayoutBackgroundIntern(s.f.tree, &wholeGray, 1) != b);
    CHECK(prLayoutBackgroundIntern(s.f.tree, &halfAgain, 1) == b);
    // The tree keeps its own copy of the items.
    PrLayoutBackgroundLayer kept = prLayoutBackgroundLayer(s.f.tree, b, 0);
    CHECK(kept.filters != &half);
    CHECK(kept.filters[0].amount == 0.5f);
}
