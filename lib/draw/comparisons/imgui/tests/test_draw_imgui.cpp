#include "pr/array.h"
#include "pr/draw_cpu.h"
#include "pr/draw_imgui.h"
#include "pr/draw_list.h"
#include "pr/gpu.h"
#include "pr/string.h"
#include "pr/vec.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

using namespace pr;

struct Device {
    PrGpuDevice* gpuDevice = nullptr;
    PrDrawRenderer* renderer = nullptr;
    PrGpuTexture texture{};
    PrDrawFrame* frame = nullptr;

    Device(uint32_t width, uint32_t height) {
        REQUIRE(prGpuDeviceCreate(PrGpuBackend_Default, nullptr, &gpuDevice) == PrStatus_Ok);
        REQUIRE(prDrawImGuiCreate(gpuDevice, &renderer));
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
    Array<uint8_t> render(const PrDrawList* list) {
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
        Array<uint8_t> pixels(size_t(width) * height * 4);
        PrPixels out{pixels.data(), size_t(width) * 4, {width, height}};
        prGpuTextureRead(gpuDevice, texture, PrGpuAccessFlag_ShaderRead, PrIVec2{0, 0}, out);
        return pixels;
    }
};

const PrFont* load(const char* name) {
    String path = format("{}/{}", PR_FONTS_DIR, name);
    const PrFont* font = nullptr;
    REQUIRE(prFontCreateFromFile(path, 0, nullptr, &font) == PrStatus_Ok);
    return font;
}

// Returns the list rendered by the cpu backend onto transparent pixels, which the test compares Dear ImGui's render
// with.
Array<uint8_t> cpuRender(const PrDrawList* list, uint32_t width, uint32_t height) {
    Array<uint8_t> pixels(size_t(width) * height * 4);
    PrDrawRenderer* renderer = prDrawCpuCreate(nullptr);
    PrPixels into{pixels.data(), size_t(width) * 4, {width, height}};
    REQUIRE(prDrawRendererRenderPixels(renderer, list, into, PrPixelFormat_Rgba8) == PrStatus_Ok);
    prDrawRendererDestroy(renderer);
    return pixels;
}

// Returns the total coverage of the pixels: the sum of their alphas, in whole pixels.
double inked(const Array<uint8_t>& pixels) {
    double sum = 0;
    for (uint32_t i = 3; i < pixels.count(); i += 4)
        sum += pixels[i] / 255.0;
    return sum;
}

// Records a run of the text's glyphs, one for each character, advanced by the font's advances.
void addText(PrDrawList* list, const PrFont* font, float size, const char* text, PrVec2 at,
        const PrDrawStroke* stroke = nullptr) {
    Array<PrFontGlyph> glyphs;
    for (const char* c = text; *c != 0; ++c) {
        uint16_t id = prFontCodePointGlyph(font, uint32_t(*c));
        glyphs.push(PrFontGlyph{id, at});
        at.x += prFontGlyphAdvance(font, id, 0) * size;
    }
    PrFontGlyphRun run{};
    run.font = font;
    run.size = size;
    run.glyphs = glyphs.data();
    run.glyphCount = glyphs.count();
    prDrawListGlyphs(list, &run, PrVec2{}, {.color = PrColor{0, 0, 0, 1}, .stroke = stroke});
}

const uint8_t* at(const Array<uint8_t>& pixels, uint32_t width, uint32_t x, uint32_t y) {
    return pixels.data() + (size_t(y) * width + x) * 4;
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

TEST_CASE("imgui renders on the device") {
    Device device(16, 16);
    CHECK(prDrawRendererDevice(device.renderer) == device.gpuDevice);
}

TEST_CASE("imgui fills a rect over a cleared texture, and blends a translucent one over it") {
    Device device(16, 16);
    PrDrawList* list = prDrawListCreate();
    prDrawListClearRect(list, PrRect{0, 0, 64, 64}, PrColor{1, 1, 1, 1});
    prDrawListRect(list, PrRect{4, 4, 12, 12}, {.color = PrColor{1, 0, 0, 1}});
    prDrawListRect(list, PrRect{8, 0, 16, 16}, {.color = PrColor{0, 0, 0, 0.5f}});
    Array<uint8_t> pixels = device.render(list);
    const uint8_t* paper = at(pixels, 16, 1, 1);
    CHECK(paper[0] == 255);
    CHECK(paper[1] == 255);
    CHECK(paper[3] == 255);
    const uint8_t* red = at(pixels, 16, 5, 5);
    CHECK(red[0] == 255);
    CHECK(red[1] == 0);
    CHECK(red[3] == 255);
    // Half black over red and over white.
    const uint8_t* darkened = at(pixels, 16, 10, 10);
    CHECK(darkened[0] >= 126);
    CHECK(darkened[0] <= 129);
    CHECK(darkened[1] == 0);
    const uint8_t* gray = at(pixels, 16, 14, 1);
    CHECK(gray[1] >= 126);
    CHECK(gray[1] <= 129);
    CHECK(gray[3] == 255);
    prDrawListRelease(list);
}

TEST_CASE("imgui draws shaped glyphs from its atlas, covering what the cpu backend covers") {
    Device device(128, 64);
    const PrFont* font = load("NotoSans[wdth,wght].ttf");
    PrDrawList* list = prDrawListCreate();
    addText(list, font, 32, "Hxg", PrVec2{8, 44});
    Array<uint8_t> drawn = device.render(list);
    Array<uint8_t> expected = cpuRender(list, 128, 64);
    double ink = inked(expected);
    CHECK(ink > 200);
    CHECK(inked(drawn) == doctest::Approx(ink).epsilon(0.03));
    // The left stem of the H is solid in both.
    CHECK(at(drawn, 128, 12, 30)[3] == 255);
    CHECK(at(expected, 128, 12, 30)[3] == 255);

    // A later render without the font drops it from the atlas, and a later render with the font adds it back.
    prDrawListClear(list);
    device.render(list);
    addText(list, font, 32, "Hxg", PrVec2{8, 44});
    CHECK(inked(device.render(list)) == doctest::Approx(ink).epsilon(0.03));

    // Stroked glyphs are left out.
    prDrawListClear(list);
    PrDrawStroke stroke = prDrawStrokeDefault();
    stroke.shape.width = 1;
    addText(list, font, 32, "Hxg", PrVec2{8, 44}, &stroke);
    CHECK(inked(device.render(list)) == 0);
    prDrawListRelease(list);
    prFontRelease(font);
}

TEST_CASE("imgui strokes a closed contour centered, inside or outside, as the cpu backend does") {
    const PrDrawStrokeAlign aligns[] = {PrDrawStrokeAlign_Center, PrDrawStrokeAlign_Inside, PrDrawStrokeAlign_Outside};
    const bool reverses[] = {false, true};
    for (PrDrawStrokeAlign align : aligns) {
        CAPTURE(int(align));
        // Clockwise and counter-clockwise contours have the same inside.
        for (bool reversed : reverses) {
            CAPTURE(reversed);
            Device device(64, 64);
            PrDrawList* list = prDrawListCreate();
            PrPath* path = prPathCreate();
            PrVec2 corners[4] = {{16, 16}, {48, 16}, {48, 48}, {16, 48}};
            prPathMoveTo(path, corners[0]);
            for (uint32_t i = 1; i < 4; ++i)
                prPathLineTo(path, corners[reversed ? 4 - i : i]);
            prPathClose(path);
            PrDrawStroke stroke = prDrawStrokeDefault();
            stroke.shape.width = 4;
            stroke.align = align;
            prDrawListPath(list, path, PrPathFillRule_NonZero, {.color = PrColor{0, 0, 0, 1}, .stroke = &stroke});
            Array<uint8_t> drawn = device.render(list);
            Array<uint8_t> expected = cpuRender(list, 64, 64);
            // The tangent misses the projected near edge by most of a pixel, which loses that edge's last row of ink.
            CHECK(inked(drawn) == doctest::Approx(inked(expected)).epsilon(0.06));
            // A pixel of the stroke on the left side. Outside is to its left, and inside is to its right.
            uint32_t x = align == PrDrawStrokeAlign_Inside ? 17 : align == PrDrawStrokeAlign_Outside ? 14 : 16;
            CHECK(at(drawn, 64, x, 32)[3] == 255);
            CHECK(at(expected, 64, x, 32)[3] == 255);
            prPathDestroy(path);
            prDrawListRelease(list);
        }
    }
}

TEST_CASE("imgui draws a variable font's instance as its own font") {
    Device device(128, 64);
    const PrFont* regular = load("NotoSans[wdth,wght].ttf");
    PrFontVariation heavy{0x77676874, 900}; // 'wght'
    PrFontDerivation derivation = prFontDerivationDefault();
    derivation.variations = &heavy;
    derivation.variationCount = 1;
    const PrFont* bold = prFontCreateDerived(regular, &derivation);
    PrDrawList* list = prDrawListCreate();
    addText(list, regular, 32, "Hxg", PrVec2{8, 44});
    double light = inked(device.render(list));
    prDrawListClear(list);
    addText(list, bold, 32, "Hxg", PrVec2{8, 44});
    double black = inked(device.render(list));
    CHECK(black > light * 1.3);
    CHECK(black == doctest::Approx(inked(cpuRender(list, 128, 64))).epsilon(0.05));
    prDrawListRelease(list);
    prFontRelease(bold);
    prFontRelease(regular);
}

TEST_CASE("imgui draws a projected group's contents through its projection's tangent, where cpu projects them") {
    Device device(64, 64);
    PrDrawList* list = prDrawListCreate();
    // A light tilt, which the tangent approximates within a fraction of a pixel.
    recordTilted(list, 0.35f, 300);
    Array<uint8_t> drawn = device.render(list);
    Array<uint8_t> expected = cpuRender(list, 64, 64);
    // The tangent misses the projected near edge by most of a pixel, which loses that edge's last row of ink.
    CHECK(inked(drawn) == doctest::Approx(inked(expected)).epsilon(0.06));
    CHECK(at(drawn, 64, 32, 32)[0] == at(expected, 64, 32, 32)[0]);
    // An edge-on plane draws nothing.
    prDrawListClear(list);
    recordTilted(list, 1.5707964f, 300);
    CHECK(inked(device.render(list)) == 0);
    prDrawListRelease(list);
}
