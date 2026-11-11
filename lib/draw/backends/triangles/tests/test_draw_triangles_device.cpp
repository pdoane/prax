#include "pr/draw_cpu.h"
#include "pr/draw_list.h"
#include "pr/draw_triangles.h"
#include "pr/font.h"
#include "pr/gpu.h"
#include "pr/vec.h"

#include "tools/harness/test.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

extern "C" int prTestDrawTrianglesCApi(void);

namespace pr_impl {

namespace {

struct Device {
    PrGpuDevice* gpuDevice = nullptr;
    PrDrawRenderer* renderer = nullptr;
    PrGpuTexture texture{};
    PrDrawFrame* frame = nullptr;

    Device(uint32_t width, uint32_t height) {
        REQUIRE(prGpuDeviceCreate(PrGpuBackend_Default, nullptr, &gpuDevice) == PrStatus_Ok);
        REQUIRE(prDrawTrianglesCreate(gpuDevice, nullptr, &renderer) == PrStatus_Ok);
        PrGpuTextureDesc desc = prGpuTextureDesc2d(PrGpuFormat_Rgba8, PrUVec2{width, height},
            PrGpuTextureUsageFlag_Sampled | PrGpuTextureUsageFlag_RenderTarget);
        REQUIRE(prGpuTextureCreate(gpuDevice, &desc, &texture) == PrStatus_Ok);
    }

    ~Device() {
        prDrawFrameDestroy(frame);
        prDrawRendererDestroy(renderer);
        prGpuTextureDestroy(gpuDevice, texture);
        prGpuDeviceDestroy(gpuDevice);
    }

    // Prepares and encodes the list in a pass over the texture, then reads the texture back.
    std::vector<uint8_t> render(const PrDrawList* list) {
        PrGpuCommands* commands = nullptr;
        REQUIRE(prGpuCommandsBegin(gpuDevice, nullptr, &commands));
        PrDrawTarget prepared{
            .extent = prGpuTextureExtent(gpuDevice, texture),
            .format = PrPixelFormat_Rgba8,
            .commands = commands,
        };
        if (frame == nullptr)
            frame = prDrawFrameCreate(renderer);
        REQUIRE(prDrawFramePrepare(frame, list, &prepared) == PrStatus_Ok);
        PrGpuSubresources all{};
        prGpuCommandsTextureBarrier(commands, texture, all, PrGpuAccessFlag_ShaderRead,
            PrGpuAccessFlag_ColorAttachment);
        PrGpuPassDesc desc = prGpuPassDescColor(texture, PrGpuLoad_Clear, PrColor{0, 0, 0, 0});
        PrGpuPass* pass = prGpuPassBegin(commands, &desc);
        prDrawFrameRender(frame, pass, PrIVec2{0, 0});
        prGpuPassEnd(pass);
        prGpuCommandsTextureBarrier(commands, texture, all, PrGpuAccessFlag_ColorAttachment,
            PrGpuAccessFlag_ShaderRead);
        prGpuCommandsSubmitAndWait(commands);
        uint32_t width = prGpuTextureExtent(gpuDevice, texture).x;
        uint32_t height = prGpuTextureExtent(gpuDevice, texture).y;
        std::vector<uint8_t> pixels(size_t(width) * height * 4);
        PrPixels out{pixels.data(), size_t(width) * 4, {width, height}};
        prGpuTextureRead(gpuDevice, texture, PrGpuAccessFlag_ShaderRead, PrIVec2{0, 0}, out);
        return pixels;
    }

