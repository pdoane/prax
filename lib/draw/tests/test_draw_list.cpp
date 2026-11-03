#include "pr/draw_commands.h"
#include "pr/draw_filter.h"
#include "pr/draw_list.h"
#include "pr/float.h"
#include "pr/vec.h"

#include "doctest/doctest.h"

#include <string>

extern "C" int prTestDrawCApi(void);

namespace pr_impl {

namespace {

std::string dump(const PrDrawList* list) {
    PrString text = prDrawListDump(list);
    PrStringView view = prStringView(&text);
    std::string result(view.data != nullptr ? view.data : "", view.size);
    prStringReset(&text);
    return result;
}

PrDrawStroke stroke(float width) {
    PrDrawStroke s = prDrawStrokeDefault();
    s.shape.width = width;
    return s;
}

constexpr PrColor kRed{1, 0, 0, 1};

// Maps the straight color `in` by `matrix` into `out`.
void apply(const PrAffine4& matrix, const float in[4], float out[4]) {
    for (int row = 0; row < 4; ++row) {
        out[row] = matrix.m[row][4];
        for (int column = 0; column < 4; ++column)
            out[row] += matrix.m[row][column] * in[column];
    }
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("the public headers compile as C") {
    CHECK(prTestDrawCApi() == 0);
}

TEST_CASE("shapes fill without a stroke and are stroked with one") {
    PrDrawList* list = prDrawListCreate();
    prDrawListClearRect(list, PrRect{0, 0, 64, 64}, PrColor{1, 1, 1, 1});
    prDrawListRect(list, PrRect{0, 0, 10, 5}, {.color = kRed});
    PrDrawStroke thin = stroke(2);
    prDrawListEllipse(list, PrVec2{5, 5}, PrVec2{3, 2}, 0, {.color = PrColor{0, 0, 1, 0.5f}, .stroke = &thin});
    prDrawListLine(list, PrVec2{0, 0}, PrVec2{4, 4}, {.color = kRed, .stroke = &thin});
    CHECK(dump(list) == "push-clip-rect 0 0 64 64\n"
        "clear 1 1 1 1\n"
        "pop-clip\n"
        "rect 0 0 10 5 color 1 0 0 1\n"
        "ellipse 5 5 3 2 0 stroke 2 4 0 miter butt butt butt color 0 0 1 0.5\n"
        "line 0 0 4 4 stroke 2 4 0 miter butt butt butt color 1 0 0 1\n");
    prDrawListRelease(list);
}

TEST_CASE("a paint's operator, blend mode and edges are recorded into its shape") {
    PrDrawList* list = prDrawListCreate();
    prDrawListRect(list, PrRect{0, 0, 1, 1}, {.color = kRed});
    prDrawListRect(list, PrRect{0, 0, 1, 1}, {
        .color = kRed,
        .composite = PrDrawComposite_Copy,
        .blendMode = PrDrawBlendMode_Multiply,
        .edges = PrDrawEdges_Aliased,
    });
    REQUIRE(prDrawListCommandCount(list) == 2);
    const PrDrawCommand* commands = prDrawListCommands(list);
    CHECK(commands[0].antialias);
    CHECK(commands[0].composite == PrDrawComposite_SourceOver);
    CHECK(commands[0].blendMode == PrDrawBlendMode_Normal);
    CHECK(!commands[1].antialias);
    CHECK(commands[1].composite == PrDrawComposite_Copy);
    CHECK(commands[1].blendMode == PrDrawBlendMode_Multiply);
    CHECK(dump(list) == "rect 0 0 1 1 color 1 0 0 1\nrect 0 0 1 1 color 1 0 0 1 composite 2 blend 1 aliased\n");
    prDrawListRelease(list);
}

TEST_CASE("what cannot paint is not recorded") {
    PrDrawList* list = prDrawListCreate();
    prDrawListRect(list, PrRect{5, 5, 5, 10}, {.color = kRed});
    prDrawListRect(list, PrRect{0, 0, 10, 10}, {.color = PrColor{1, 0, 0, 0}});
    PrVec2 two[] = {{0, 0}, {1, 1}};
    prDrawListPolylineContour(list, two, 2, PrDrawContour_Closed, PrPathFillRule_NonZero, {.color = kRed});
    PrPath* empty = prPathCreate();
    prDrawListPath(list, empty, PrPathFillRule_NonZero, {.color = kRed});
    prPathDestroy(empty);
    CHECK(prDrawListCommandCount(list) == 0);
    PrDrawStroke thin = stroke(1);
    prDrawListRect(list, PrRect{5, 5, 5, 10}, {.color = kRed, .stroke = &thin});
    CHECK(prDrawListCommandCount(list) == 1);
    prDrawListRelease(list);
}

TEST_CASE("strokes, dashes, points and paths are copied as they are when recorded") {
    PrDrawList* list = prDrawListCreate();
    float dashes[] = {3, 1};
    PrDrawStroke dashed = stroke(1);
    dashed.shape.dashes = dashes;
    dashed.shape.dashCount = 2;
    dashed.nonScaling = true;
    PrVec2 points[] = {{0, 0}, {10, 0}, {10, 10}};
    prDrawListPolylineContour(list, points, 3, PrDrawContour_Open, PrPathFillRule_NonZero, {.color = kRed, .stroke = &dashed});
    PrPath* path = prPathCreate();
    prPathMoveTo(path, PrVec2{0, 0});
    prPathQuadTo(path, PrVec2{5, 10}, PrVec2{10, 0});
    prPathClose(path);
    prDrawListPath(list, path, PrPathFillRule_EvenOdd, {.color = kRed});
    dashes[0] = 9;
    points[1] = PrVec2{7, 7};
    prPathClear(path);
    prPathDestroy(path);
    for (int i = 0; i < 300; ++i)
        prDrawListPolylineContour(list, points, 3, PrDrawContour_Closed, PrPathFillRule_NonZero, {.color = kRed});
    std::string text = dump(list);
    CHECK(text.substr(0, text.find('\n', text.find('\n') + 1) + 1) ==
        "polyline open 0 0 10 0 10 10 stroke 1 4 0 miter butt butt butt dashes 3 1 non-scaling color 1 0 0 1\n"
        "path M 0 0 Q 5 10 10 0 Z evenodd color 1 0 0 1\n");
    prDrawListRelease(list);
    prDrawListRelease(nullptr);
}

TEST_CASE("a polyline records every contour, whether each is closed, and a copy of its points") {
    PrDrawList* list = prDrawListCreate();
    PrVec2 points[] = {{0, 0}, {10, 0}, {10, 10}, {20, 0}, {30, 0}};
    uint32_t contours[] = {0, 3};
    uint64_t closed = 1;
    PrPathPolyline polyline{points, contours, &closed, 5, 5, 2, 2};
    prDrawListPolyline(list, &polyline, PrPathFillRule_EvenOdd, {.color = kRed});
    points[0] = PrVec2{5, 5};
    closed = 0;
    // A fill with no contour of three points has no area.
    PrPathPolyline flat{points + 3, contours, &closed, 2, 2, 1, 1};
    prDrawListPolyline(list, &flat, PrPathFillRule_NonZero, {.color = kRed});
    PrDrawStroke thin = stroke(1);
    prDrawListPolyline(list, &flat, PrPathFillRule_NonZero, {.color = kRed, .stroke = &thin});
    CHECK(dump(list) == "polyline closed 0 0 10 0 10 10 open 20 0 30 0 evenodd color 1 0 0 1\n"
        "polyline open 20 0 30 0 stroke 1 4 0 miter butt butt butt color 1 0 0 1\n");
    prDrawListRelease(list);
}

TEST_CASE("pushes and pops nest in one order") {
    PrDrawList* list = prDrawListCreate();
    prDrawListPushTransformAffine2(list, prAffine2Translate(PrVec2{2, 3}));
    prDrawListPushClipRect(list, PrRect{0, 0, 5, 5}, PrDrawEdges_Antialiased);
    CHECK(prDrawListDepth(list) == 2);
    prDrawListPopClip(list);
    PrPath* path = prPathCreate();
    prPathAddRect(path, PrRect{1, 1, 2, 2});
    prDrawListPushClipPath(list, path, PrPathFillRule_EvenOdd, PrDrawEdges_Antialiased);
    prPathDestroy(path);
    prDrawListPopClip(list);
    prDrawListPopTransform(list);
    CHECK(prDrawListDepth(list) == 0);
    CHECK(dump(list) == "push-transform-affine 1 0 0 1 2 3\n"
        "push-clip-rect 0 0 5 5\n"
        "pop-clip\n"
        "push-clip-path M 1 1 L 2 1 L 2 2 L 1 2 Z evenodd\n"
        "pop-clip\n"
        "pop-transform\n");
    prDrawListRelease(list);
}

TEST_CASE("a group in sRGB-linear is recorded as groups in sRGB, which one pop closes") {
    PrDrawList* list = prDrawListCreate();
    PrDrawGroup group = prDrawGroupDefault();
    group.space = PrColorSpace_SrgbLinear;
    group.blendMode = PrDrawBlendMode_Multiply;
    // Without other effects, one group reads what lies beneath and copies its result over it.
    prDrawListPushGroup(list, &group);
    CHECK(prDrawListDepth(list) == 1);
    // With opacity, a group inside holds it.
    PrDrawGroup inner = group;
    inner.opacity = 0.5f;
    inner.blendMode = PrDrawBlendMode_Normal;
    prDrawListPushGroup(list, &inner);
    CHECK(prDrawListDepth(list) == 3);
    prDrawListRect(list, PrRect{0, 0, 1, 1}, {.color = PrColor{1, 1, 1, 1}});
    prDrawListPopGroup(list);
    CHECK(prDrawListDepth(list) == 1);
    prDrawListPopGroup(list);
    CHECK(prDrawListDepth(list) == 0);
    REQUIRE(prDrawListCommandCount(list) == 7);
    const PrDrawCommand* commands = prDrawListCommands(list);
    const PrDrawGroup* outer = commands[0].group;
    CHECK(commands[0].composite == PrDrawComposite_Copy);
    CHECK(outer->space == PrColorSpace_Srgb);
    REQUIRE(outer->filter != nullptr);
    const PrDrawFilterNode& node = prDrawFilterNodes(outer->filter)[0];
    CHECK(node.kind == PrDrawFilterKind_Blend);
    CHECK(node.blend == PrDrawBlendMode_Multiply);
    CHECK(node.space == PrColorSpace_SrgbLinear);
    CHECK(node.inputs[1] == PrDrawFilterInput_Backdrop);
    CHECK(prDrawListReadsBackdrop(list));
    CHECK(commands[2].group->opacity == 0.5f);
    CHECK(commands[2].composite == PrDrawComposite_SourceOver);
    prDrawListRelease(list);
}

TEST_CASE("a brush is copied, and commands in an equal brush share the copy") {
    PrDrawList* list = prDrawListCreate();
    PrDrawColorStop stops[] = {{.offset = 0, .color = kRed}, {.offset = 1, .color = PrColor{0, 0, 1, 1}}};
    PrDrawBrush brush = prDrawBrushLinear(PrVec2{0, 0}, PrVec2{10, 0}, stops, 2);
    prDrawListRect(list, PrRect{0, 0, 1, 1}, {.brush = &brush});
    prDrawListEllipse(list, PrVec2{5, 5}, PrVec2{1, 1}, 0, {.brush = &brush});
    stops[0].offset = 0.5f;
    prDrawListRect(list, PrRect{0, 0, 1, 1}, {.brush = &brush});
    prDrawListRect(list, PrRect{0, 0, 1, 1}, {.color = kRed});
    REQUIRE(prDrawListCommandCount(list) == 4);
    const PrDrawCommand* commands = prDrawListCommands(list);
    CHECK(commands[0].brush == commands[1].brush);
    CHECK(commands[0].brush != commands[2].brush);
    CHECK(commands[0].brush->stops[0].offset == 0.0f);
    CHECK(commands[2].brush->stops[0].offset == 0.5f);
    CHECK(commands[3].brush == nullptr);
    // A command in a brush has the brush's opacity as its color's alpha.
    CHECK(prColorFromColor16(commands[0].color).a == 1.0f);
    brush.opacity = 0.5f;
    prDrawListRect(list, PrRect{0, 0, 1, 1}, {.brush = &brush});
    CHECK(prColorFromColor16(prDrawListCommands(list)[4].color).a == 0.5f);
    // A gradient without stops, or with no opacity, paints nothing.
    brush.opacity = 0;
    prDrawListRect(list, PrRect{0, 0, 1, 1}, {.brush = &brush});
    brush.opacity = 1;
    brush.stopCount = 0;
    prDrawListRect(list, PrRect{0, 0, 1, 1}, {.brush = &brush});
    CHECK(prDrawListCommandCount(list) == 5);
    prDrawListRelease(list);
}

TEST_CASE("a singular transform is recorded as any other, for its backend to compose") {
    PrDrawList* list = prDrawListCreate();
    prDrawListPushTransformAffine2(list, prAffine2Scale(PrVec2{0, 1}));
    prDrawListRect(list, PrRect{0, 0, 10, 10}, {.color = kRed});
    prDrawListPopTransform(list);
    CHECK(dump(list) == "push-transform-affine 0 0 0 1 0 0\n"
        "rect 0 0 10 10 color 1 0 0 1\n"
        "pop-transform\n");
    prDrawListRelease(list);
}

TEST_CASE("new shapes, clips and groups record what they are given") {
    PrDrawList* list = prDrawListCreate();
    PrCornerRect shape{PrRect{0, 0, 10, 10}, {{{4, 4}, 2}, {{0, 0}, 1}, {{2, 3}, -1}, {{1, 1}, 1}}};
    prDrawListCornerRect(list, shape, {.color = kRed});
    prDrawListSector(list, PrVec2{5, 5}, 1, 4, 0, 2, {.color = kRed});
    prDrawListCapsule(list, PrVec2{0, 0}, PrVec2{4, 0}, 2, {.color = kRed});
    prDrawListPushClipCornerRect(list, shape, PrDrawEdges_Antialiased);
    prDrawListPushClipTargetRect(list, PrRect{0, 0, 3, 3}, PrDrawEdges_Antialiased);
    PrDrawGroup group = prDrawGroupDefault();
    group.opacity = 0.5f;
    group.isolated = true;
    group.bounds = PrRect{0, 0, 2, 2};
    prDrawListPushGroup(list, &group);
    prDrawListRect(list, PrRect{0, 0, 1, 1}, {.color = kRed});
    prDrawListPopGroup(list);
    prDrawListPopClip(list);
    prDrawListPopClip(list);
    shape.corners[0].shape = 7;
    CHECK(dump(list) == "corner-rect 0 0 10 10 corners 4 4 2 0 0 1 2 3 -1 1 1 1 color 1 0 0 1\n"
        "sector 5 5 1 4 0 2 color 1 0 0 1\n"
        "capsule 0 0 4 0 2 color 1 0 0 1\n"
        "push-clip-corner-rect 0 0 10 10 corners 4 4 2 0 0 1 2 3 -1 1 1 1\n"
        "push-clip-target-rect 0 0 3 3\n"
        "push-group 0 0 2 2 opacity 0.5 isolated\n"
        "rect 0 0 1 1 color 1 0 0 1\n"
        "pop-group\n"
        "pop-clip\n"
        "pop-clip\n");
    prDrawListRelease(list);
}

TEST_CASE("a group without bounds takes the bounds of its contents, unless it may draw outside them") {
    PrDrawList* list = prDrawListCreate();
    PrDrawGroup group = prDrawGroupDefault();
    group.isolated = true;
    prDrawListPushGroup(list, &group);
    prDrawListPushTransformAffine2(list, prAffine2Translate(PrVec2{10, 20}));
    prDrawListRect(list, PrRect{0, 0, 4, 4}, {.color = kRed});
    prDrawListPopTransform(list);
    prDrawListPopGroup(list);
    // The bounds reach as far as antialiasing does.
    CHECK(dump(list) == "push-group 9 19 15 25 opacity 1 isolated\n"
        "push-transform-affine 1 0 0 1 10 20\n"
        "rect 0 0 4 4 color 1 0 0 1\n"
        "pop-transform\n"
        "pop-group\n");
    prDrawListRelease(list);

    // An operator that changes what lies outside the contents covers the whole clip.
    list = prDrawListCreate();
    group = prDrawGroupDefault();
    group.composite = PrDrawComposite_Copy;
    prDrawListPushGroup(list, &group);
    prDrawListRect(list, PrRect{0, 0, 4, 4}, {.color = kRed});
    prDrawListPopGroup(list);
    CHECK(prDrawListCommands(list)[0].group->bounds.right == 0);
    prDrawListRelease(list);
}

TEST_CASE("a group that draws nothing is removed with its contents") {
    PrDrawList* list = prDrawListCreate();
    prDrawListRect(list, PrRect{0, 0, 1, 1}, {.color = kRed});
    PrDrawGroup transparent = prDrawGroupDefault();
    transparent.opacity = 0;
    prDrawListPushGroup(list, &transparent);
    prDrawListRect(list, PrRect{0, 0, 4, 4}, {.color = kRed});
    prDrawListPopGroup(list);
    // Contents clipped to nothing cover nothing.
    PrDrawGroup isolated = prDrawGroupDefault();
    isolated.isolated = true;
    prDrawListPushGroup(list, &isolated);
    prDrawListPushClipRect(list, PrRect{0, 0, 0, 0}, PrDrawEdges_Snapped);
    prDrawListRect(list, PrRect{0, 0, 4, 4}, {.color = kRed});
    prDrawListPopClip(list);
    prDrawListPopGroup(list);
    CHECK(dump(list) == "rect 0 0 1 1 color 1 0 0 1\n");

    // A transparent child of a knockout group still knocks out what its siblings drew.
    PrDrawGroup knockout = prDrawGroupDefault();
    knockout.knockout = true;
    prDrawListPushGroup(list, &knockout);
    prDrawListPushGroup(list, &transparent);
    prDrawListRect(list, PrRect{0, 0, 4, 4}, {.color = kRed});
    prDrawListPopGroup(list);
    prDrawListPopGroup(list);
    CHECK(prDrawListCommandCount(list) == 6);
    prDrawListRelease(list);
}

TEST_CASE("a list embeds another, which it holds and which stays frozen from then on") {
    PrDrawList* inner = prDrawListCreate();
    prDrawListPushTransformAffine2(inner, prAffine2Translate(PrVec2{10, 0}));
    PrDrawList* outer = prDrawListCreate();
    // A list with open pushes is not embedded.
    prDrawListEmbed(outer, inner, PrDVec2{0, 0});
    CHECK(prDrawListCommandCount(outer) == 0);
    uint64_t open = prDrawListId(inner);
    prDrawListRect(inner, PrRect{0, 0, 5, 5}, {.color = kRed});
    prDrawListPopTransform(inner);
    CHECK(prDrawListId(inner) != open);
    prDrawListEmbed(outer, inner, PrDVec2{0, 0});
    REQUIRE(prDrawListCommandCount(outer) == 1);
    PrRect bounds = prDrawListBounds(inner);
    CHECK(bounds.left <= 10.0f);
    CHECK(bounds.right >= 15.0f);
    CHECK(bounds.right <= 17.0f);
    uint64_t frozen = prDrawListId(inner);
    prDrawListRelease(inner);
    // The outer list's reference keeps the inner list alive.
    const PrDrawList* embedded = prDrawListCommands(outer)[0].embed.list;
    CHECK(prDrawListCommandCount(embedded) == 3);
    CHECK(prDrawListId(embedded) == frozen);
    prDrawListRelease(outer);
}

TEST_CASE("an embed moved by an offset is one command, and its bounds move with it") {
    PrDrawList* inner = prDrawListCreate();
    prDrawListRect(inner, PrRect{0, 0, 4, 4}, {.color = kRed});
    PrDrawList* outer = prDrawListCreate();
    prDrawListEmbed(outer, inner, PrDVec2{100, 50});
    REQUIRE(prDrawListCommandCount(outer) == 1);
    CHECK(prDrawListCommands(outer)[0].embed.offset.x == 100.0);
    PrRect bounds = prDrawListBounds(outer);
    // Bounds are outset for antialiasing.
    CHECK((bounds.left >= 96.0f && bounds.left <= 100.0f));
    CHECK((bounds.top >= 46.0f && bounds.top <= 50.0f));
    CHECK((bounds.right >= 104.0f && bounds.right <= 108.0f));
    prDrawListRelease(inner);
    prDrawListRelease(outer);
}

TEST_CASE("images keep their pixels and convert other spaces to extended sRGB") {
    const uint8_t pixels[8] = {255, 0, 0, 255, 0, 255, 0, 128};
    PrDrawImageDesc desc{PrPixelView{pixels, 8, {2, 1}}, PrPixelFormat_Rgba8, PrDrawAlpha_Straight, PrColorSpace_Srgb};
    const PrDrawImage* image = nullptr;
    REQUIRE(prDrawImageCreate(&desc, &image) == PrStatus_Ok);
    CHECK(prDrawImageFormat(image) == PrPixelFormat_Rgba8);
    CHECK(prDrawImagePixels(image).data[7] == 128);
    const PrDrawImage* p3 = nullptr;
    desc.space = PrColorSpace_DisplayP3;
    REQUIRE(prDrawImageCreate(&desc, &p3) == PrStatus_Ok);
    CHECK(prDrawImageFormat(p3) == PrPixelFormat_RgbaF16);
    CHECK(prDrawImageId(p3) != prDrawImageId(image));
    // Pixels too large to address are refused before anything is read.
    const PrDrawImage* none = nullptr;
    desc.space = PrColorSpace_Srgb;
    desc.pixels.extent.x = 0xffffffffu;
    desc.pixels.extent.y = 0xffffffffu;
    desc.pixels.stride = SIZE_MAX;
    CHECK(prDrawImageCreate(&desc, &none) == PrStatus_Limit);
    CHECK(none == nullptr);

    PrDrawList* list = prDrawListCreate();
    PrDrawSource source = prDrawSourceImage(image, PrRect{0, 0, 2, 1});
    PrDrawBrush brush = prDrawBrushImage(source, PrRect{0, 0, 20, 10}, PrDrawSampling_Linear);
    prDrawListRect(list, PrRect{0, 0, 20, 10}, {.brush = &brush});
    prDrawImageRelease(image);
    // The list holds the brush's image.
    CHECK(prDrawImageExtent(prDrawListCommands(list)[0].brush->image.source.image).x == 2);
    prDrawListRelease(list);
    prDrawImageRelease(p3);
}

TEST_CASE("an oriented copy of an image keeps its format and arranges its pixels") {
    const uint8_t pixels[8] = {255, 0, 0, 255, 0, 255, 0, 128};
    PrDrawImageDesc desc{PrPixelView{pixels, 8, {2, 1}}, PrPixelFormat_Rgba8, PrDrawAlpha_Straight, PrColorSpace_Srgb};
    const PrDrawImage* image = nullptr;
    REQUIRE(prDrawImageCreate(&desc, &image) == PrStatus_Ok);
    const PrDrawImage* turned = prDrawImageCreateOriented(image, PrImageOrientation_Rotate90);
    CHECK(prDrawImageExtent(turned).x == 1);
    CHECK(prDrawImageExtent(turned).y == 2);
    CHECK(prDrawImageFormat(turned) == PrPixelFormat_Rgba8);
    CHECK(prDrawImageAlpha(turned) == PrDrawAlpha_Straight);
    CHECK(prDrawImageId(turned) != prDrawImageId(image));
    // A quarter turn clockwise puts the left pixel on top.
    PrPixelView view = prDrawImagePixels(turned);
    CHECK(view.data[0] == 255);
    CHECK(view.data[view.stride + 1] == 255);
    CHECK(view.data[view.stride + 3] == 128);
    prDrawImageRelease(turned);
    prDrawImageRelease(image);
}

TEST_CASE("a filter builder numbers its nodes in order") {
    PrDrawFilterBuilder* builder = prDrawFilterBuilderCreate();
    PrDrawFilterNode blur{};
    blur.kind = PrDrawFilterKind_Blur;
    blur.inputs[0] = PrDrawFilterInput_SourceAlpha;
    blur.blur.sigma = PrVec2{2, 2};
    CHECK(prDrawFilterBuilderAdd(builder, &blur) == 0);
    PrDrawFilterNode offset{};
    offset.kind = PrDrawFilterKind_Offset;
    offset.inputs[0] = 0;
    offset.offset = PrVec2{3, 4};
    CHECK(prDrawFilterBuilderAdd(builder, &offset) == 1);
    const PrDrawFilter* filter = nullptr;
    filter = prDrawFilterCreate(builder);
    CHECK(prDrawFilterNodeCount(filter) == 2);
    PrRect outset = prDrawFilterOutset(filter);
    CHECK(outset.right >= 6.0f);
    CHECK(outset.left <= -6.0f);
    PrDrawGroup group = prDrawGroupDefault();
    group.filter = filter;
    PrDrawList* list = prDrawListCreate();
    prDrawListPushGroup(list, &group);
    prDrawListPopGroup(list);
    prDrawFilterRelease(filter);
    CHECK(prDrawFilterNodeCount(prDrawListCommands(list)[0].group->filter) == 2);
    prDrawListRelease(list);
    // Octaves beyond those that add anything visible are dropped.
    PrDrawFilterNode noise{};
    noise.kind = PrDrawFilterKind_Turbulence;
    noise.turbulence.baseFrequency = PrVec2{0.1f, 0.1f};
    noise.turbulence.octaves = 1000;
    prDrawFilterBuilderAdd(builder, &noise);
    filter = prDrawFilterCreate(builder);
    CHECK(prDrawFilterNodes(filter)[0].turbulence.octaves == 16);
    prDrawFilterRelease(filter);
    prDrawFilterBuilderDestroy(builder);
}

TEST_CASE("a filter node clamps negative sizes and produces nothing for parameters that are not finite") {
    PrDrawFilterBuilder* builder = prDrawFilterBuilderCreate();
    PrDrawFilterNode blur{};
    blur.kind = PrDrawFilterKind_Blur;
    blur.inputs[0] = PrDrawFilterInput_Source;
    blur.blur.sigma = PrVec2{-1, 2};
    prDrawFilterBuilderAdd(builder, &blur);
    PrDrawFilterNode shadow{};
    shadow.kind = PrDrawFilterKind_DropShadow;
    shadow.inputs[0] = PrDrawFilterInput_Source;
    shadow.region = PrRect{0, 0, 10, 10};
    shadow.shadow.sigma = PrVec2{kInfinityF32, 1};
    prDrawFilterBuilderAdd(builder, &shadow);
    PrDrawFilterNode morphology{};
    morphology.kind = PrDrawFilterKind_Morphology;
    morphology.inputs[0] = PrDrawFilterInput_Source;
    morphology.morphology.radius = PrVec2{1, kNanF32};
    prDrawFilterBuilderAdd(builder, &morphology);
    PrDrawFilterNode offset{};
    offset.kind = PrDrawFilterKind_Offset;
    offset.inputs[0] = PrDrawFilterInput_Source;
    offset.offset = PrVec2{kInfinityF32, 0};
    prDrawFilterBuilderAdd(builder, &offset);
    const PrDrawFilter* filter = prDrawFilterCreate(builder);
    const PrDrawFilterNode* nodes = prDrawFilterNodes(filter);
    CHECK(nodes[0].kind == PrDrawFilterKind_Blur);
    CHECK(nodes[0].blur.sigma.x == 0.0f);
    CHECK(nodes[0].blur.sigma.y == 2.0f);
    for (uint32_t i = 1; i < 4; ++i) {
        CHECK(nodes[i].kind == PrDrawFilterKind_Flood);
        CHECK(prFloat16ToFloat(nodes[i].flood.a) == 0.0f);
    }
    CHECK(nodes[1].region.right == 10.0f);
    PrRect outset = prDrawFilterOutset(filter);
    CHECK(isFiniteF32(outset.left));
    CHECK(isFiniteF32(outset.right));
    prDrawFilterRelease(filter);
    prDrawFilterBuilderDestroy(builder);
}

TEST_CASE("a convolution's default divisor is its kernel's sum, and 1 for a kernel that sums to zero within rounding") {
    PrDrawFilterBuilder* builder = prDrawFilterBuilderCreate();
    const float blur[3] = {0.1f, 0.1f, 0.1f};
    const float edges[9] = {0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, -0.8f};
    PrDrawFilterNode convolve{};
    convolve.kind = PrDrawFilterKind_Convolution;
    convolve.inputs[0] = PrDrawFilterInput_Source;
    convolve.convolution = PrDrawFilterConvolution{.kernel = blur, .extent = PrUVec2{3, 1}};
    prDrawFilterBuilderAdd(builder, &convolve);
    convolve.convolution = PrDrawFilterConvolution{.kernel = edges, .extent = PrUVec2{3, 3}};
    prDrawFilterBuilderAdd(builder, &convolve);
    convolve.convolution.divisor = 2;
    prDrawFilterBuilderAdd(builder, &convolve);
    const PrDrawFilter* filter = prDrawFilterCreate(builder);
    const PrDrawFilterNode* nodes = prDrawFilterNodes(filter);
    CHECK(nodes[0].convolution.divisor == doctest::Approx(0.3f));
    CHECK(nodes[1].convolution.divisor == 1.0f);
    CHECK(nodes[2].convolution.divisor == 2.0f);
    prDrawFilterRelease(filter);
    prDrawFilterBuilderDestroy(builder);
}

TEST_CASE("pixels written cover their image wherever the clips are") {
    PrDrawList* list = prDrawListCreate();
    uint8_t pixels[4 * 4 * 4] = {};
    PrDrawImageDesc desc{
        PrPixelView{pixels, 16, {4, 4}}, PrPixelFormat_Rgba8, PrDrawAlpha_Premultiplied,
        PrColorSpace_Srgb,
    };
    const PrDrawImage* image = nullptr;
    REQUIRE(prDrawImageCreate(&desc, &image) == PrStatus_Ok);
    prDrawListPushTransformAffine2(list, PrAffine2{2, 0, 0, 2, 100, 100});
    prDrawListPushClipRect(list, PrRect{0, 0, 1, 1}, PrDrawEdges_Antialiased);
    prDrawListWritePixels(list, image, PrIVec2{10, 20});
    prDrawListPopClip(list);
    prDrawListPopTransform(list);
    PrRect bounds = prDrawListBounds(list);
    CHECK(bounds.left == 10.0f);
    CHECK(bounds.top == 20.0f);
    CHECK(bounds.right == 14.0f);
    CHECK(bounds.bottom == 24.0f);
    prDrawImageRelease(image);
    prDrawListRelease(list);
}

TEST_CASE("a list that would reference itself does not") {
    PrDrawList* list = prDrawListCreate();
    prDrawListRect(list, PrRect{0, 0, 1, 1}, {.color = PrColor{1, 0, 0, 1}});
    prDrawListEmbed(list, list, PrDVec2{0, 0});
    CHECK(prDrawListCommandCount(list) == 1);
    prDrawListRect(list, PrRect{0, 0, 1, 1}, {.color = PrColor{0, 0, 1, 1}});
    CHECK(prDrawListCommandCount(list) == 2);

    // A source of the list freezes the list. Each call below would draw the list into itself, and records nothing.
    PrDrawSource source = prDrawSourceList(list, PrRect{0, 0, 1, 1});
    PrDrawBrush brush = prDrawBrushImage(source, PrRect{0, 0, 1, 1}, PrDrawSampling_Linear);
    prDrawListRect(list, PrRect{0, 0, 1, 1}, {.brush = &brush});
    PrDrawNineSlice slices{};
    prDrawListNineSlice(list, source, &slices, PrRect{0, 0, 1, 1}, PrDrawSampling_Linear, {.color = PrColor{1, 1, 1, 1}});
    PrDrawBorder border{};
    for (PrDrawBorderSide& side : border.sides)
        side = PrDrawBorderSide{1, PrDrawBorderStyle_Solid, PrColor{1, 1, 1, 1}, &brush, 0};
    border.shape.rect = PrRect{0, 0, 4, 4};
    prDrawListBorder(list, &border, {});
    CHECK(prDrawListCommandCount(list) == 2);
    prDrawListRelease(list);
}

TEST_CASE("color matrices saturate, rotate hues and move luminance to alpha, keeping gray and alpha as they are") {
    const float gray[4] = {0.5f, 0.5f, 0.5f, 0.75f};
    const float red[4] = {1, 0, 0, 1};
    float out[4];
    for (const PrAffine4& matrix : {prDrawColorMatrixSaturate(0.3f), prDrawColorMatrixHueRotate(1)}) {
        apply(matrix, gray, out);
        for (int channel = 0; channel < 3; ++channel)
            CHECK(out[channel] == doctest::Approx(0.5f));
        CHECK(out[3] == doctest::Approx(0.75f));
    }
    apply(prDrawColorMatrixSaturate(0), red, out);
    CHECK(out[0] == doctest::Approx(0.2126f));
    CHECK(out[1] == doctest::Approx(0.2126f));
    apply(prDrawColorMatrixSaturate(1), red, out);
    CHECK(out[0] == doctest::Approx(1));
    CHECK(out[1] == doctest::Approx(0).epsilon(0.0001));
    apply(prDrawColorMatrixHueRotate(0), red, out);
    CHECK(out[0] == doctest::Approx(1));
    CHECK(out[2] == doctest::Approx(0).epsilon(0.0001));
    apply(prDrawColorMatrixLuminanceToAlpha(), red, out);
    CHECK(out[0] == 0);
    CHECK(out[3] == doctest::Approx(0.2126f));
}
