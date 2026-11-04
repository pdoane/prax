#include "pr/draw_cpu.h"
#include "pr/draw_filter.h"
#include "pr/vec.h"

#include "tools/harness/test.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

extern "C" int prTestDrawCpuCApi(void);

namespace pr_impl {

namespace {

// Records a rectangle filled with the source placed over it.
void drawImageRect(PrDrawList* list, const PrDrawSource& source, PrRect destination, PrDrawSampling sampling) {
    PrDrawBrush brush = prDrawBrushImage(source, destination, sampling);
    prDrawListRect(list, destination, {.brush = &brush});
}

struct Target {
    std::vector<uint8_t> bytes;
    PrPixels pixels;
    PrPixelFormat format;
};

Target target(uint32_t width, uint32_t height, PrPixelFormat format = PrPixelFormat_Rgba8) {
    Target t;
    t.bytes.assign(size_t(width) * height * 4, 0);
    t.pixels = PrPixels{t.bytes.data(), size_t(width) * 4, {width, height}};
    t.format = format;
    return t;
}

PrStatus render(Target& t, const PrDrawList* list) {
    PrDrawRenderer* renderer = prDrawCpuCreate(nullptr);
    PrStatus status = prDrawRendererRenderPixels(renderer, list, t.pixels, t.format);
    prDrawRendererDestroy(renderer);
    return status;
}

// Returns one pixel's bytes as "r g b a".
std::string pixel(const Target& t, uint32_t x, uint32_t y) {
    const uint8_t* p = t.bytes.data() + y * t.pixels.stride + x * 4;
    return std::to_string(p[0]) + " " + std::to_string(p[1]) + " " + std::to_string(p[2]) + " " + std::to_string(p[3]);
}

// Returns the alpha of each pixel of a row, as text.
std::string alphaRow(const Target& t, uint32_t y) {
    std::string text;
    for (uint32_t x = 0; x < t.pixels.extent.x; ++x) {
        if (x != 0)
            text += ' ';
        text += std::to_string(t.bytes[y * t.pixels.stride + x * 4 + 3]);
    }
    return text;
}

PrDrawStroke stroke(float width, bool nonScaling = false) {
    PrDrawStroke s = prDrawStrokeDefault();
    s.shape.width = width;
    s.nonScaling = nonScaling;
    return s;
}

constexpr PrColor kBlack{0, 0, 0, 1};
constexpr PrColor kWhite{1, 1, 1, 1};

// Returns a filter of `nodes`, in order, over `region`.
const PrDrawFilter* graphOf(std::initializer_list<PrDrawFilterNode> nodes, PrRect region) {
    PrDrawFilterBuilder* builder = prDrawFilterBuilderCreate();
    prDrawFilterBuilderSetRegion(builder, region);
    for (const PrDrawFilterNode& node : nodes)
        prDrawFilterBuilderAdd(builder, &node);
    const PrDrawFilter* filter = prDrawFilterCreate(builder);
    prDrawFilterBuilderDestroy(builder);
    return filter;
}

// Draws `rect` in `color` through the filter, which the call releases.
void drawFiltered(PrDrawList* list, const PrDrawFilter* filter, PrRect rect, PrColor color) {
    PrDrawGroup group = prDrawGroupDefault();
    group.bounds = prDrawFilterRegion(filter);
    group.filter = filter;
    prDrawListPushGroup(list, &group);
    prDrawListRect(list, rect, {.color = color});
    prDrawListPopGroup(list);
    prDrawFilterRelease(filter);
}

// Creates a list of two halves, red then blue, over (0, 0) to (2, 1).
PrDrawList* halves() {
    PrDrawList* list = prDrawListCreate();
    prDrawListRect(list, PrRect{0, 0, 1, 1}, {.color = PrColor{1, 0, 0, 1}});
    prDrawListRect(list, PrRect{1, 0, 2, 1}, {.color = PrColor{0, 0, 1, 1}});
    return list;
}

// A plane tilted back about its horizontal center line by `radians`, seen from `distance` in front, and placed at
// `center`.
PrMatrix4 tiltedPlane(float radians, float distance, PrVec2 center) {
    PrMatrix4 tilt = prMatrix4Rotate(PrVec3{1, 0, 0}, radians);
    PrMatrix4 eye = prMatrix4Perspective(distance);
    PrMatrix4 place = prMatrix4Translate(PrVec3{center.x, center.y, 0});
    PrMatrix4 seen = prMatrix4Concat(&tilt, &eye);
    return prMatrix4Concat(&seen, &place);
}

// Returns the 4x4 matrix that applies `plane` to the z = 0 plane.
PrMatrix4 liftPlane(const PrMatrix3& plane) {
    PrMatrix4 m{};
    const uint32_t index[3] = {0, 1, 3};
    for (uint32_t row = 0; row < 3; ++row) {
        for (uint32_t column = 0; column < 3; ++column)
            m.m[index[row]][index[column]] = plane.m[row][column];
    }
    m.m[2][2] = 1;
    return m;
}

void pushProjected(PrDrawList* list, const PrMatrix4* projection, PrRect bounds) {
    PrDrawGroup group = prDrawGroupDefault();
    group.bounds = bounds;
    group.projection = projection;
    prDrawListPushGroup(list, &group);
}

// Returns the number of bytes in which two renders differ by more than `tolerance`.
uint32_t differences(const Target& a, const Target& b, uint32_t tolerance) {
    uint32_t count = 0;
    for (size_t i = 0; i < a.bytes.size(); ++i)
        count += a.bytes[i] > b.bytes[i] + tolerance || b.bytes[i] > a.bytes[i] + tolerance;
    return count;
}

uint32_t alphaAt(const Target& t, uint32_t x, uint32_t y) {
    return t.bytes[y * t.pixels.stride + x * 4 + 3];
}

constexpr PrColor kRed{1, 0, 0, 1};
constexpr PrColor kGreen{0, 1, 0, 1};
constexpr PrColor kBlue{0, 0, 1, 1};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("the public header compiles as C") {
    CHECK(prTestDrawCpuCApi() == 0);
}

TEST_CASE("clearing replaces every pixel with the premultiplied tint") {
    Target t = target(2, 2);
    PrDrawList* list = prDrawListCreate();
    prDrawListClearRect(list, PrRect{0, 0, 64, 64}, PrColor{1, 0.5f, 0, 0.5f});
    CHECK(render(t, list) == PrStatus_Ok);
    CHECK(pixel(t, 0, 0) == "128 64 0 128");
    CHECK(pixel(t, 1, 1) == "128 64 0 128");
    prDrawListRelease(list);
}

TEST_CASE("fills blend over what is drawn, by coverage") {
    Target t = target(4, 1);
    PrDrawList* list = prDrawListCreate();
    prDrawListClearRect(list, PrRect{0, 0, 64, 64}, kWhite);
    prDrawListRect(list, PrRect{0, 0, 1.5f, 1}, {.color = kBlack});
    prDrawListRect(list, PrRect{3, 0, 4, 1}, {.color = PrColor{1, 0, 0, 0.5f}});
    render(t, list);
    CHECK(pixel(t, 0, 0) == "0 0 0 255");
    CHECK(pixel(t, 1, 0) == "127 127 127 255");
    CHECK(pixel(t, 2, 0) == "255 255 255 255");
    CHECK(pixel(t, 3, 0) == "255 127 127 255");
    prDrawListRelease(list);
}

TEST_CASE("shapes take the transform in force and the antialiasing they were recorded with") {
    Target t = target(4, 2);
    PrDrawList* list = prDrawListCreate();
    prDrawListPushTransformAffine2(list, prAffine2Translate(PrVec2{2, 0}));
    prDrawListRect(list, PrRect{0, 0, 1.4f, 1}, {.color = PrColor{0, 1, 0, 1}, .edges = PrDrawEdges_Aliased});
    prDrawListPopTransform(list);
    render(t, list);
    CHECK(pixel(t, 1, 0) == "0 0 0 0");
    CHECK(pixel(t, 2, 0) == "0 255 0 255");
    CHECK(pixel(t, 3, 0) == "0 0 0 0");
    CHECK(pixel(t, 2, 1) == "0 0 0 0");
    prDrawListRelease(list);
}

TEST_CASE("transforms nest, the inner one applied first") {
    Target t = target(8, 1);
    PrDrawList* list = prDrawListCreate();
    prDrawListPushTransformAffine2(list, prAffine2Translate(PrVec2{4, 0}));
    prDrawListPushTransformAffine2(list, prAffine2Scale(PrVec2{2, 1}));
    prDrawListRect(list, PrRect{0, 0, 1, 1}, {.color = kBlack});
    prDrawListPopTransform(list);
    prDrawListPopTransform(list);
    render(t, list);
    CHECK(alphaRow(t, 0) == "0 0 0 0 255 255 0 0");
    prDrawListRelease(list);
}

TEST_CASE("paths, rounded rectangles and ellipses fill, clipped to the target") {
    Target t = target(8, 8, PrPixelFormat_Bgra8);
    PrDrawList* list = prDrawListCreate();
    PrPath* path = prPathCreate();
    prPathAddRect(path, PrRect{-4, -4, 2, 2});
    prDrawListPath(list, path, PrPathFillRule_NonZero, {.color = PrColor{1, 0, 0, 1}});
    prPathDestroy(path);
    prDrawListCornerRect(list, PrCornerRect{PrRect{4, 4, 8, 8}, {prCornerRound(PrVec2{4, 4}), {}, {}, {}}},
        {.color = PrColor{0, 0, 1, 1}});
    render(t, list);
    CHECK(pixel(t, 0, 0) == "0 0 255 255");
    CHECK(pixel(t, 2, 2) == "0 0 0 0");
    CHECK(pixel(t, 7, 7) == "255 0 0 255");
    CHECK(pixel(t, 4, 4) == "0 0 0 0");
    prDrawListRelease(list);
}

TEST_CASE("a stroke covers its width along the shape") {
    Target t = target(6, 6);
    PrDrawList* list = prDrawListCreate();
    PrDrawStroke two = stroke(2);
    prDrawListRect(list, PrRect{1, 1, 5, 5}, {.color = kBlack, .stroke = &two});
    render(t, list);
    CHECK(alphaRow(t, 0) == "255 255 255 255 255 255");
    CHECK(alphaRow(t, 1) == "255 255 255 255 255 255");
    CHECK(alphaRow(t, 2) == "255 255 0 0 255 255");
    prDrawListRelease(list);
}

TEST_CASE("a non-scaling stroke of width one is one target pixel whatever the transform") {
    Target t = target(8, 4);
    PrDrawList* list = prDrawListCreate();
    PrDrawStroke hairline = stroke(1, true);
    prDrawListPushTransformAffine2(list, prAffine2Scale(PrVec2{4, 4}));
    prDrawListLine(list, PrVec2{0.25f, 0.375f}, PrVec2{1.75f, 0.375f}, {.color = kBlack, .stroke = &hairline});
    prDrawListPopTransform(list);
    render(t, list);
    CHECK(alphaRow(t, 0) == "0 0 0 0 0 0 0 0");
    CHECK(alphaRow(t, 1) == "0 255 255 255 255 255 255 0");
    CHECK(alphaRow(t, 2) == "0 0 0 0 0 0 0 0");
    prDrawListRelease(list);
}

TEST_CASE("a non-scaling stroke keeps its width in target pixels") {
    Target t = target(8, 8);
    PrDrawList* list = prDrawListCreate();
    PrDrawStroke two = stroke(2, true);
    prDrawListPushTransformAffine2(list, prAffine2Scale(PrVec2{4, 4}));
    prDrawListLine(list, PrVec2{0, 1}, PrVec2{2, 1}, {.color = kBlack, .stroke = &two});
    prDrawListPopTransform(list);
    render(t, list);
    CHECK(alphaRow(t, 2) == "0 0 0 0 0 0 0 0");
    CHECK(alphaRow(t, 3) == "255 255 255 255 255 255 255 255");
    CHECK(alphaRow(t, 4) == "255 255 255 255 255 255 255 255");
    CHECK(alphaRow(t, 5) == "0 0 0 0 0 0 0 0");
    prDrawListRelease(list);
}

TEST_CASE("clips intersect, each lasting until its pop") {
    Target t = target(8, 1);
    PrDrawList* list = prDrawListCreate();
    prDrawListPushClipRect(list, PrRect{1, 0, 6, 1}, PrDrawEdges_Antialiased);
    PrPath* path = prPathCreate();
    prPathAddRect(path, PrRect{3, 0, 8, 1});
    prDrawListPushClipPath(list, path, PrPathFillRule_NonZero, PrDrawEdges_Antialiased);
    prPathDestroy(path);
    prDrawListRect(list, PrRect{0, 0, 8, 1}, {.color = kBlack});
    prDrawListPopClip(list);
    prDrawListRect(list, PrRect{0, 0, 2.5f, 1}, {.color = kBlack});
    prDrawListPopClip(list);
    render(t, list);
    CHECK(alphaRow(t, 0) == "0 255 128 255 255 255 0 0");
    prDrawListRelease(list);
}

TEST_CASE("a clip's edge is antialiased unless it was recorded aliased") {
    Target t = target(4, 1);
    PrDrawList* list = prDrawListCreate();
    prDrawListPushClipRect(list, PrRect{0, 0, 1.5f, 1}, PrDrawEdges_Antialiased);
    prDrawListRect(list, PrRect{0, 0, 4, 1}, {.color = kBlack});
    prDrawListPopClip(list);
    prDrawListPushClipRect(list, PrRect{2, 0, 3.4f, 1}, PrDrawEdges_Aliased);
    prDrawListRect(list, PrRect{0, 0, 4, 1}, {.color = kBlack, .edges = PrDrawEdges_Aliased});
    prDrawListPopClip(list);
    render(t, list);
    CHECK(alphaRow(t, 0) == "255 128 255 0");
    prDrawListRelease(list);
}

TEST_CASE("composite operators combine by their factors") {
    // Each operator in a clip that covers its own pixel, over white.
    Target t = target(4, 1);
    PrDrawList* list = prDrawListCreate();
    prDrawListClearRect(list, PrRect{0, 0, 64, 64}, kWhite);
    auto cell = [&](uint32_t x, PrDrawComposite op, PrColor tint) {
        prDrawListPushClipRect(list, PrRect{float(x), 0, float(x + 1), 1}, PrDrawEdges_Antialiased);
        prDrawListRect(list, PrRect{float(x), 0, float(x + 1), 1}, {.color = tint, .composite = op});
        prDrawListPopClip(list);
    };
    cell(0, PrDrawComposite_Clear, kBlack);
    cell(1, PrDrawComposite_Copy, PrColor{1, 0, 0, 0.5f});
    cell(2, PrDrawComposite_DestinationOut, PrColor{0, 0, 0, 0.5f});
    cell(3, PrDrawComposite_DestinationOver, kBlack);
    render(t, list);
    CHECK(pixel(t, 0, 0) == "0 0 0 0");
    CHECK(pixel(t, 1, 0) == "128 0 0 128");
    CHECK(pixel(t, 2, 0) == "128 128 128 128");
    CHECK(pixel(t, 3, 0) == "255 255 255 255");
    prDrawListRelease(list);
}

TEST_CASE("an operator acts where its shape covers, and a group's reaches the whole clip") {
    Target t = target(4, 1);
    PrDrawList* list = prDrawListCreate();
    prDrawListClearRect(list, PrRect{0, 0, 64, 64}, kWhite);
    prDrawListPushClipRect(list, PrRect{0, 0, 3, 1}, PrDrawEdges_Antialiased);
    prDrawListRect(list, PrRect{1, 0, 2, 1}, {.color = PrColor{1, 0, 0, 1}, .composite = PrDrawComposite_Copy});
    prDrawListPopClip(list);
    render(t, list);
    CHECK(pixel(t, 0, 0) == "255 255 255 255");
    CHECK(pixel(t, 1, 0) == "255 0 0 255");
    CHECK(pixel(t, 2, 0) == "255 255 255 255");
    // The shape in a group whose operator replaces the whole clip, which is how Canvas 2D composites.
    prDrawListClear(list);
    prDrawListClearRect(list, PrRect{0, 0, 64, 64}, kWhite);
    prDrawListPushClipRect(list, PrRect{0, 0, 3, 1}, PrDrawEdges_Antialiased);
    PrDrawGroup copy = prDrawGroupDefault();
    copy.composite = PrDrawComposite_Copy;
    prDrawListPushGroup(list, &copy);
    prDrawListRect(list, PrRect{1, 0, 2, 1}, {.color = PrColor{1, 0, 0, 1}, .composite = PrDrawComposite_Copy});
    prDrawListPopGroup(list);
    prDrawListPopClip(list);
    render(t, list);
    CHECK(pixel(t, 0, 0) == "0 0 0 0");
    CHECK(pixel(t, 1, 0) == "255 0 0 255");
    CHECK(pixel(t, 2, 0) == "0 0 0 0");
    CHECK(pixel(t, 3, 0) == "255 255 255 255");
    prDrawListRelease(list);
}

TEST_CASE("a shape's coverage mixes its operator's result with what lies beneath") {
    // Destination-in with half alpha keeps half of the destination where the shape covers, three quarters where it
    // covers half a pixel, and all of it outside.
    Target t = target(4, 1);
    PrDrawList* list = prDrawListCreate();
    prDrawListClearRect(list, PrRect{0, 0, 64, 64}, kWhite);
    prDrawListRect(list, PrRect{0, 0, 1.5f, 1},
        {.color = PrColor{0, 0, 0, 0.5f}, .composite = PrDrawComposite_DestinationIn});
    render(t, list);
    CHECK(pixel(t, 0, 0) == "128 128 128 128");
    CHECK(pixel(t, 1, 0) == "191 191 191 191");
    CHECK(pixel(t, 2, 0) == "255 255 255 255");
    prDrawListRelease(list);
}

TEST_CASE("clearing replaces the target within the clip only") {
    Target t = target(3, 1);
    PrDrawList* list = prDrawListCreate();
    prDrawListClearRect(list, PrRect{0, 0, 64, 64}, kWhite);
    prDrawListPushClipRect(list, PrRect{1, 0, 2, 1}, PrDrawEdges_Antialiased);
    prDrawListClearRect(list, PrRect{0, 0, 64, 64}, PrColor{0, 0, 1, 1});
    prDrawListPopClip(list);
    render(t, list);
    CHECK(pixel(t, 0, 0) == "255 255 255 255");
    CHECK(pixel(t, 1, 0) == "0 0 255 255");
    CHECK(pixel(t, 2, 0) == "255 255 255 255");
    prDrawListRelease(list);
}

TEST_CASE("blend modes mix the colors before compositing") {
    Target t = target(3, 1);
    PrDrawList* list = prDrawListCreate();
    prDrawListClearRect(list, PrRect{0, 0, 64, 64}, PrColor{0.5f, 0.5f, 0.5f, 1});
    prDrawListRect(list, PrRect{0, 0, 1, 1}, {.color = PrColor{1, 0, 0.5f, 1}, .blendMode = PrDrawBlendMode_Multiply});
    prDrawListRect(list, PrRect{1, 0, 2, 1}, {.color = PrColor{1, 0, 0.5f, 1}, .blendMode = PrDrawBlendMode_Screen});
    prDrawListRect(list, PrRect{2, 0, 3, 1},
        {.color = PrColor{1, 0, 0.5f, 1}, .blendMode = PrDrawBlendMode_Difference});
    render(t, list);
    CHECK(pixel(t, 0, 0) == "128 0 64 255");
    CHECK(pixel(t, 1, 0) == "255 128 192 255");
    CHECK(pixel(t, 2, 0) == "127 128 1 255");
    prDrawListRelease(list);
}

TEST_CASE("a render larger than the limit is refused") {
    PrDrawRenderer* renderer = prDrawCpuCreate(nullptr);
    PrDrawList* list = prDrawListCreate();
    PrDrawTarget huge{};
    huge.extent.x = kPrDrawRenderMaxSize + 1;
    huge.extent.y = 1;
    PrDrawFrame* frame = prDrawFrameCreate(renderer);
    CHECK(prDrawFramePrepare(frame, list, &huge) == PrStatus_Limit);
    CHECK(!frame->prepared);
    prDrawFrameDestroy(frame);
    prDrawListRelease(list);
    prDrawRendererDestroy(renderer);
}

TEST_CASE("a group composites its result by its own operator") {
    Target t = target(3, 1);
    PrDrawList* list = prDrawListCreate();
    prDrawListClearRect(list, PrRect{0, 0, 64, 64}, kWhite);
    PrDrawGroup difference = prDrawGroupDefault();
    difference.blendMode = PrDrawBlendMode_Difference;
    prDrawListPushGroup(list, &difference);
    prDrawListRect(list, PrRect{0, 0, 1, 1}, {.color = kWhite});
    prDrawListPopGroup(list);
    PrDrawGroup group = prDrawGroupDefault();
    group.opacity = 0.5f;
    group.composite = PrDrawComposite_DestinationOut;
    prDrawListPushGroup(list, &group);
    prDrawListRect(list, PrRect{1, 0, 2, 1}, {.color = kBlack});
    prDrawListPopGroup(list);
    render(t, list);
    CHECK(pixel(t, 0, 0) == "0 0 0 255");
    CHECK(pixel(t, 1, 0) == "127 127 127 127");
    CHECK(pixel(t, 2, 0) == "255 255 255 255");
    prDrawListRelease(list);
}

TEST_CASE("a gradient brush paints each pixel at its center's parameter") {
    Target t = target(4, 1);
    PrDrawList* list = prDrawListCreate();
    PrDrawColorStop stops[] = {{.offset = 0, .color = kBlack}, {.offset = 1, .color = kWhite}};
    PrDrawBrush brush = prDrawBrushLinear(PrVec2{0, 0}, PrVec2{4, 0}, stops, 2);
    prDrawListRect(list, PrRect{0, 0, 4, 1}, {.brush = &brush});
    render(t, list);
    CHECK(pixel(t, 0, 0) == "32 32 32 255");
    CHECK(pixel(t, 3, 0) == "223 223 223 255");
    prDrawListRelease(list);
}

TEST_CASE("an image maps its texels onto the destination") {
    Target t = target(4, 2);
    const uint8_t texels[8] = {255, 0, 0, 255, 0, 0, 255, 255};
    PrDrawImageDesc desc{PrPixelView{texels, 8, {2, 1}}, PrPixelFormat_Rgba8, PrDrawAlpha_Straight, PrColorSpace_Srgb};
    const PrDrawImage* image = nullptr;
    REQUIRE(prDrawImageCreate(&desc, &image) == PrStatus_Ok);
    PrDrawList* list = prDrawListCreate();
    PrDrawSource source = prDrawSourceImage(image, PrRect{0, 0, 2, 1});
    drawImageRect(list, source, PrRect{0, 0, 4, 2}, PrDrawSampling_Nearest);
    prDrawImageRelease(image);
    render(t, list);
    CHECK(pixel(t, 1, 1) == "255 0 0 255");
    CHECK(pixel(t, 2, 0) == "0 0 255 255");
    prDrawListRelease(list);
}

TEST_CASE("copies filtered on their own read their own edge texels, and paint nothing between them") {
    // Red and blue texels, magnified so that the filter at each copy's edge reaches past it.
    const uint8_t texels[8] = {255, 0, 0, 255, 0, 0, 255, 255};
    PrDrawImageDesc desc{
        PrPixelView{texels, 8, {2, 1}}, PrPixelFormat_Rgba8, PrDrawAlpha_Premultiplied,
        PrColorSpace_Srgb,
    };
    const PrDrawImage* image = nullptr;
    REQUIRE(prDrawImageCreate(&desc, &image) == PrStatus_Ok);
    auto draw = [&](PrDrawTileFlags flags, float spacing) {
        Target t = target(12, 1);
        PrDrawList* list = prDrawListCreate();
        PrDrawBrush brush = prDrawBrushImage(prDrawSourceImage(image, PrRect{0, 0, 2, 1}), PrRect{0, 0, 4, 1},
            PrDrawSampling_Linear);
        brush.image.tiles.x = PrDrawExtend_Repeat;
        brush.image.tiles.spacing = PrVec2{spacing, 0};
        brush.image.tiles.flags = flags;
        prDrawListRect(list, PrRect{0, 0, 12, 1}, {.brush = &brush});
        render(t, list);
        prDrawListRelease(list);
        return t;
    };
    // The last pixel of the first copy and the first of the next are each copy's own edge texel.
    Target clamped = draw(PrDrawTileFlag_Clamped, 0);
    CHECK(pixel(clamped, 3, 0) == "0 0 255 255");
    CHECK(pixel(clamped, 4, 0) == "255 0 0 255");
    // As one periodic image, the filter reads across the copies' edge.
    Target periodic = draw(0, 0);
    CHECK(pixel(periodic, 3, 0) != "0 0 255 255");
    // Spaced copies keep their edges, and the gap between them is empty.
    Target spaced = draw(PrDrawTileFlag_Clamped, 2);
    CHECK(pixel(spaced, 3, 0) == "0 0 255 255");
    CHECK(pixel(spaced, 4, 0) == "0 0 0 0");
    CHECK(pixel(spaced, 6, 0) == "255 0 0 255");
    prDrawImageRelease(image);
}

TEST_CASE("an embedded list plays as if its commands were recorded in its place, moved by its offset") {
    Target direct = target(6, 3);
    Target played = target(6, 3);
    PrDrawList* list = prDrawListCreate();
    prDrawListEllipse(list, PrVec2{2, 1.5f}, PrVec2{1.7f, 1.2f}, 0, {.color = kBlack});
    prDrawListPushTransformAffine2(list, prAffine2Translate(PrVec2{3, 0}));
    prDrawListEllipse(list, PrVec2{2, 1.5f}, PrVec2{1.7f, 1.2f}, 0, {.color = kBlack});
    prDrawListPopTransform(list);
    render(direct, list);
    prDrawListClear(list);
    PrDrawList* inner = prDrawListCreate();
    prDrawListEllipse(inner, PrVec2{2, 1.5f}, PrVec2{1.7f, 1.2f}, 0, {.color = kBlack});
    prDrawListEmbed(list, inner, PrDVec2{0, 0});
    prDrawListPushTransformAffine2(list, prAffine2Translate(PrVec2{3, 0}));
    prDrawListEmbed(list, inner, PrDVec2{0, 0});
    prDrawListPopTransform(list);
    render(played, list);
    CHECK(direct.bytes == played.bytes);
    Target moved = target(6, 3);
    prDrawListClear(list);
    prDrawListEmbed(list, inner, PrDVec2{0, 0});
    prDrawListEmbed(list, inner, PrDVec2{3, 0});
    prDrawListRelease(inner);
    render(moved, list);
    CHECK(direct.bytes == moved.bytes);
    prDrawListRelease(list);
}

TEST_CASE("a group's opacity applies to its result, not to each of its children") {
    Target t = target(3, 1);
    PrDrawList* list = prDrawListCreate();
    PrDrawGroup group = prDrawGroupDefault();
    group.opacity = 0.5f;
    prDrawListPushGroup(list, &group);
    prDrawListRect(list, PrRect{0, 0, 2, 1}, {.color = kBlack});
    prDrawListRect(list, PrRect{1, 0, 3, 1}, {.color = kBlack});
    prDrawListPopGroup(list);
    render(t, list);
    CHECK(alphaRow(t, 0) == "128 128 128");
    prDrawListRelease(list);
}

TEST_CASE("a group in sRGB-linear composites onto what lies beneath in linear light") {
    Target t = target(3, 1);
    PrDrawList* list = prDrawListCreate();
    prDrawListRect(list, PrRect{0, 0, 3, 1}, {.color = kBlack});
    PrDrawGroup group = prDrawGroupDefault();
    group.space = PrColorSpace_SrgbLinear;
    prDrawListPushGroup(list, &group);
    prDrawListRect(list, PrRect{0, 0, 1, 1}, {.color = PrColor{1, 1, 1, 0.5f}});
    prDrawListPopGroup(list);
    // With opacity, the group's result is halved before it composites.
    group.opacity = 0.5f;
    prDrawListPushGroup(list, &group);
    prDrawListRect(list, PrRect{1, 0, 2, 1}, {.color = kWhite});
    prDrawListPopGroup(list);
    REQUIRE(prDrawListDepth(list) == 0);
    render(t, list);
    // Half of white over black is 0.5 in linear light, which is 188 in sRGB.
    CHECK(pixel(t, 0, 0) == "188 188 188 255");
    CHECK(pixel(t, 1, 0) == "188 188 188 255");
    // What lies beneath and outside the group's contents is unchanged.
    CHECK(pixel(t, 2, 0) == "0 0 0 255");
    prDrawListRelease(list);
}

TEST_CASE("knockout lets each child replace those before it") {
    Target t = target(3, 1);
    PrDrawList* list = prDrawListCreate();
    PrDrawGroup group = prDrawGroupDefault();
    group.knockout = true;
    prDrawListPushGroup(list, &group);
    prDrawListRect(list, PrRect{0, 0, 2, 1}, {.color = PrColor{1, 0, 0, 1}});
    prDrawListRect(list, PrRect{1, 0, 3, 1}, {.color = PrColor{0, 0, 1, 0.5f}});
    prDrawListPopGroup(list);
    render(t, list);
    CHECK(pixel(t, 0, 0) == "255 0 0 255");
    CHECK(pixel(t, 1, 0) == "0 0 128 128");
    prDrawListRelease(list);
}

TEST_CASE("a mask multiplies the group's result by its value") {
    Target t = target(2, 1);
    PrDrawList* list = prDrawListCreate();
    PrDrawColorStop stops[] = {
        {.offset = 0, .color = kWhite},
        {.offset = 0.5f, .color = kWhite},
        {.offset = 0.5f, .color = PrColor{1, 1, 1, 0}},
        {.offset = 1, .color = PrColor{1, 1, 1, 0}},
    };
    PrDrawBrush brush = prDrawBrushLinear(PrVec2{0, 0}, PrVec2{2, 0}, stops, 4);
    PrDrawMask mask = prDrawMaskDefault(&brush);
    PrDrawGroup group = prDrawGroupDefault();
    group.mask = &mask;
    prDrawListPushGroup(list, &group);
    prDrawListRect(list, PrRect{0, 0, 2, 1}, {.color = kBlack});
    prDrawListPopGroup(list);
    render(t, list);
    CHECK(alphaRow(t, 0) == "255 0");
    prDrawListRelease(list);
}

TEST_CASE("clips of shaped corners and device rects") {
    Target t = target(10, 10);
    PrDrawList* list = prDrawListCreate();
    prDrawListPushClipCornerRect(list, prCornerRectRound(PrRect{0, 0, 10, 10}, 5), PrDrawEdges_Antialiased);
    prDrawListPushTransformAffine2(list, prAffine2Scale(PrVec2{3, 3}));
    prDrawListPushClipTargetRect(list, PrRect{0, 0, 10, 5}, PrDrawEdges_Antialiased);
    prDrawListRect(list, PrRect{0, 0, 10, 10}, {.color = kBlack});
    prDrawListPopClip(list);
    prDrawListPopTransform(list);
    prDrawListPopClip(list);
    render(t, list);
    CHECK(pixel(t, 0, 0) == "0 0 0 0");
    CHECK(pixel(t, 5, 2) == "0 0 0 255");
    CHECK(pixel(t, 5, 6) == "0 0 0 0");
    prDrawListRelease(list);
}

TEST_CASE("a stroke aligned inside covers only the shape's interior") {
    Target t = target(10, 1);
    PrDrawList* list = prDrawListCreate();
    PrDrawStroke inside = stroke(2);
    inside.align = PrDrawStrokeAlign_Inside;
    prDrawListRect(list, PrRect{2, -5, 8, 6}, {.color = kBlack, .stroke = &inside});
    render(t, list);
    CHECK(alphaRow(t, 0) == "0 0 255 255 0 0 255 255 0 0");
    prDrawListRelease(list);
}

TEST_CASE("a drop shadow falls outside its caster only") {
    Target t = target(30, 10);
    PrDrawList* list = prDrawListCreate();
    PrDrawShadow shadow{};
    shadow.shape.rect = PrRect{5, 0, 10, 10};
    shadow.offset = PrVec2{4, 0};
    shadow.sigma = 0;
    prDrawListDropShadow(list, &shadow, {.color = kBlack});
    render(t, list);
    CHECK(alphaRow(t, 5) == "0 0 0 0 0 0 0 0 0 0 255 255 255 255 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0");
    prDrawListRelease(list);
}

TEST_CASE("a filter moves a group's contents") {
    Target t = target(6, 1);
    PrDrawList* list = prDrawListCreate();
    PrDrawFilterBuilder* builder = prDrawFilterBuilderCreate();
    PrDrawFilterNode offset{};
    offset.kind = PrDrawFilterKind_Offset;
    offset.inputs[0] = PrDrawFilterInput_Source;
    offset.offset = PrVec2{3, 0};
    prDrawFilterBuilderAdd(builder, &offset);
    const PrDrawFilter* filter = nullptr;
    filter = prDrawFilterCreate(builder);
    prDrawFilterBuilderDestroy(builder);
    PrDrawGroup group = prDrawGroupDefault();
    group.filter = filter;
    prDrawListPushGroup(list, &group);
    prDrawListRect(list, PrRect{0, 0, 2, 1}, {.color = kBlack});
    prDrawListPopGroup(list);
    prDrawFilterRelease(filter);
    render(t, list);
    CHECK(alphaRow(t, 0) == "0 0 0 255 255 0");
    prDrawListRelease(list);
}

TEST_CASE("written pixels ignore transforms, clips and compositing") {
    Target t = target(3, 1);
    const uint8_t texels[4] = {10, 20, 30, 40};
    PrDrawImageDesc desc{
        PrPixelView{texels, 4, {1, 1}}, PrPixelFormat_Rgba8, PrDrawAlpha_Premultiplied,
        PrColorSpace_Srgb,
    };
    const PrDrawImage* image = nullptr;
    REQUIRE(prDrawImageCreate(&desc, &image) == PrStatus_Ok);
    PrDrawList* list = prDrawListCreate();
    prDrawListClearRect(list, PrRect{0, 0, 64, 64}, kWhite);
    prDrawListPushTransformAffine2(list, prAffine2Scale(PrVec2{5, 5}));
    prDrawListPushClipRect(list, PrRect{0, 0, 0.1f, 0.1f}, PrDrawEdges_Antialiased);
    prDrawListWritePixels(list, image, PrIVec2{1, 0});
    prDrawListPopClip(list);
    prDrawListPopTransform(list);
    prDrawImageRelease(image);
    render(t, list);
    CHECK(pixel(t, 1, 0) == "10 20 30 40");
    CHECK(pixel(t, 2, 0) == "255 255 255 255");
    prDrawListRelease(list);
}

TEST_CASE("nothing draws under a transform that collapses as it composes, until its pop") {
    Target t = target(4, 1);
    PrDrawList* list = prDrawListCreate();
    prDrawListPushTransformAffine2(list, prAffine2Scale(PrVec2{1e-30f, 1e-30f}));
    prDrawListPushTransformAffine2(list, prAffine2Scale(PrVec2{1e-30f, 1e-30f}));
    prDrawListClearRect(list, PrRect{0, 0, 64, 64}, kBlack);
    PrDrawGroup group = prDrawGroupDefault();
    group.opacity = 0.5f;
    prDrawListPushGroup(list, &group);
    prDrawListPushClipRect(list, PrRect{0, 0, 1, 1}, PrDrawEdges_Antialiased);
    prDrawListRect(list, PrRect{0, 0, 1e30f, 1e30f}, {.color = kBlack});
    prDrawListPopClip(list);
    prDrawListPopGroup(list);
    prDrawListPopTransform(list);
    prDrawListPopTransform(list);
    prDrawListRect(list, PrRect{3, 0, 4, 1}, {.color = kBlack});
    CHECK(render(t, list) == PrStatus_Ok);
    CHECK(alphaRow(t, 0) == "0 0 0 255");
    prDrawListRelease(list);
}

TEST_CASE("a list brush mapped along a stroke renders at the stroke's size") {
    // A list of a red square beside a blue square, spanning the stroke's unit square. The left half of the stroke's
    // length is red, and the right half is blue.
    PrDrawList* drawn = prDrawListCreate();
    prDrawListRect(drawn, PrRect{0, 0, 1, 1}, {.color = PrColor{1, 0, 0, 1}});
    prDrawListRect(drawn, PrRect{1, 0, 2, 1}, {.color = PrColor{0, 0, 1, 1}});
    PrDrawList* list = prDrawListCreate();
    PrDrawBrush brush = prDrawBrushDefault(PrDrawBrushKind_Image);
    brush.image.source = prDrawSourceList(drawn, PrRect{0, 0, 2, 1});
    brush.image.tiles.tile = PrRect{0, 0, 1, 1};
    brush.image.sampling = PrDrawSampling_Linear;
    PrDrawStroke s = stroke(10);
    s.mapping = PrDrawStrokeMapping_Along;
    prDrawListLine(list, PrVec2{0, 10}, PrVec2{100, 10}, {.brush = &brush, .stroke = &s});
    Target t = target(100, 20);
    CHECK(render(t, list) == PrStatus_Ok);
    CHECK(pixel(t, 20, 10) == "255 0 0 255");
    CHECK(pixel(t, 80, 10) == "0 0 255 255");
    prDrawListRelease(drawn);
    prDrawListRelease(list);
}

TEST_CASE("a backdrop filter under a rotation computes in the group's space") {
    // A band across the top, beneath a group rotated a quarter turn. The offset of the group's backdrop filter along
    // the group's x axis moves the band down on the device.
    Target t = target(20, 20);
    PrDrawList* list = prDrawListCreate();
    prDrawListRect(list, PrRect{0, 0, 20, 4}, {.color = kBlack});
    PrDrawFilterBuilder* builder = prDrawFilterBuilderCreate();
    PrDrawFilterNode offset{};
    offset.kind = PrDrawFilterKind_Offset;
    offset.inputs[0] = PrDrawFilterInput_Backdrop;
    offset.offset = PrVec2{6, 0};
    prDrawFilterBuilderAdd(builder, &offset);
    const PrDrawFilter* filter = nullptr;
    filter = prDrawFilterCreate(builder);
    prDrawFilterBuilderDestroy(builder);
    PrAffine2 turn = prAffine2Concat(prAffine2Concat(prAffine2Translate(PrVec2{-10, -10}), prAffine2Rotate(3.14159265f / 2)),
        prAffine2Translate(PrVec2{10, 10}));
    prDrawListPushTransformAffine2(list, turn);
    PrDrawGroup group = prDrawGroupDefault();
    group.bounds = PrRect{0, 0, 20, 20};
    group.backdrop = filter;
    prDrawListPushGroup(list, &group);
    prDrawListPopGroup(list);
    prDrawListPopTransform(list);
    prDrawFilterRelease(filter);
    CHECK(render(t, list) == PrStatus_Ok);
    CHECK(t.bytes[(8 * 20 + 10) * 4 + 3] == 255);
    CHECK(t.bytes[(5 * 20 + 10) * 4 + 3] == 0);
    CHECK(t.bytes[(2 * 20 + 10) * 4 + 3] == 255);
    prDrawListRelease(list);
}

TEST_CASE("hard mix sets each channel whole where the two sum to one or more") {
    Target t = target(2, 1);
    PrDrawList* list = prDrawListCreate();
    prDrawListRect(list, PrRect{0, 0, 2, 1}, {.color = PrColor{0.6f, 0.6f, 0.2f, 1}});
    prDrawListRect(list, PrRect{0, 0, 1, 1},
        {.color = PrColor{0.5f, 0.3f, 0.9f, 1}, .blendMode = PrDrawBlendMode_HardMix});
    prDrawListRect(list, PrRect{1, 0, 2, 1},
        {.color = PrColor{0.39f, 0.41f, 0.7f, 1}, .blendMode = PrDrawBlendMode_HardMix});
    CHECK(render(t, list) == PrStatus_Ok);
    CHECK(pixel(t, 0, 0) == "255 0 255 255");
    CHECK(pixel(t, 1, 0) == "0 255 0 255");
    prDrawListRelease(list);
}

TEST_CASE("an image brush and a nine-slice draw an area of a list as they draw an image") {
    PrDrawList* drawn = halves();
    Target t = target(100, 40);
    PrDrawList* list = prDrawListCreate();
    PrDrawSource source = prDrawSourceList(drawn, PrRect{0, 0, 2, 1});
    drawImageRect(list, source, PrRect{0, 0, 100, 20}, PrDrawSampling_Linear);
    // A frame: the left slice is red, the right slice is blue, and the middle is stretched between them.
    PrDrawNineSlice slices{};
    for (uint32_t i = 0; i < 4; ++i)
        slices.slices[i] = slices.widths[i] = 0;
    slices.slices[1] = slices.slices[3] = 0.5f;
    slices.widths[1] = slices.widths[3] = 10;
    slices.center = true;
    prDrawListNineSlice(list, source, &slices, PrRect{0, 20, 100, 40}, PrDrawSampling_Linear, {.color = kWhite});
    CHECK(render(t, list) == PrStatus_Ok);
    CHECK(pixel(t, 25, 10) == "255 0 0 255");
    CHECK(pixel(t, 75, 10) == "0 0 255 255");
    CHECK(pixel(t, 3, 30) == "255 0 0 255");
    CHECK(pixel(t, 97, 30) == "0 0 255 255");
    prDrawListRelease(list);
    prDrawListRelease(drawn);
}

TEST_CASE("a tile filter repeats a part of its input across its region") {
    Target t = target(60, 20);
    PrDrawList* list = prDrawListCreate();
    PrDrawFilterBuilder* builder = prDrawFilterBuilderCreate();
    PrDrawFilterNode tile{};
    tile.kind = PrDrawFilterKind_Tile;
    tile.inputs[0] = PrDrawFilterInput_Source;
    tile.tile = PrRect{0, 0, 20, 20};
    prDrawFilterBuilderAdd(builder, &tile);
    const PrDrawFilter* filter = nullptr;
    filter = prDrawFilterCreate(builder);
    prDrawFilterBuilderDestroy(builder);
    PrDrawGroup group = prDrawGroupDefault();
    group.bounds = PrRect{0, 0, 60, 20};
    group.filter = filter;
    prDrawListPushGroup(list, &group);
    prDrawListRect(list, PrRect{0, 0, 10, 10}, {.color = kBlack});
    prDrawListPopGroup(list);
    prDrawFilterRelease(filter);
    CHECK(render(t, list) == PrStatus_Ok);
    CHECK(t.bytes[(5 * 60 + 45) * 4 + 3] == 255);
    CHECK(t.bytes[(5 * 60 + 55) * 4 + 3] == 0);
    CHECK(t.bytes[(15 * 60 + 45) * 4 + 3] == 0);
    prDrawListRelease(list);
}

TEST_CASE("a blur reads past the region of its input by its edge mode") {
    // The left half of the row, copied by a node whose region is that half, then blurred across the whole row.
    PrDrawFilterNode half{};
    half.kind = PrDrawFilterKind_Offset;
    half.inputs[0] = PrDrawFilterInput_Source;
    half.region = PrRect{0, 0, 8, 1};
    PrDrawFilterNode blur{};
    blur.kind = PrDrawFilterKind_Blur;
    blur.inputs[0] = 0;
    blur.blur.sigma = PrVec2{1, 0};
    auto alphaAt = [&](PrImageEdgeMode edge, uint32_t x) {
        blur.blur.edge = edge;
        Target t = target(16, 1);
        PrDrawList* list = prDrawListCreate();
        drawFiltered(list, graphOf({half, blur}, PrRect{0, 0, 16, 1}), PrRect{0, 0, 16, 1}, kBlack);
        render(t, list);
        prDrawListRelease(list);
        return int(t.bytes[x * 4 + 3]);
    };
    // Without an edge mode, the half's edge fades, and nothing reaches past it from the source beyond.
    CHECK(alphaAt(PrImageEdgeMode_None, 7) < 255);
    CHECK(alphaAt(PrImageEdgeMode_None, 12) == 0);
    // Duplicate, wrap and mirror read the half's opaque pixels past its edge, across the whole row.
    for (PrImageEdgeMode edge : {PrImageEdgeMode_Duplicate, PrImageEdgeMode_Wrap, PrImageEdgeMode_Mirror}) {
        CHECK(alphaAt(edge, 7) == 255);
        CHECK(alphaAt(edge, 12) == 255);
    }
}

TEST_CASE("a displacement map reads its map in the node's space and moves its input unconverted") {
    // A map of half gray at half opacity displaces nothing in sRGB. In linear light its red is about 0.21, which moves
    // the input by 10 * (0.21 - 0.5), about 3 pixels to the right. Its alpha, which displaces nothing vertically, is
    // the same in both spaces.
    PrDrawFilterNode map{};
    map.kind = PrDrawFilterKind_Flood;
    map.flood = prColor16FromColor(PrColor{0.5f, 0.5f, 0.5f, 0.5f});
    PrDrawFilterNode displace{};
    displace.kind = PrDrawFilterKind_DisplacementMap;
    displace.inputs[0] = PrDrawFilterInput_Source;
    displace.inputs[1] = 0;
    displace.displacement = PrDrawFilterDisplacement{.scale = 10, .x = PrDrawChannel_R, .y = PrDrawChannel_A};
    for (PrColorSpace space : {PrColorSpace_Srgb, PrColorSpace_SrgbLinear}) {
        displace.space = space;
        map.space = space;
        Target t = target(12, 1);
        PrDrawList* list = prDrawListCreate();
        drawFiltered(list, graphOf({map, displace}, PrRect{0, 0, 12, 1}), PrRect{0, 0, 4, 1}, PrColor{0.5f, 0.5f, 0.5f, 1});
        render(t, list);
        prDrawListRelease(list);
        bool linear = space == PrColorSpace_SrgbLinear;
        CHECK(alphaRow(t, 0) == (linear ? "0 0 0 255 255 255 255 0 0 0 0 0" : "255 255 255 255 0 0 0 0 0 0 0 0"));
        CHECK(pixel(t, linear ? 4 : 1, 0) == "128 128 128 255");
    }
}

TEST_CASE("stitched turbulence tiles the node's region, whatever the graph's region") {
    PrDrawFilterNode noise{};
    noise.kind = PrDrawFilterKind_Turbulence;
    noise.region = PrRect{0, 0, 16, 16};
    noise.turbulence = PrDrawFilterTurbulence{
        .baseFrequency = PrVec2{0.1f, 0.1f},
        .octaves = 2,
        .seed = 3,
        .kind = PrDrawTurbulenceKind_FractalNoise,
        .stitch = true,
    };
    auto noiseIn = [&](PrRect graph) {
        Target t = target(16, 16);
        PrDrawList* list = prDrawListCreate();
        drawFiltered(list, graphOf({noise}, graph), PrRect{0, 0, 1, 1}, kBlack);
        render(t, list);
        prDrawListRelease(list);
        return t;
    };
    Target whole = noiseIn(PrRect{0, 0, 16, 16});
    Target part = noiseIn(PrRect{0, 0, 8, 8});
    for (uint32_t y = 0; y < 8; ++y) {
        for (uint32_t x = 0; x < 8; ++x)
            CHECK(pixel(part, x, y) == pixel(whole, x, y));
    }
}

TEST_CASE("stitched turbulence joins its tile's last column to its first, and its last row to its first") {
    PrDrawFilterNode noise{};
    noise.kind = PrDrawFilterKind_Turbulence;
    noise.region = PrRect{0, 0, 32, 32};
    noise.turbulence = PrDrawFilterTurbulence{
        .baseFrequency = PrVec2{0.1f, 0.1f},
        .octaves = 2,
        .seed = 3,
        .kind = PrDrawTurbulenceKind_FractalNoise,
    };
    // Returns the mean difference in alpha across the seam where a copy of the tile would follow it, and the mean
    // difference between neighbors within the tile, along x.
    auto steps = [&](bool stitch, bool across) {
        noise.turbulence.stitch = stitch;
        Target t = target(32, 32);
        PrDrawList* list = prDrawListCreate();
        drawFiltered(list, graphOf({noise}, PrRect{0, 0, 32, 32}), PrRect{0, 0, 1, 1}, kBlack);
        render(t, list);
        prDrawListRelease(list);
        auto alpha = [&](uint32_t x, uint32_t y) {
            return int32_t(across ? t.bytes[(size_t(y) * 32 + x) * 4 + 3] : t.bytes[(size_t(x) * 32 + y) * 4 + 3]);
        };
        double seam = 0;
        double within = 0;
        for (uint32_t y = 0; y < 32; ++y) {
            seam += std::abs(alpha(31, y) - alpha(0, y));
            for (uint32_t x = 0; x + 1 < 32; ++x)
                within += std::abs(alpha(x + 1, y) - alpha(x, y));
        }
        return std::pair<double, double>{seam / 32, within / (32 * 31)};
    };
    for (bool across : {true, false}) {
        std::pair<double, double> stitched = steps(true, across);
        CHECK(stitched.first <= 2 * stitched.second + 1);
        // Without stitching, the seam is a jump, which shows that the measure tells them apart.
        std::pair<double, double> unstitched = steps(false, across);
        CHECK(unstitched.first > 2 * unstitched.second + 1);
    }
}

TEST_CASE("a drop shadow computed in linear light keeps its color") {
    PrDrawFilterNode shadow{};
    shadow.kind = PrDrawFilterKind_DropShadow;
    shadow.space = PrColorSpace_SrgbLinear;
    shadow.inputs[0] = PrDrawFilterInput_Source;
    shadow.shadow = PrDrawFilterShadow{.offset = PrVec2{3, 0}, .color = prColor16FromColor(PrColor{0.5f, 0, 1, 1})};
    Target t = target(8, 1);
    PrDrawList* list = prDrawListCreate();
    drawFiltered(list, graphOf({shadow}, PrRect{0, 0, 8, 1}), PrRect{0, 0, 2, 1}, kBlack);
    render(t, list);
    prDrawListRelease(list);
    CHECK(pixel(t, 4, 0) == "128 0 255 255");
}

TEST_CASE("a filter's image node draws an area of a list") {
    PrDrawList* drawn = halves();
    Target t = target(40, 20);
    PrDrawList* list = prDrawListCreate();
    PrDrawFilterBuilder* builder = prDrawFilterBuilderCreate();
    PrDrawFilterNode image{};
    image.kind = PrDrawFilterKind_Image;
    image.image.source = prDrawSourceList(drawn, PrRect{1, 0, 2, 1});
    image.image.rect = PrRect{10, 0, 30, 20};
    image.image.sampling = PrDrawSampling_Linear;
    prDrawFilterBuilderAdd(builder, &image);
    const PrDrawFilter* filter = nullptr;
    filter = prDrawFilterCreate(builder);
    prDrawFilterBuilderDestroy(builder);
    prDrawListRelease(drawn);
    PrDrawGroup group = prDrawGroupDefault();
    group.bounds = PrRect{0, 0, 40, 20};
    group.filter = filter;
    prDrawListPushGroup(list, &group);
    prDrawListPopGroup(list);
    prDrawFilterRelease(filter);
    CHECK(render(t, list) == PrStatus_Ok);
    CHECK(pixel(t, 20, 10) == "0 0 255 255");
    CHECK(pixel(t, 5, 10) == "0 0 0 0");
    prDrawListRelease(list);
}

TEST_CASE("a backdrop reads what the pixels held beneath the list") {
    // The left half of the pixels is red before the render. A backdrop over the whole target that inverts what lies
    // beneath turns it cyan, and the transparent right half stays transparent.
    Target t = target(4, 1);
    for (uint32_t x = 0; x < 2; ++x) {
        t.bytes[x * 4] = 255;
        t.bytes[x * 4 + 3] = 255;
    }
    PrDrawFilterBuilder* builder = prDrawFilterBuilderCreate();
    PrDrawFilterNode invert{};
    invert.kind = PrDrawFilterKind_ColorMatrix;
    invert.inputs[0] = PrDrawFilterInput_Backdrop;
    const float matrix[20] = {-1, 0, 0, 0, 1, 0, -1, 0, 0, 1, 0, 0, -1, 0, 1, 0, 0, 0, 1, 0};
    for (uint32_t i = 0; i < 20; ++i)
        invert.matrix.m[i / 5][i % 5] = matrix[i];
    prDrawFilterBuilderAdd(builder, &invert);
    const PrDrawFilter* filter = nullptr;
    filter = prDrawFilterCreate(builder);
    prDrawFilterBuilderDestroy(builder);
    PrDrawList* list = prDrawListCreate();
    PrDrawGroup group = prDrawGroupDefault();
    group.bounds = PrRect{0, 0, 4, 1};
    group.backdrop = filter;
    prDrawListPushGroup(list, &group);
    prDrawListPopGroup(list);
    prDrawFilterRelease(filter);
    CHECK(render(t, list) == PrStatus_Ok);
    CHECK(pixel(t, 0, 0) == "0 255 255 255");
    CHECK(pixel(t, 3, 0) == "0 0 0 0");
    prDrawListRelease(list);
}

TEST_CASE("a list source larger than the largest texture still draws, at the largest size") {
    PrDrawList* drawn = prDrawListCreate();
    prDrawListRect(drawn, PrRect{0, 0, 1, 1}, {.color = PrColor{1, 0, 0, 1}});
    PrDrawSource source = prDrawSourceList(drawn, PrRect{0, 0, 1, 1});
    PrDrawBrush brush = prDrawBrushImage(source, PrRect{0, 0, 20000, 20000}, PrDrawSampling_Linear);
    PrDrawList* list = prDrawListCreate();
    prDrawListRect(list, PrRect{0, 0, 4, 4}, {.brush = &brush});
    Target t = target(4, 4);
    render(t, list);
    CHECK(pixel(t, 2, 2) == "255 0 0 255");
    prDrawListRelease(list);
    prDrawListRelease(drawn);
}

TEST_CASE("a group whose filter reaches far draws at a lower resolution, in bounded memory") {
    PrDrawFilterBuilder* builder = prDrawFilterBuilderCreate();
    PrDrawFilterNode blur{};
    blur.kind = PrDrawFilterKind_Blur;
    blur.inputs[0] = PrDrawFilterInput_Source;
    blur.blur.sigma = PrVec2{100000, 100000};
    prDrawFilterBuilderAdd(builder, &blur);
    const PrDrawFilter* filter = nullptr;
    filter = prDrawFilterCreate(builder);
    prDrawFilterBuilderDestroy(builder);
    PrDrawList* list = prDrawListCreate();
    PrDrawGroup group = prDrawGroupDefault();
    group.filter = filter;
    prDrawListPushGroup(list, &group);
    prDrawListRect(list, PrRect{0, 0, 64, 64}, {.color = kBlack});
    prDrawListPopGroup(list);
    Target t = target(64, 64);
    CHECK(render(t, list) == PrStatus_Ok);
    // A blur this wide spreads the square to almost nothing.
    CHECK(t.bytes[(32 * 64 + 32) * 4 + 3] <= 2);
    prDrawListRelease(list);
    prDrawFilterRelease(filter);
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
    Target a = target(8, 6);
    Target b = target(8, 6);
    REQUIRE(render(a, far) == PrStatus_Ok);
    REQUIRE(render(b, near) == PrStatus_Ok);
    CHECK(alphaRow(a, 0) == alphaRow(b, 0));
    CHECK(a.bytes == b.bytes);
    prDrawListRelease(far);
    prDrawListRelease(near);
}

TEST_CASE("a projected group whose matrix is affine draws as its contents under that transform") {
    PrAffine2 t = prAffine2Concat(prAffine2Rotate(0.3f), prAffine2Translate(PrVec2{16, 16}));
    Target flat = target(32, 32);
    PrDrawList* list = prDrawListCreate();
    prDrawListPushTransformAffine2(list, t);
    prDrawListEllipse(list, PrVec2{0, 0}, PrVec2{10, 6}, 0, {.color = kBlack});
    prDrawListPopTransform(list);
    render(flat, list);

    Target projected = target(32, 32);
    prDrawListClear(list);
    PrMatrix4 m = prMatrix4FromAffine2(t);
    pushProjected(list, &m, PrRect{-12, -12, 12, 12});
    prDrawListEllipse(list, PrVec2{0, 0}, PrVec2{10, 6}, 0, {.color = kBlack});
    prDrawListPopGroup(list);
    CHECK(render(projected, list) == PrStatus_Ok);
    // The content is resampled once, bilinearly, so it agrees within that resampling's error along its edge.
    CHECK(differences(flat, projected, 64) == 0);
    CHECK(alphaAt(projected, 16, 16) == 255);
    prDrawListRelease(list);
}

TEST_CASE("a projected group tilted back is narrower at its far edge") {
    Target t = target(40, 40);
    PrDrawList* list = prDrawListCreate();
    PrMatrix4 seen = tiltedPlane(0.8f, 60, PrVec2{20, 20});
    pushProjected(list, &seen, PrRect{-16, -16, 16, 16});
    prDrawListRect(list, PrRect{-16, -16, 16, 16}, {.color = kBlack});
    prDrawListPopGroup(list);
    CHECK(render(t, list) == PrStatus_Ok);
    auto widthOf = [&](uint32_t y) {
        uint32_t count = 0;
        for (uint32_t x = 0; x < 40; ++x)
            count += alphaAt(t, x, y) > 128;
        return count;
    };
    uint32_t top = 0;
    uint32_t bottom = 0;
    uint32_t rows = 0;
    for (uint32_t y = 0; y < 40; ++y) {
        if (widthOf(y) != 0) {
            if (top == 0)
                top = widthOf(y);
            bottom = widthOf(y);
            ++rows;
        }
    }
    CHECK(top + 4 < bottom);
    // The plane is foreshortened: it covers fewer rows than its height.
    CHECK(rows < 32);
    // Each covered pixel lands where the projection puts it.
    PrVec2 center{};
    REQUIRE(prMatrix4MapPoint(&seen, PrVec2{0, 0}, &center));
    CHECK(alphaAt(t, uint32_t(center.x), uint32_t(center.y)) == 255);
    prDrawListRelease(list);
}

TEST_CASE("a projected group is cut where its plane passes behind the eye") {
    // A plane tilted steeply under a near eye. Its red band lies behind the eye, and its green band in front.
    PrMatrix4 seen = tiltedPlane(1.0f, 20, PrVec2{16, 16});
    float behind = 50;
    PrVec2 unused{};
    if (prMatrix4MapPoint(&seen, PrVec2{0, behind}, &unused))
        behind = -behind;
    REQUIRE(!prMatrix4MapPoint(&seen, PrVec2{0, behind}, &unused));
    Target t = target(32, 32);
    PrDrawList* list = prDrawListCreate();
    pushProjected(list, &seen, PrRect{-10, -60, 10, 60});
    prDrawListRect(list, PrRect{-10, behind - 10, 10, behind + 10}, {.color = kRed});
    prDrawListRect(list, PrRect{-10, -10, 10, 10}, {.color = kGreen});
    prDrawListPopGroup(list);
    CHECK(render(t, list) == PrStatus_Ok);
    uint32_t red = 0;
    uint32_t green = 0;
    for (uint32_t y = 0; y < 32; ++y) {
        for (uint32_t x = 0; x < 32; ++x) {
            const uint8_t* p = t.bytes.data() + y * t.pixels.stride + x * 4;
            red += p[0] > 16;
            green += p[1] > 128;
        }
    }
    CHECK(red == 0);
    CHECK(green > 0);
    prDrawListRelease(list);
}

TEST_CASE("a projected group inside another draws through both projections, flattened into the outer plane") {
    PrMatrix4 outer = tiltedPlane(0.6f, 80, PrVec2{24, 24});
    PrMatrix4 inner = tiltedPlane(-0.4f, 120, PrVec2{2, -2});
    Target nested = target(48, 48);
    PrDrawList* list = prDrawListCreate();
    pushProjected(list, &outer, PrRect{-20, -20, 20, 20});
    pushProjected(list, &inner, PrRect{-12, -12, 12, 12});
    prDrawListRect(list, PrRect{-12, -12, 12, 12}, {.color = kBlack});
    prDrawListPopGroup(list);
    prDrawListPopGroup(list);
    CHECK(render(nested, list) == PrStatus_Ok);

    // The same plane through the composition of the two plane maps, resampled once.
    PrMatrix3 outerPlane = prMatrix3FromMatrix4(&outer);
    PrMatrix3 innerPlane = prMatrix3FromMatrix4(&inner);
    PrMatrix3 both = prMatrix3Concat(&innerPlane, &outerPlane);
    PrMatrix4 single = liftPlane(both);
    Target once = target(48, 48);
    prDrawListClear(list);
    pushProjected(list, &single, PrRect{-12, -12, 12, 12});
    prDrawListRect(list, PrRect{-12, -12, 12, 12}, {.color = kBlack});
    prDrawListPopGroup(list);
    CHECK(render(once, list) == PrStatus_Ok);
    CHECK(differences(nested, once, 64) <= 8);
    PrVec2 center{};
    REQUIRE(prMatrix3MapPoint(&both, PrVec2{0, 0}, &center));
    CHECK(alphaAt(nested, uint32_t(center.x), uint32_t(center.y)) == 255);
    prDrawListRelease(list);
}

TEST_CASE("a projected group's opacity, blend mode and enclosing clip apply where it lands") {
    PrMatrix4 seen = tiltedPlane(0.5f, 80, PrVec2{16, 16});
    Target faded = target(32, 32);
    PrDrawList* list = prDrawListCreate();
    PrDrawGroup group = prDrawGroupDefault();
    group.bounds = PrRect{-12, -12, 12, 12};
    group.projection = &seen;
    group.opacity = 0.5f;
    prDrawListPushGroup(list, &group);
    prDrawListRect(list, PrRect{-12, -12, 12, 12}, {.color = kBlack});
    // Overlapping contents compose in the layer before the opacity applies.
    prDrawListRect(list, PrRect{-6, -6, 6, 6}, {.color = kBlack});
    prDrawListPopGroup(list);
    CHECK(render(faded, list) == PrStatus_Ok);
    CHECK(alphaAt(faded, 16, 16) == 128);

    Target multiplied = target(32, 32);
    prDrawListClear(list);
    prDrawListClearRect(list, PrRect{0, 0, 32, 32}, kBlue);
    prDrawListPushClipRect(list, PrRect{0, 0, 16, 32}, PrDrawEdges_Antialiased);
    group.opacity = 1;
    group.blendMode = PrDrawBlendMode_Multiply;
    prDrawListPushGroup(list, &group);
    prDrawListRect(list, PrRect{-12, -12, 12, 12}, {.color = kRed});
    prDrawListPopGroup(list);
    prDrawListPopClip(list);
    CHECK(render(multiplied, list) == PrStatus_Ok);
    CHECK(pixel(multiplied, 14, 16) == "0 0 0 255");
    CHECK(pixel(multiplied, 18, 16) == "0 0 255 255");
    prDrawListRelease(list);
}

TEST_CASE("a backdrop on or inside a projected group reads what lies beneath through the projection") {
    // Red lies beneath the left half. Backdrops that move what they read to the right find it through the plane.
    PrDrawFilterBuilder* builder = prDrawFilterBuilderCreate();
    PrDrawFilterNode offset{};
    offset.kind = PrDrawFilterKind_Offset;
    offset.inputs[0] = PrDrawFilterInput_Backdrop;
    offset.offset = PrVec2{16, 0};
    prDrawFilterBuilderAdd(builder, &offset);
    const PrDrawFilter* filter = prDrawFilterCreate(builder);
    prDrawFilterBuilderDestroy(builder);

    PrMatrix4 identity = prMatrix4Identity();
    Target t = target(32, 32);
    PrDrawList* list = prDrawListCreate();
    prDrawListRect(list, PrRect{0, 0, 16, 32}, {.color = kRed});
    PrDrawGroup projected = prDrawGroupDefault();
    projected.bounds = PrRect{0, 0, 32, 32};
    projected.projection = &identity;
    projected.backdrop = filter;
    prDrawListPushGroup(list, &projected);
    prDrawListRect(list, PrRect{0, 16, 16, 32}, {.color = kGreen});
    PrDrawGroup inner = prDrawGroupDefault();
    inner.bounds = PrRect{0, 0, 32, 32};
    inner.backdrop = filter;
    prDrawListPushGroup(list, &inner);
    prDrawListPopGroup(list);
    prDrawListPopGroup(list);
    CHECK(render(t, list) == PrStatus_Ok);
    // The group's own backdrop and the inner one both read the red beneath. The inner one reads the green that the
    // plane drew over it.
    CHECK(pixel(t, 24, 8) == "255 0 0 255");
    CHECK(pixel(t, 24, 24) == "0 255 0 255");
    CHECK(pixel(t, 8, 8) == "255 0 0 255");
    CHECK(pixel(t, 8, 24) == "0 255 0 255");
    prDrawListRelease(list);

    // A plane drawn at twice its size: the inner backdrop moves what it reads by 16 units of the plane, which are 32
    // target pixels, so the red beneath on the left shows on the right.
    PrMatrix4 doubled = prMatrix4Scale(PrVec3{2, 2, 1});
    Target wide = target(64, 32);
    list = prDrawListCreate();
    prDrawListRect(list, PrRect{0, 0, 16, 32}, {.color = kRed});
    PrDrawGroup plane = prDrawGroupDefault();
    plane.bounds = PrRect{0, 0, 32, 16};
    plane.projection = &doubled;
    prDrawListPushGroup(list, &plane);
    prDrawListPushGroup(list, &inner);
    prDrawListPopGroup(list);
    prDrawListPopGroup(list);
    CHECK(render(wide, list) == PrStatus_Ok);
    CHECK(pixel(wide, 40, 8) == "255 0 0 255");
    CHECK(pixel(wide, 56, 8) == "0 0 0 0");
    CHECK(pixel(wide, 8, 8) == "255 0 0 255");
    prDrawListRelease(list);
    prDrawFilterRelease(filter);
}

TEST_CASE("a projected group whose plane is edge-on draws nothing") {
    PrMatrix4 edge = prMatrix4Rotate(PrVec3{0, 1, 0}, 3.14159265f / 2);
    PrMatrix4 eye = prMatrix4Perspective(10);
    PrMatrix4 place = prMatrix4Translate(PrVec3{2, 0.5f, 0});
    PrMatrix4 seen = prMatrix4Concat(&edge, &eye);
    seen = prMatrix4Concat(&seen, &place);
    Target t = target(4, 1);
    PrDrawList* list = prDrawListCreate();
    pushProjected(list, &seen, PrRect{-5, -5, 5, 5});
    prDrawListRect(list, PrRect{-5, -5, 5, 5}, {.color = kBlack});
    prDrawListPopGroup(list);
    prDrawListRect(list, PrRect{3, 0, 4, 1}, {.color = kBlack});
    CHECK(render(t, list) == PrStatus_Ok);
    CHECK(alphaRow(t, 0) == "0 0 0 255");
    prDrawListRelease(list);
}

TEST_CASE("a blur on a projected group spreads along its plane's axis, wherever the plane lands") {
    // The plane is turned a quarter turn in itself before it tilts, so its x axis runs down the target. A blur along
    // the plane's x spreads the square up and down the target, and not across it.
    PrDrawFilterBuilder* builder = prDrawFilterBuilderCreate();
    PrDrawFilterNode blur{};
    blur.kind = PrDrawFilterKind_Blur;
    blur.inputs[0] = PrDrawFilterInput_Source;
    blur.blur.sigma = PrVec2{3, 0};
    prDrawFilterBuilderAdd(builder, &blur);
    const PrDrawFilter* filter = prDrawFilterCreate(builder);
    prDrawFilterBuilderDestroy(builder);
    PrMatrix4 quarter = prMatrix4Rotate(PrVec3{0, 0, 1}, 3.14159265f / 2);
    PrMatrix4 tilted = tiltedPlane(0.4f, 120, PrVec2{16, 16});
    PrMatrix4 seen = prMatrix4Concat(&quarter, &tilted);
    Target t = target(32, 32);
    PrDrawList* list = prDrawListCreate();
    PrDrawGroup group = prDrawGroupDefault();
    group.bounds = PrRect{-14, -14, 14, 14};
    group.projection = &seen;
    group.filter = filter;
    prDrawListPushGroup(list, &group);
    prDrawListRect(list, PrRect{-2, -2, 2, 2}, {.color = kBlack});
    prDrawListPopGroup(list);
    prDrawFilterRelease(filter);
    CHECK(render(t, list) == PrStatus_Ok);
    PrVec2 center{};
    REQUIRE(prMatrix4MapPoint(&seen, PrVec2{0, 0}, &center));
    uint32_t cx = uint32_t(center.x);
    uint32_t cy = uint32_t(center.y);
    CHECK(alphaAt(t, cx, cy) > 0);
    CHECK(alphaAt(t, cx, cy - 5) > 8);
    CHECK(alphaAt(t, cx, cy + 5) > 8);
    CHECK(alphaAt(t, cx - 5, cy) == 0);
    CHECK(alphaAt(t, cx + 5, cy) == 0);
    prDrawListRelease(list);
}

TEST_CASE("a half-float render keeps colors past white through paint, groups and filters") {
    // Columns of eight pixels: a bright solid, a gradient at three times white, a half-float image at twice white, a
    // group at half opacity over four times white, and a blurred group over three times white.
    PrDrawList* list = prDrawListCreate();
    const PrColor bright{4, 2, 0.5f, 1};
    prDrawListRect(list, PrRect{0, 0, 8, 8}, {.color = bright});
    PrDrawColorStop stops[2] = {
        {.offset = 0, .color = PrColor{3, 3, 3, 1}},
        {.offset = 1, .color = PrColor{3, 3, 3, 1}},
    };
    PrDrawBrush gradient = prDrawBrushLinear(PrVec2{8, 0}, PrVec2{16, 0}, stops, 2);
    prDrawListRect(list, PrRect{8, 0, 16, 8}, {.brush = &gradient});
    PrFloat16 texels[4 * 4];
    for (uint32_t i = 0; i < 16; ++i)
        texels[i] = prFloat16FromFloat(i % 4 < 3 ? 2.0f : 1.0f);
    PrDrawImageDesc desc{
        .pixels = PrPixelView{reinterpret_cast<const uint8_t*>(texels), 16, {2, 2}},
        .format = PrPixelFormat_RgbaF16,
        .alpha = PrDrawAlpha_Premultiplied,
        .space = PrColorSpace_Srgb,
    };
    const PrDrawImage* image = nullptr;
    REQUIRE(prDrawImageCreate(&desc, &image) == PrStatus_Ok);
    drawImageRect(list, prDrawSourceImage(image, PrRect{0, 0, 2, 2}), PrRect{16, 0, 24, 8}, PrDrawSampling_Nearest);
    PrDrawGroup half = prDrawGroupDefault();
    half.opacity = 0.5f;
    prDrawListPushGroup(list, &half);
    prDrawListRect(list, PrRect{24, 0, 32, 8}, {.color = PrColor{4, 4, 4, 1}});
    prDrawListPopGroup(list);
    PrDrawFilterBuilder* builder = prDrawFilterBuilderCreate();
    PrDrawFilterNode blur{};
    blur.kind = PrDrawFilterKind_Blur;
    blur.inputs[0] = PrDrawFilterInput_Source;
    blur.blur.sigma = PrVec2{1, 1};
    prDrawFilterBuilderAdd(builder, &blur);
    const PrDrawFilter* filter = prDrawFilterCreate(builder);
    prDrawFilterBuilderDestroy(builder);
    PrDrawGroup blurred = prDrawGroupDefault();
    blurred.filter = filter;
    prDrawListPushGroup(list, &blurred);
    prDrawListRect(list, PrRect{32, -8, 64, 16}, {.color = PrColor{3, 3, 3, 1}});
    prDrawListPopGroup(list);

    constexpr uint32_t kWidth = 48;
    std::vector<PrFloat16> wide(size_t(kWidth) * 8 * 4);
    PrPixels pixels{reinterpret_cast<uint8_t*>(wide.data()), size_t(kWidth) * 8, {kWidth, 8}};
    PrDrawRenderer* renderer = prDrawCpuCreate(nullptr);
    auto at = [&](uint32_t column) {
        const PrFloat16* p = wide.data() + (size_t(4) * kWidth + column * 8 + 4) * 4;
        return PrColor(PrColor16{p[0], p[1], p[2], p[3]});
    };
    auto near = [](PrColor a, PrColor b) {
        return std::abs(a.r - b.r) < 0.02f && std::abs(a.g - b.g) < 0.02f && std::abs(a.b - b.b) < 0.02f &&
            std::abs(a.a - b.a) < 0.02f;
    };
    REQUIRE(prDrawRendererRenderPixels(renderer, list, pixels, PrPixelFormat_RgbaF16) == PrStatus_Ok);
    CHECK(near(at(0), bright));
    CHECK(near(at(1), PrColor{3, 3, 3, 1}));
    CHECK(near(at(2), PrColor{2, 2, 2, 1}));
    CHECK(near(at(3), PrColor{2, 2, 2, 0.5f}));
    CHECK(near(at(4), PrColor{3, 3, 3, 1}));

    // A frame renders the same colors, encoded, whatever the display it is shown on, and refuses a target of 10-bit
    // channels.
    PrDrawFrame* frame = prDrawFrameCreate(renderer);
    PrDrawTarget wideTarget{.extent = {kWidth, 8}, .format = PrPixelFormat_RgbaF16};
    REQUIRE(prDrawFramePrepare(frame, list, &wideTarget) == PrStatus_Ok);
    std::fill(wide.begin(), wide.end(), PrFloat16{});
    prDrawFrameRenderPixels(frame, pixels, PrPixelFormat_RgbaF16);
    CHECK(near(at(0), bright));
    CHECK(near(at(1), PrColor{3, 3, 3, 1}));
    PrDrawTarget packed{.extent = {kWidth, 8}, .format = PrPixelFormat_Rgb10A2};
    CHECK(prDrawFramePrepare(frame, list, &packed) == PrStatus_NotSupported);
    prDrawFrameDestroy(frame);

    // An image brush with a limit scales its texels down to it in linear light.
    PrDrawList* limited = prDrawListCreate();
    PrDrawBrush capped = prDrawBrushImage(prDrawSourceImage(image, PrRect{0, 0, 2, 2}), PrRect{0, 0, 48, 8},
        PrDrawSampling_Nearest);
    capped.image.limit = 1.5f;
    prDrawListRect(limited, PrRect{0, 0, 48, 8}, {.brush = &capped});
    std::fill(wide.begin(), wide.end(), PrFloat16{});
    REQUIRE(prDrawRendererRenderPixels(renderer, limited, pixels, PrPixelFormat_RgbaF16) == PrStatus_Ok);
    float limit = 1.055f * std::pow(1.5f, 1 / 2.4f) - 0.055f;
    CHECK(near(at(0), PrColor{limit, limit, limit, 1}));
    prDrawListRelease(limited);

    // An 8-bit render of the same list clamps.
    Target t = target(kWidth, 8);
    REQUIRE(render(t, list) == PrStatus_Ok);
    CHECK(pixel(t, 4, 4) == "255 255 128 255");
    prDrawRendererDestroy(renderer);
    prDrawFilterRelease(filter);
    prDrawImageRelease(image);
    prDrawListRelease(list);
}

TEST_CASE("a group draws into the host's pixels, which keep each revision's contents once") {
    Target held = target(4, 4);
    PrDrawCpuTexture texture{held.pixels, held.format};
    uint64_t drawn = 0;
    PrDrawGroupLayer layer{
        .texture = prDrawCpuTexture(&texture),
        .extent = {4, 4},
        .drawn = &drawn,
        .sampling = PrDrawSampling_Nearest,
    };
    // A list of revisions `first` to `last` of a group whose bounds cover the target. Revision 1 replaces the pixels
    // with red, and each later one draws half-opaque blue over the left half, which would darken if it drew twice.
    auto list = [&](uint64_t first, uint64_t last) {
        PrDrawList* l = prDrawListCreate();
        layer.revision = last;
        PrDrawGroup group = prDrawGroupDefault();
        group.bounds = PrRect{0, 0, 8, 8};
        group.layer = &layer;
        prDrawListPushGroup(l, &group);
        for (uint64_t revision = first; revision <= last; ++revision) {
            PrDrawGroup segment = prDrawGroupDefault();
            segment.revision = revision;
            prDrawListPushGroup(l, &segment);
            if (revision == 1)
                prDrawListRect(l, PrRect{0, 0, 8, 8},
                    {.color = PrColor{1, 0, 0, 1}, .composite = PrDrawComposite_Copy});
            else
                prDrawListRect(l, PrRect{0, 0, 4, 8}, {.color = PrColor{0, 0, 1, 0.5f}});
            prDrawListPopGroup(l);
        }
        prDrawListPopGroup(l);
        return l;
    };
    auto drawnOnce = [&](const PrDrawList* l) {
        Target t = target(8, 8);
        REQUIRE(render(t, l) == PrStatus_Ok);
        return t;
    };
    PrDrawList* first = list(1, 1);
    Target once = drawnOnce(first);
    CHECK(drawn == 1);
    CHECK(pixel(once, 6, 6) == "255 0 0 255");
    CHECK(pixel(held, 3, 3) == "255 0 0 255");
    // The same list again draws nothing new into the pixels, which still show what they hold.
    CHECK(pixel(drawnOnce(first), 6, 6) == "255 0 0 255");
    // A list of revisions 1 and 2 draws only 2 over what the pixels hold, and drawn again draws nothing new.
    PrDrawList* second = list(1, 2);
    Target blended = drawnOnce(second);
    CHECK(drawn == 2);
    CHECK(pixel(blended, 6, 6) == "255 0 0 255");
    CHECK(pixel(blended, 1, 1) != "255 0 0 255");
    CHECK(pixel(drawnOnce(second), 1, 1) == pixel(blended, 1, 1));
    prDrawListRelease(first);
    prDrawListRelease(second);
}