    // Returns the counts of the work that the last render's prepare produced.
    PrDrawStats stats() const {
        return prDrawFrameStats(frame);
    }
};

// Returns the list rendered by the cpu backend onto transparent pixels, which the test compares the device's render
// with.
std::vector<uint8_t> cpuRender(const PrDrawList* list, uint32_t width, uint32_t height) {
    std::vector<uint8_t> pixels(size_t(width) * height * 4, 0);
    PrDrawRenderer* renderer = prDrawCpuCreate(nullptr);
    PrPixels into{pixels.data(), size_t(width) * 4, {width, height}};
    REQUIRE(prDrawRendererRenderPixels(renderer, list, into, PrPixelFormat_Rgba8) == PrStatus_Ok);
    prDrawRendererDestroy(renderer);
    return pixels;
}

double inked(const std::vector<uint8_t>& pixels) {
    double sum = 0;
    for (size_t i = 3; i < pixels.size(); i += 4)
        sum += pixels[i] / 255.0;
    return sum;
}

const uint8_t* at(const std::vector<uint8_t>& pixels, uint32_t width, uint32_t x, uint32_t y) {
    return pixels.data() + (size_t(y) * width + x) * 4;
}

const PrFont* load(const char* name) {
    std::string path = std::string(PR_FONTS_DIR) + "/" + name;
    const PrFont* font = nullptr;
    REQUIRE(prFontCreateFromFile(path, 0, nullptr, &font) == PrStatus_Ok);
    return font;
}

// Returns the largest difference in any channel of any pixel.
int largestDifference(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    int largest = 0;
    for (size_t i = 0; i < a.size(); ++i)
        largest = std::max(largest, std::abs(int(a[i]) - int(b[i])));
    return largest;
}

// Returns the fraction of the pixels that are opaque in both images and whose color differs by more than `tolerance` in
// a channel.
double fractionDiffering(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, int tolerance) {
    uint32_t opaque = 0;
    uint32_t differing = 0;
    for (size_t i = 0; i < a.size(); i += 4) {
        if (a[i + 3] != 255 || b[i + 3] != 255)
            continue;
        ++opaque;
        for (size_t k = 0; k < 3; ++k) {
            if (std::abs(int(a[i + k]) - int(b[i + k])) > tolerance) {
                ++differing;
                break;
            }
        }
    }
    return opaque == 0 ? 0 : double(differing) / opaque;
}

void addText(PrDrawList* list, const PrFont* font, float size, const char* text, PrVec2 at,
        const PrDrawStroke* stroke = nullptr) {
    std::vector<PrFontGlyph> glyphs;
    for (const char* c = text; *c != 0; ++c) {
        uint16_t id = prFontCodePointGlyph(font, uint32_t(*c));
        glyphs.push_back(PrFontGlyph{id, at});
        at.x += prFontGlyphAdvance(font, id, 0) * size;
    }
    PrFontGlyphRun run{};
    run.font = font;
    run.size = size;
    run.glyphs = glyphs.data();
    run.glyphCount = uint32_t(glyphs.size());
    prDrawListGlyphs(list, &run, PrVec2{}, {.color = PrColor{0, 0, 0, 1}, .stroke = stroke});
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("the triangles renderer draws a frame on the device") {
    Device device(64, 64);
    CHECK(prDrawRendererDevice(device.renderer) == device.gpuDevice);
    PrDrawList* list = prDrawListCreate();
    prDrawListClearRect(list, PrRect{0, 0, 64, 64}, PrColor{1, 1, 1, 1});
    prDrawListEllipse(list, PrVec2{32, 32}, PrVec2{20, 12}, 0.4f, {.color = PrColor{1, 0, 0, 1}});
    prDrawListRect(list, PrRect{32, 0, 64, 64}, {.color = PrColor{0, 0, 0, 0.5f}});
    std::vector<uint8_t> drawn = device.render(list);
    std::vector<uint8_t> expected = cpuRender(list, 64, 64);
    CHECK(at(drawn, 64, 2, 2)[0] == 255);
    CHECK(at(drawn, 64, 2, 2)[3] == 255);
    CHECK(at(drawn, 64, 24, 32)[0] == 255);
    CHECK(at(drawn, 64, 24, 32)[1] == 0);
    CHECK(int(at(drawn, 64, 60, 60)[1]) == doctest::Approx(int(at(expected, 64, 60, 60)[1])).epsilon(0.02));
    double red = 0;
    double expectedRed = 0;
    for (size_t i = 0; i < drawn.size(); i += 4) {
        red += drawn[i] - drawn[i + 1];
        expectedRed += expected[i] - expected[i + 1];
    }
    CHECK(red == doctest::Approx(expectedRed).epsilon(0.01));
    prDrawListRelease(list);
}

TEST_CASE("the triangles renderer draws glyphs from the atlas as the cpu backend covers them") {
    Device device(128, 64);
    const PrFont* font = load("NotoSans[wdth,wght].ttf");
    PrDrawList* list = prDrawListCreate();
    std::vector<PrFontGlyph> glyphs;
    PrVec2 pen{8, 44};
    for (const char* c = "Hxg"; *c != 0; ++c) {
        uint16_t id = prFontCodePointGlyph(font, uint32_t(*c));
        glyphs.push_back(PrFontGlyph{id, pen});
        pen.x += prFontGlyphAdvance(font, id, 0) * 32;
    }
    PrFontGlyphRun run{};
    run.font = font;
    run.size = 32;
    run.glyphs = glyphs.data();
    run.glyphCount = uint32_t(glyphs.size());
    prDrawListGlyphs(list, &run, PrVec2{}, {.color = PrColor{0, 0, 0, 1}});
    std::vector<uint8_t> drawn = device.render(list);
    double ink = inked(cpuRender(list, 128, 64));
    CHECK(inked(drawn) == doctest::Approx(ink).epsilon(0.02));
    // The text is drawn again, from the atlas that is already on the device.
    CHECK(inked(device.render(list)) == doctest::Approx(ink).epsilon(0.02));
    prDrawListRelease(list);
    prFontRelease(font);
}

TEST_CASE("the triangles renderer draws the host's texture of straight texels as the cpu backend draws their image") {
    Device device(32, 16);
    std::vector<uint8_t> texels(4 * 4 * 4);
    for (uint32_t i = 0; i < 16; ++i) {
        uint8_t* t = texels.data() + i * 4;
        bool left = i % 4 < 2;
        t[0] = left ? 255 : 0;
        t[2] = left ? 0 : 255;
        t[3] = left ? 128 : 255;
    }
    PrGpuTextureDesc desc = prGpuTextureDesc2d(prGpuFormatFromPixelFormat(PrPixelFormat_Rgba8), PrUVec2{4, 4},
        PrGpuTextureUsageFlag_Sampled);
    PrGpuTexture texture{};
    REQUIRE(prGpuTextureCreate(device.gpuDevice, &desc, &texture) == PrStatus_Ok);
    PrPixels upload{texels.data(), 16, {4, 4}};
    prGpuTextureWrite(device.gpuDevice, texture, PrIVec2{0, 0}, prPixelsView(upload));
    PrDrawImageDesc imageDesc{
        PrPixelView{texels.data(), 16, {4, 4}}, PrPixelFormat_Rgba8, PrDrawAlpha_Straight,
        PrColorSpace_Srgb,
    };
    const PrDrawImage* image = nullptr;
    REQUIRE(prDrawImageCreate(&imageDesc, &image) == PrStatus_Ok);
    auto draw = [&](PrDrawSource source) {
        PrDrawList* list = prDrawListCreate();
        PrDrawBrush brush = prDrawBrushImage(source, PrRect{0, 0, 32, 16}, PrDrawSampling_Nearest);
        prDrawListRect(list, PrRect{0, 0, 32, 16}, {.brush = &brush});
        return list;
    };
    PrDrawList* fromTexture = draw(prDrawSourceTexture(prDrawGpuTexture(texture), prGpuTextureExtent(device.gpuDevice, texture),
        PrRect{0, 0, 4, 4}, PrDrawAlpha_Straight));
    PrDrawList* fromImage = draw(prDrawSourceImage(image, PrRect{0, 0, 4, 4}));
    std::vector<uint8_t> drawn = device.render(fromTexture);
    std::vector<uint8_t> expected = cpuRender(fromImage, 32, 16);
    for (uint32_t y : {4u, 12u}) {
        for (uint32_t x : {4u, 28u}) {
            for (uint32_t k = 0; k < 4; ++k)
                CHECK(std::abs(int(drawn[(y * 32 + x) * 4 + k]) - int(expected[(y * 32 + x) * 4 + k])) <= 2);
        }
    }
    prDrawListRelease(fromTexture);
    prDrawListRelease(fromImage);
    prDrawImageRelease(image);
    prDrawFrameDestroy(device.frame);
    device.frame = nullptr;
    prGpuTextureDestroy(device.gpuDevice, texture);
}

TEST_CASE("triangles fills shapes with the coverage the cpu backend gives them") {
    Device h(96, 64);
    PrDrawList* list = prDrawListCreate();
    prDrawListRect(list, PrRect{4.25f, 6.5f, 40.75f, 30}, {.color = PrColor{1, 0, 0, 1}});
    prDrawListEllipse(list, PrVec2{64, 32}, PrVec2{20, 14}, 0.3f, {.color = PrColor{0, 0, 1, 0.8f}});
    prDrawListCornerRect(list, prCornerRectRound(PrRect{6, 36, 44, 60}, 8), {.color = PrColor{0, 0.5f, 0, 1}});
    std::vector<uint8_t> drawn = h.render(list);
    std::vector<uint8_t> expected = cpuRender(list, 96, 64);
    CHECK(inked(drawn) == doctest::Approx(inked(expected)).epsilon(0.005));
    CHECK(largestDifference(drawn, expected) <= 48);
    // Checks a pixel inside each shape.
    CHECK(at(drawn, 96, 20, 20)[0] == 255);
    CHECK(at(drawn, 96, 64, 32)[2] == 204);
    prDrawListRelease(list);
}

TEST_CASE("triangles fills a path by its rule and strokes it") {
    Device h(64, 64);
    PrDrawList* list = prDrawListCreate();
    PrPath* path = prPathCreate();
    prPathAddRect(path, PrRect{8, 8, 56, 56});
    prPathAddEllipse(path, PrVec2{32, 32}, PrVec2{12, 12}, 0);
    prDrawListPath(list, path, PrPathFillRule_EvenOdd, {.color = PrColor{0, 0, 0, 1}});
    std::vector<uint8_t> drawn = h.render(list);
    std::vector<uint8_t> expected = cpuRender(list, 64, 64);
    CHECK(inked(drawn) == doctest::Approx(inked(expected)).epsilon(0.005));
    CHECK(at(drawn, 64, 32, 32)[3] == 0);

    prDrawListClear(list);
    PrDrawStroke stroke = prDrawStrokeDefault();
    stroke.shape.width = 3;
    stroke.shape.join = PrPathJoin_Round;
    prDrawListPath(list, path, PrPathFillRule_NonZero, {.color = PrColor{0, 0, 0, 1}, .stroke = &stroke});
    drawn = h.render(list);
    expected = cpuRender(list, 64, 64);
    CHECK(inked(drawn) == doctest::Approx(inked(expected)).epsilon(0.01));
    CHECK(largestDifference(drawn, expected) <= 64);

    // A stroke narrower than a pixel is drawn a pixel wide, and its opacity is scaled by its width.
    prDrawListClear(list);
    stroke.shape.width = 0.25f;
    prDrawListLine(list, PrVec2{4, 10.5f}, PrVec2{60, 10.5f}, {.color = PrColor{0, 0, 0, 1}, .stroke = &stroke});
    drawn = h.render(list);
    expected = cpuRender(list, 64, 64);
    CHECK(inked(drawn) == doctest::Approx(inked(expected)).epsilon(0.02));
    CHECK(int(at(drawn, 64, 30, 10)[3]) == doctest::Approx(64).epsilon(0.05));
    prPathDestroy(path);
    prDrawListRelease(list);
}

TEST_CASE("triangles evaluates a gradient at each vertex") {
    Device h(64, 16);
    PrDrawList* list = prDrawListCreate();
    PrDrawColorStop stops[2] = {
        {.offset = 0, .color = PrColor{1, 0, 0, 1}},
        {.offset = 1, .color = PrColor{0, 0, 1, 1}},
    };
    PrDrawBrush brush = prDrawBrushLinear(PrVec2{0, 0}, PrVec2{64, 0}, stops, 2);
    prDrawListRect(list, PrRect{0, 0, 64, 16}, {.brush = &brush});
    std::vector<uint8_t> drawn = h.render(list);
    // A two-stop linear gradient is linear across the rectangle, so interpolation between the vertices reproduces it.
    CHECK(int(at(drawn, 64, 1, 8)[0]) >= 245);
    CHECK(int(at(drawn, 64, 62, 8)[2]) >= 245);
    CHECK(int(at(drawn, 64, 32, 8)[0]) == doctest::Approx(125).epsilon(0.05));
    prDrawListRelease(list);
}

TEST_CASE("triangles cuts each kind of gradient where its colors turn") {
    PrDrawColorStop wheel[7];
    for (uint32_t i = 0; i < 7; ++i) {
        float hue = float(i % 6) / 6;
        auto clamp = [](float v) { return v < 0 ? 0 : v > 1 ? 1 : v; };
        wheel[i] = PrDrawColorStop{
            .offset = float(i) / 6,
            .color = PrColor{
                clamp(std::fabs(hue * 6 - 3) - 1),
                clamp(2 - std::fabs(hue * 6 - 2)),
                clamp(2 - std::fabs(hue * 6 - 4)), 1,
            },
        };
    }
    PrDrawColorStop hard[4] = {
        {.offset = 0, .color = PrColor{1, 0, 0, 1}},
        {.offset = 0.5f, .color = PrColor{1, 0, 0, 1}},
        {.offset = 0.5f, .color = PrColor{0, 0, 1, 1}},
        {.offset = 1, .color = PrColor{0, 0, 1, 1}},
    };
    struct Case {
        PrDrawBrushKind kind;
        PrDrawExtend extend;
        const PrDrawColorStop* stops;
        uint32_t count;
        int tolerance;
    };
    const Case cases[] = {
        {PrDrawBrushKind_Linear, PrDrawExtend_Repeat, wheel, 7, 4},
        {PrDrawBrushKind_Linear, PrDrawExtend_Reflect, hard, 4, 4},
        {PrDrawBrushKind_Radial, PrDrawExtend_Repeat, wheel, 7, 8},
        {PrDrawBrushKind_Conic, PrDrawExtend_Pad, wheel, 7, 12},
        {PrDrawBrushKind_Diamond, PrDrawExtend_Reflect, wheel, 7, 4},
    };
    for (const Case& k : cases) {
        CAPTURE(int(k.kind));
        Device h(200, 200);
        PrDrawList* list = prDrawListCreate();
        // At twice the scale, such as on a high-density display.
        prDrawListPushTransformAffine2(list, prAffine2Scale(PrVec2{2, 2}));
        PrDrawBrush brush = prDrawBrushDefault(k.kind);
        brush.extend = k.extend;
        brush.transform = prAffine2Rotate(0.3f);
        brush.stops = k.stops;
        brush.stopCount = k.count;
        brush.linear.start = PrVec2{10, 10};
        brush.linear.end = PrVec2{40, 20};
        if (k.kind == PrDrawBrushKind_Radial)
            brush.radial = {PrVec2{50, 20}, 0, PrVec2{50, 20}, 30};
        if (k.kind == PrDrawBrushKind_Conic)
            brush.conic = {PrVec2{50, 20}, 0.4f};
        if (k.kind == PrDrawBrushKind_Diamond)
            brush.diamond = {PrVec2{50, 20}, PrVec2{25, 15}};
        prDrawListEllipse(list, PrVec2{50, 50}, PrVec2{45, 40}, 0, {.brush = &brush});
        prDrawListPopTransform(list);
        std::vector<uint8_t> drawn = h.render(list);
        std::vector<uint8_t> expected = cpuRender(list, 200, 200);
        // A pixel on a hard stop takes one color from the cpu backend, and a mix of both from the triangles.
        CHECK(fractionDiffering(drawn, expected, k.tolerance) <= 0.005);
        prDrawListRelease(list);
    }
}

TEST_CASE("triangles places a brush along and across a stroke") {
    PrDrawColorStop bands[4] = {
        {.offset = 0, .color = PrColor{0, 0, 0, 1}},
        {.offset = 0.5f, .color = PrColor{0, 0, 0, 1}},
        {.offset = 0.5f, .color = PrColor{1, 0.7f, 0, 1}},
        {.offset = 1, .color = PrColor{1, 0.7f, 0, 1}},
    };
    for (PrDrawStrokeMapping mapping : {PrDrawStrokeMapping_Along, PrDrawStrokeMapping_Across}) {
        CAPTURE(int(mapping));
        Device h(200, 100);
        PrDrawList* list = prDrawListCreate();
        PrVec2 end = mapping == PrDrawStrokeMapping_Along ? PrVec2{0.1f, 0} : PrVec2{0.5f, 0};
        PrDrawBrush brush = prDrawBrushLinear(PrVec2{0, 0}, end, bands, 4);
        brush.extend = PrDrawExtend_Repeat;
        PrPath* path = prPathCreate();
        prPathMoveTo(path, PrVec2{20, 50});
        prPathCubicTo(path, PrVec2{60, -10}, PrVec2{140, 110}, PrVec2{180, 50});
        PrDrawStroke stroke = prDrawStrokeDefault();
        stroke.shape.width = 16;
        stroke.shape.join = PrPathJoin_Round;
        stroke.shape.startCap = PrPathCap_Round;
        stroke.shape.endCap = PrPathCap_Round;
        stroke.mapping = mapping;
        prDrawListPath(list, path, PrPathFillRule_NonZero, {.brush = &brush, .stroke = &stroke});
        std::vector<uint8_t> drawn = h.render(list);
        std::vector<uint8_t> expected = cpuRender(list, 200, 100);
        CHECK(inked(drawn) == doctest::Approx(inked(expected)).epsilon(0.01));
        CHECK(fractionDiffering(drawn, expected, 16) <= 0.02);
        prPathDestroy(path);
        prDrawListRelease(list);
    }
}

TEST_CASE("triangles scissors to a rectangle clip and cuts to a convex one") {
    Device h(32, 32);
    PrDrawList* list = prDrawListCreate();
    prDrawListPushClipRect(list, PrRect{8, 8, 24, 24}, PrDrawEdges_Antialiased);
    prDrawListRect(list, PrRect{0, 0, 32, 32}, {.color = PrColor{0, 0, 0, 1}});
    prDrawListPopClip(list);
    std::vector<uint8_t> drawn = h.render(list);
    CHECK(inked(drawn) == doctest::Approx(256));
    CHECK(h.stats().draws == 1);

    prDrawListClear(list);
    prDrawListPushTransformAffine2(list, prAffine2Rotate(0.5f));
    prDrawListPushClipRect(list, PrRect{8, 0, 20, 12}, PrDrawEdges_Antialiased);
    prDrawListPopClip(list);
    prDrawListPopTransform(list);
    PrPath* path = prPathCreate();
    prPathAddEllipse(path, PrVec2{12, 12}, PrVec2{8, 8}, 0);
    prDrawListPushClipPath(list, path, PrPathFillRule_NonZero, PrDrawEdges_Antialiased);
    prDrawListRect(list, PrRect{0, 0, 32, 32}, {.color = PrColor{0, 0, 0, 1}});
    prDrawListPopClip(list);
    drawn = h.render(list);
    // Its edge is aliased: a pixel is inside if its center is inside.
    CHECK(inked(drawn) == doctest::Approx(3.14159 * 64).epsilon(0.05));

    // A rectangle turned by the transform clips to its turned outline.
    prDrawListClear(list);
    prDrawListPushTransformAffine2(list, prAffine2Concat(prAffine2Rotate(0.5f), prAffine2Translate(PrVec2{16, 4})));
    prDrawListPushClipRect(list, PrRect{0, 0, 12, 12}, PrDrawEdges_Antialiased);
    prDrawListRect(list, PrRect{-40, -40, 40, 40}, {.color = PrColor{0, 0, 0, 1}});
    prDrawListPopClip(list);
    prDrawListPopTransform(list);
    drawn = h.render(list);
    CHECK(inked(drawn) == doctest::Approx(144).epsilon(0.05));

    // A clip that is not convex scissors to its bounds.
    prDrawListClear(list);
    prPathClear(path);
    prPathAddRect(path, PrRect{4, 4, 12, 28});
    prPathAddRect(path, PrRect{20, 4, 28, 28});
    prDrawListPushClipPath(list, path, PrPathFillRule_NonZero, PrDrawEdges_Antialiased);
    prDrawListRect(list, PrRect{0, 0, 32, 32}, {.color = PrColor{0, 0, 0, 1}});
    prDrawListPopClip(list);
    drawn = h.render(list);
    CHECK(inked(drawn) == doctest::Approx(24 * 24));
    prPathDestroy(path);
    prDrawListRelease(list);
}

TEST_CASE("triangles multiplies a group's opacity into what it draws") {
    Device h(16, 16);
    PrDrawList* list = prDrawListCreate();
    PrDrawGroup group = prDrawGroupDefault();
    group.opacity = 0.5f;
    prDrawListPushGroup(list, &group);
    prDrawListRect(list, PrRect{0, 0, 16, 16}, {.color = PrColor{0, 0, 0, 1}});
    prDrawListPopGroup(list);
    std::vector<uint8_t> drawn = h.render(list);
    CHECK(int(at(drawn, 16, 8, 8)[3]) == doctest::Approx(128).epsilon(0.02));
    prDrawListRelease(list);
}

TEST_CASE("triangles draws nothing for a projected group and its contents") {
    Device h(32, 16);
    PrDrawList* list = prDrawListCreate();
    PrMatrix4 flat = prMatrix4Translate(PrVec3{0, 0, 0});
    PrDrawGroup group = prDrawGroupDefault();
    group.bounds = PrRect{0, 0, 16, 16};
    group.projection = &flat;
    prDrawListPushGroup(list, &group);
    prDrawListRect(list, PrRect{0, 0, 16, 16}, {.color = PrColor{0, 0, 0, 1}});
    prDrawListPushGroup(list, &group);
    prDrawListRect(list, PrRect{0, 0, 16, 16}, {.color = PrColor{0, 0, 0, 1}});
    prDrawListPopGroup(list);
    prDrawListPopGroup(list);
    prDrawListRect(list, PrRect{16, 0, 32, 16}, {.color = PrColor{0, 0, 0, 1}});
    std::vector<uint8_t> drawn = h.render(list);
    CHECK(inked(drawn) == doctest::Approx(256));
    CHECK(at(drawn, 32, 8, 8)[3] == 0);
    CHECK(at(drawn, 32, 24, 8)[3] == 255);
    prDrawListRelease(list);
}

TEST_CASE("triangles uploads its atlas and images once") {
    Device h(32, 32);
    PrDrawList* list = prDrawListCreate();
    prDrawListRect(list, PrRect{0, 0, 8, 8}, {.color = PrColor{0, 0, 0, 1}});
    h.render(list);
    uint64_t first = h.stats().uploadBytes;
    h.render(list);
    // The second frame uploads only its vertices and indices.
    uint64_t geometry = h.stats().uploadBytes;
    CHECK(geometry < first);

    uint8_t texels[4 * 4] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
    PrDrawImageDesc target{
        PrPixelView{texels, 8, {2, 2}}, PrPixelFormat_Rgba8, PrDrawAlpha_Premultiplied,
        PrColorSpace_Srgb,
    };
    const PrDrawImage* image = nullptr;
    REQUIRE(prDrawImageCreate(&target, &image) == PrStatus_Ok);
    PrDrawSource source = prDrawSourceImage(image, PrRect{0, 0, 2, 2});
    prDrawListClear(list);
    PrDrawBrush brush = prDrawBrushImage(source, PrRect{0, 0, 32, 32}, PrDrawSampling_Nearest);
    prDrawListRect(list, PrRect{0, 0, 32, 32}, {.brush = &brush});
    std::vector<uint8_t> drawn = h.render(list);
    uint64_t withImage = h.stats().uploadBytes;
    CHECK(at(drawn, 32, 4, 4)[0] == 255);
    CHECK(at(drawn, 32, 28, 4)[1] == 255);
    CHECK(at(drawn, 32, 4, 28)[2] == 255);
    h.render(list);
    CHECK(h.stats().uploadBytes < withImage);
    prDrawImageRelease(image);
    prDrawListRelease(list);
}

TEST_CASE("triangles draws glyphs from its atlas, covering what the cpu backend covers") {
    Device h(128, 64);
    const PrFont* font = load("NotoSans[wdth,wght].ttf");
    PrDrawList* list = prDrawListCreate();
    addText(list, font, 32, "Hxg", PrVec2{8.3f, 44});
    std::vector<uint8_t> drawn = h.render(list);
    std::vector<uint8_t> expected = cpuRender(list, 128, 64);
    double ink = inked(expected);
    CHECK(ink > 200);
    CHECK(inked(drawn) == doctest::Approx(ink).epsilon(0.02));
    uint64_t withGlyphs = h.stats().uploadBytes;
    // The glyphs rasterized into the atlas are uploaded once. When they are drawn again, they need no update.
    h.render(list);
    CHECK(h.stats().uploadBytes < withGlyphs);
    // Everything shares the atlas's texture, and therefore one draw.
    prDrawListRect(list, PrRect{0, 0, 4, 4}, {.color = PrColor{0, 0, 0, 1}});
    h.render(list);
    CHECK(h.stats().draws == 1);

    // Rotated glyphs are filled as paths.
    prDrawListClear(list);
    prDrawListPushTransformAffine2(list, prAffine2Concat(prAffine2Rotate(0.2f), prAffine2Translate(PrVec2{10, 0})));
    addText(list, font, 24, "Hxg", PrVec2{8, 40});
    prDrawListPopTransform(list);
    drawn = h.render(list);
    expected = cpuRender(list, 128, 64);
    CHECK(inked(drawn) == doctest::Approx(inked(expected)).epsilon(0.02));
    prDrawListRelease(list);
    prFontRelease(font);
}

TEST_CASE("triangles draws a drop shadow with about the cpu backend's coverage") {
    Device h(96, 96);
    PrDrawList* list = prDrawListCreate();
    PrDrawShadow shadow{};
    shadow.shape.rect = PrRect{24, 24, 72, 72};
    for (PrCorner& corner : shadow.shape.corners)
        corner = PrCorner{{8, 8}, 1};
    shadow.offset = PrVec2{0, 4};
    shadow.sigma = 6;
    prDrawListDropShadow(list, &shadow, {.color = PrColor{0, 0, 0, 1}});
    std::vector<uint8_t> drawn = h.render(list);
    std::vector<uint8_t> expected = cpuRender(list, 96, 96);
    // The cpu backend cuts the shadow to outside the caster, so only the ink outside the caster is compared.
    PrDrawList* caster = prDrawListCreate();
    prDrawListCornerRect(caster, shadow.shape, {.color = PrColor{0, 0, 0, 1}});
    std::vector<uint8_t> cut = cpuRender(caster, 96, 96);
    double outside = 0;
    for (size_t i = 3; i < drawn.size(); i += 4)
        outside += drawn[i] / 255.0 * (1 - cut[i] / 255.0);
    CHECK(outside == doctest::Approx(inked(expected)).epsilon(0.05));
    CHECK(at(drawn, 96, 48, 48)[3] == 255);
    prDrawListRelease(caster);

    prDrawListClear(list);
    prDrawListInnerShadow(list, &shadow, {.color = PrColor{0, 0, 0, 1}});
    drawn = h.render(list);
    expected = cpuRender(list, 96, 96);
    // The band has no tail. The blur's tail would fall far into the opening.
    CHECK(inked(drawn) == doctest::Approx(inked(expected)).epsilon(0.2));
    CHECK(at(drawn, 96, 10, 10)[3] == 0);
    prDrawListRelease(list);
}

TEST_CASE("the triangles header compiles as C") {
    CHECK(prTestDrawTrianglesCApi() == 0);
}
