// The Canvas 2D renderer's rendering, in a browser: each scene renders through the browser's 2D context and through the
// CPU renderer, on the same Canvas 2D device, and the two must agree within the differences of their rasterizers.

#include "pr/draw_canvas.h"
#include "pr/draw_cpu.h"
#include "pr/draw_filter.h"
#include "pr/draw_image.h"
#include "pr/draw_list.h"
#include "pr/font.h"
#include "pr/gpu.h"
#include "pr/path.h"

#include "tools/harness/test.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace pr_impl {

namespace {

// How a scene's two renders differ.
struct Difference {
    double canvasInk;       // the sum of the canvas render's alpha, in pixels
    double cpuInk;          // the same of the CPU render
    double differing;       // the fraction of pixels whose premultiplied channels differ by more than the tolerance
};

// A Canvas 2D device, and the Canvas 2D and CPU renderers on it.
struct Renderers {
    PrGpuDevice* device = nullptr;
    PrDrawRenderer* canvas = nullptr;
    PrDrawRenderer* cpu = nullptr;

    Renderers() {
        REQUIRE(prGpuDeviceCreate(PrGpuBackend_Canvas2d, nullptr, &device) == PrStatus_Ok);
        REQUIRE(prDrawCanvasCreate(device, nullptr, &canvas) == PrStatus_Ok);
        PrDrawCpuOptions options = prDrawCpuOptionsDefault();
        options.device = device;
        cpu = prDrawCpuCreate(&options);
        REQUIRE(cpu != nullptr);
    }

    ~Renderers() {
        prDrawRendererDestroy(cpu);
        prDrawRendererDestroy(canvas);
        prGpuDeviceDestroy(device);
    }

    // Renders the list with `renderer` over a transparent texture of `extent`, and reads the texture back.
    std::vector<uint8_t> render(PrDrawRenderer* renderer, const PrDrawList* list, PrUVec2 extent) {
        PrGpuTextureDesc desc = prGpuTextureDesc2d(PrGpuFormat_Rgba8, extent,
            PrGpuTextureUsageFlag_Sampled | PrGpuTextureUsageFlag_RenderTarget);
        PrGpuTexture texture{};
        REQUIRE(prGpuTextureCreate(device, &desc, &texture) == PrStatus_Ok);
        PrGpuCommands* commands = nullptr;
        REQUIRE(prGpuCommandsBegin(device, nullptr, &commands));
        PrDrawFrame* frame = prDrawFrameCreate(renderer);
        PrDrawTarget target{.extent = extent, .format = PrPixelFormat_Rgba8, .commands = commands};
        REQUIRE(prDrawFramePrepare(frame, list, &target) == PrStatus_Ok);
        PrGpuSubresources all{};
        prGpuCommandsTextureBarrier(commands, texture, all, PrGpuAccessFlag_ShaderRead,
            PrGpuAccessFlag_ColorAttachment);
        PrGpuPassDesc pass = prGpuPassDescColor(texture, PrGpuLoad_Clear, PrColor{0, 0, 0, 0});
        PrGpuPass* encoder = prGpuPassBegin(commands, &pass);
        prDrawFrameRender(frame, encoder, PrIVec2{0, 0});
        prGpuPassEnd(encoder);
        prGpuCommandsTextureBarrier(commands, texture, all, PrGpuAccessFlag_ColorAttachment,
            PrGpuAccessFlag_ShaderRead);
        prGpuCommandsSubmitAndWait(commands);
        std::vector<uint8_t> pixels(size_t(extent.x) * extent.y * 4);
        prGpuTextureRead(device, texture, PrGpuAccessFlag_ShaderRead, PrIVec2{0, 0},
            PrPixels{pixels.data(), size_t(extent.x) * 4, extent});
        prDrawFrameDestroy(frame);
        prGpuTextureDestroy(device, texture);
        return pixels;
    }

    // Renders the list with both renderers and measures how they differ, counting a pixel as different if a channel
    // differs by more than `tolerance`.
    Difference compare(const PrDrawList* list, PrUVec2 extent, int tolerance) {
        std::vector<uint8_t> drawn = render(canvas, list, extent);
        std::vector<uint8_t> expected = render(cpu, list, extent);
        Difference d{};
        uint64_t differing = 0;
        for (size_t i = 0; i < drawn.size(); i += 4) {
            d.canvasInk += drawn[i + 3] / 255.0;
            d.cpuInk += expected[i + 3] / 255.0;
            for (size_t k = 0; k < 4; ++k) {
                if (std::abs(int(drawn[i + k]) - int(expected[i + k])) > tolerance) {
                    ++differing;
                    break;
                }
            }
        }
        d.differing = double(differing) / (double(extent.x) * extent.y);
        return d;
    }
};

// Checks that a scene's renders agree: their ink within `inkEpsilon` of each other, as a fraction, and at most
// `differing` of the pixels differing.
void agree(const Difference& d, double inkEpsilon, double differing) {
    CHECK(d.canvasInk == doctest::Approx(d.cpuInk).epsilon(inkEpsilon));
    CHECK(d.differing <= differing);
}

// The page packages the font under /fonts (pr_page_tests).
const PrFont* load(const char* name) {
    std::string path = std::string("/fonts/") + name;
    const PrFont* font = nullptr;
    REQUIRE(prFontCreateFromFile(path, 0, nullptr, &font) == PrStatus_Ok);
    return font;
}

// The colors of a hue wheel, as seven stops from red back to red.
void wheelStops(PrDrawColorStop (&out)[7]) {
    for (uint32_t i = 0; i < 7; ++i) {
        float hue = float(i % 6) / 6;
        auto clamp = [](float v) { return v < 0 ? 0 : v > 1 ? 1 : v; };
        out[i] = PrDrawColorStop{
            .offset = float(i) / 6,
            .color = PrColor{
                clamp(std::fabs(hue * 6 - 3) - 1),
                clamp(2 - std::fabs(hue * 6 - 2)),
                clamp(2 - std::fabs(hue * 6 - 4)), 1,
            },
        };
    }
}

// Returns an image of 4 by 4 texels: a checkerboard of opaque red and half-transparent blue.
const PrDrawImage* checkerImage() {
    static uint8_t texels[4 * 4 * 4];
    for (uint32_t i = 0; i < 16; ++i) {
        bool red = ((i % 4) + (i / 4)) % 2 == 0;
        uint8_t* t = texels + i * 4;
        t[0] = red ? 255 : 0;
        t[1] = 0;
        t[2] = red ? 0 : 128;
        t[3] = red ? 255 : 128;
    }
    PrDrawImageDesc desc{
        PrPixelView{texels, 16, {4, 4}}, PrPixelFormat_Rgba8, PrDrawAlpha_Premultiplied,
        PrColorSpace_Srgb,
    };
    const PrDrawImage* image = nullptr;
    REQUIRE(prDrawImageCreate(&desc, &image) == PrStatus_Ok);
    return image;
}

// Returns a filter of one node over the group's bounds.
const PrDrawFilter* filterOf(const PrDrawFilterNode& node) {
    PrDrawFilterBuilder* builder = prDrawFilterBuilderCreate();
    prDrawFilterBuilderAdd(builder, &node);
    const PrDrawFilter* filter = prDrawFilterCreate(builder);
    prDrawFilterBuilderDestroy(builder);
    return filter;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("the canvas renderer fills shapes as the cpu renderer does") {
    Renderers r;
    PrDrawList* list = prDrawListCreate();
    prDrawListRect(list, PrRect{4.25f, 6.5f, 40.75f, 30}, {.color = PrColor{1, 0, 0, 1}});
    prDrawListEllipse(list, PrVec2{70, 32}, PrVec2{22, 14}, 0.3f, {.color = PrColor{0, 0, 1, 0.8f}});
    prDrawListCornerRect(list, prCornerRectRound(PrRect{6, 36, 44, 60}, 8), {.color = PrColor{0, 0.5f, 0, 1}});
    prDrawListCapsule(list, PrVec2{100, 10}, PrVec2{120, 54}, 6, {.color = PrColor{0.5f, 0, 0.5f, 1}});
    agree(r.compare(list, PrUVec2{128, 64}, 32), 0.01, 0.01);
    prDrawListRelease(list);
}

TEST_CASE("the canvas renderer fills a path by its rule") {
    Renderers r;
    PrDrawList* list = prDrawListCreate();
    PrPath* path = prPathCreate();
    prPathAddRect(path, PrRect{8, 8, 56, 56});
    prPathAddEllipse(path, PrVec2{32, 32}, PrVec2{12, 12}, 0);
    prPathMoveTo(path, PrVec2{70, 10});
    prPathCubicTo(path, PrVec2{130, 0}, PrVec2{60, 70}, PrVec2{120, 56});
    prPathClose(path);
    for (PrPathFillRule rule : {PrPathFillRule_EvenOdd, PrPathFillRule_NonZero}) {
        CAPTURE(int(rule));
        prDrawListClear(list);
        prDrawListPath(list, path, rule, {.color = PrColor{0, 0, 0, 1}});
        agree(r.compare(list, PrUVec2{128, 64}, 32), 0.01, 0.01);
    }
    prPathDestroy(path);
    prDrawListRelease(list);
}

TEST_CASE("the canvas renderer strokes with each join, cap and dash") {
    Renderers r;
    PrDrawList* list = prDrawListCreate();
    PrPath* path = prPathCreate();
    prPathMoveTo(path, PrVec2{10, 50});
    prPathLineTo(path, PrVec2{30, 10});
    prPathLineTo(path, PrVec2{50, 50});
    prPathCubicTo(path, PrVec2{70, 0}, PrVec2{100, 70}, PrVec2{118, 20});
    const float dashes[2] = {9, 4};
    const PrPathJoin joins[3] = {PrPathJoin_Miter, PrPathJoin_Round, PrPathJoin_Bevel};
    const PrPathCap caps[3] = {PrPathCap_Butt, PrPathCap_Round, PrPathCap_Square};
    for (uint32_t i = 0; i < 4; ++i) {
        CAPTURE(i);
        prDrawListClear(list);
        PrDrawStroke stroke = prDrawStrokeDefault();
        stroke.shape.width = 6;
        stroke.shape.join = joins[i % 3];
        stroke.shape.startCap = caps[i % 3];
        stroke.shape.endCap = caps[i % 3];
        if (i == 3) {
            stroke.shape.dashes = dashes;
            stroke.shape.dashCount = 2;
        }
        prDrawListPath(list, path, PrPathFillRule_NonZero, {.color = PrColor{0, 0, 0, 1}, .stroke = &stroke});
        agree(r.compare(list, PrUVec2{128, 64}, 48), 0.02, 0.02);
    }
    prPathDestroy(path);
    prDrawListRelease(list);
}

TEST_CASE("the canvas renderer paints each kind of gradient as the cpu renderer does") {
    PrDrawColorStop wheel[7];
    wheelStops(wheel);
    PrDrawColorStop hard[4] = {
        {.offset = 0, .color = PrColor{1, 0, 0, 1}},
        {.offset = 0.5f, .color = PrColor{1, 0, 0, 1}},
        {.offset = 0.5f, .color = PrColor{0, 0, 1, 1}},
        {.offset = 1, .color = PrColor{0, 0, 1, 1}},
    };
    PrDrawColorStop fade[2] = {
        {.offset = 0, .color = PrColor{1, 0, 0, 1}},
        {.offset = 1, .color = PrColor{0, 0, 1, 0}},
    };
    struct Case {
        PrDrawBrushKind kind;
        PrDrawExtend extend;
        const PrDrawColorStop* stops;
        uint32_t count;
    };
    const Case cases[] = {
        {PrDrawBrushKind_Linear, PrDrawExtend_Pad, fade, 2},
        {PrDrawBrushKind_Linear, PrDrawExtend_Repeat, wheel, 7},
        {PrDrawBrushKind_Linear, PrDrawExtend_Reflect, hard, 4},
        {PrDrawBrushKind_Radial, PrDrawExtend_Repeat, wheel, 7},
        {PrDrawBrushKind_Conic, PrDrawExtend_Pad, wheel, 7},
        {PrDrawBrushKind_Diamond, PrDrawExtend_Reflect, wheel, 7},
    };
    Renderers r;
    for (const Case& k : cases) {
        CAPTURE(int(k.kind));
        CAPTURE(int(k.extend));
        PrDrawList* list = prDrawListCreate();
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
        // A ramp whose alpha changes mixes unpremultiplied in the context, through stops that come close to the
        // premultiplied mix, so its ink differs a little.
        agree(r.compare(list, PrUVec2{100, 100}, 16), 0.02, 0.03);
        prDrawListRelease(list);
    }
}

TEST_CASE("the canvas renderer draws images, textures and lists as sources") {
    Renderers r;
    const PrDrawImage* image = checkerImage();
    PrGpuTextureDesc desc = prGpuTextureDesc2d(PrGpuFormat_Rgba8, PrUVec2{4, 4}, PrGpuTextureUsageFlag_Sampled);
    PrGpuTexture texture{};
    REQUIRE(prGpuTextureCreate(r.device, &desc, &texture) == PrStatus_Ok);
    PrDrawList* inner = prDrawListCreate();
    prDrawListEllipse(inner, PrVec2{8, 8}, PrVec2{7, 5}, 0.5f, {.color = PrColor{0, 0.6f, 0.2f, 1}});
    prDrawListFreeze(inner);
    // The texture holds the image's texels.
    std::vector<uint8_t> texels(64);
    for (uint32_t i = 0; i < 16; ++i) {
        bool red = ((i % 4) + (i / 4)) % 2 == 0;
        texels[i * 4] = red ? 255 : 0;
        texels[i * 4 + 2] = red ? 0 : 128;
        texels[i * 4 + 3] = red ? 255 : 128;
    }
    prGpuTextureWrite(r.device, texture, PrIVec2{0, 0}, PrPixelView{texels.data(), 16, {4, 4}});
    const PrDrawSource sources[3] = {
        prDrawSourceImage(image, PrRect{0, 0, 4, 4}),
        prDrawSourceTexture(prDrawGpuTexture(texture), PrUVec2{4, 4}, PrRect{0, 0, 4, 4}, PrDrawAlpha_Premultiplied),
        prDrawSourceList(inner, PrRect{0, 0, 16, 16}),
    };
    for (uint32_t s = 0; s < 3; ++s) {
        for (PrDrawSampling sampling : {PrDrawSampling_Nearest, PrDrawSampling_Linear}) {
            CAPTURE(s);
            CAPTURE(int(sampling));
            PrDrawList* list = prDrawListCreate();
            PrDrawBrush brush = prDrawBrushImage(sources[s], PrRect{8, 8, 40, 40}, sampling);
            prDrawListRect(list, PrRect{8, 8, 40, 40}, {.brush = &brush});
            // The same source repeats across a turned rectangle.
            brush.image.tiles.x = PrDrawExtend_Repeat;
            brush.image.tiles.y = PrDrawExtend_Repeat;
            brush.transform = prAffine2Rotate(0.2f);
            prDrawListRect(list, PrRect{48, 4, 124, 60}, {.brush = &brush});
            agree(r.compare(list, PrUVec2{128, 64}, 48), 0.02, 0.04);
            prDrawListRelease(list);
        }
    }
    prDrawListRelease(inner);
    prGpuTextureDestroy(r.device, texture);
    prDrawImageRelease(image);
}

TEST_CASE("the canvas renderer clips to rects and paths, nested") {
    Renderers r;
    PrDrawList* list = prDrawListCreate();
    // An ellipse, and inside it a rect in a rotated space.
    PrPath* path = prPathCreate();
    prPathAddEllipse(path, PrVec2{48, 32}, PrVec2{30, 20}, 0);
    prDrawListPushClipPath(list, path, PrPathFillRule_NonZero, PrDrawEdges_Antialiased);
    prDrawListPushTransformAffine2(list, prAffine2Concat(prAffine2Rotate(0.4f), prAffine2Translate(PrVec2{40, 4})));
    prDrawListPushClipRect(list, PrRect{0, 0, 40, 30}, PrDrawEdges_Antialiased);
    prDrawListRect(list, PrRect{0, 0, 128, 64}, {.color = PrColor{0, 0, 0, 1}});
    prDrawListPopClip(list);
    prDrawListPopTransform(list);
    prDrawListPopClip(list);
    prDrawListPushClipCornerRect(list, prCornerRectRound(PrRect{90, 8, 124, 56}, 10), PrDrawEdges_Antialiased);
    prDrawListRect(list, PrRect{0, 0, 128, 64}, {.color = PrColor{0.8f, 0.2f, 0, 1}});
    prDrawListPopClip(list);
    agree(r.compare(list, PrUVec2{128, 64}, 48), 0.02, 0.02);
    prPathDestroy(path);
    prDrawListRelease(list);
}

TEST_CASE("the canvas renderer composites groups with opacity and blend modes") {
    Renderers r;
    PrDrawList* list = prDrawListCreate();
    prDrawListRect(list, PrRect{0, 0, 128, 32}, {.color = PrColor{0.9f, 0.7f, 0.1f, 1}});
    PrDrawGroup group = prDrawGroupDefault();
    group.opacity = 0.6f;
    prDrawListPushGroup(list, &group);
    prDrawListRect(list, PrRect{8, 8, 48, 56}, {.color = PrColor{0, 0, 1, 1}});
    prDrawListRect(list, PrRect{28, 16, 68, 48}, {.color = PrColor{1, 0, 0, 1}});
    prDrawListPopGroup(list);
    group = prDrawGroupDefault();
    group.blendMode = PrDrawBlendMode_Multiply;
    prDrawListPushGroup(list, &group);
    prDrawListEllipse(list, PrVec2{100, 32}, PrVec2{22, 22}, 0, {.color = PrColor{0.2f, 0.6f, 1, 1}});
    prDrawListPopGroup(list);
    prDrawListRect(list, PrRect{80, 40, 124, 60}, {
        .color = PrColor{0, 1, 0, 0.7f},
        .blendMode = PrDrawBlendMode_Screen,
    });
    agree(r.compare(list, PrUVec2{128, 64}, 32), 0.01, 0.01);
    prDrawListRelease(list);
}

TEST_CASE("the canvas renderer filters a group with blurs and drop shadows") {
    Renderers r;
    PrDrawFilterNode blur{};
    blur.kind = PrDrawFilterKind_Blur;
    blur.inputs[0] = PrDrawFilterInput_Source;
    blur.blur.sigma = PrVec2{3, 3};
    PrDrawFilterNode shadow{};
    shadow.kind = PrDrawFilterKind_DropShadow;
    shadow.inputs[0] = PrDrawFilterInput_Source;
    shadow.shadow = PrDrawFilterShadow{.offset = PrVec2{4, 5}, .sigma = PrVec2{3, 3}, .color = PrColor{0, 0, 0, 0.6f}};
    for (const PrDrawFilterNode* node : {&blur, &shadow}) {
        CAPTURE(int(node->kind));
        const PrDrawFilter* filter = filterOf(*node);
        PrDrawList* list = prDrawListCreate();
        PrDrawGroup group = prDrawGroupDefault();
        group.bounds = PrRect{0, 0, 128, 64};
        group.filter = filter;
        prDrawListPushGroup(list, &group);
        prDrawListCornerRect(list, prCornerRectRound(PrRect{20, 14, 70, 44}, 6), {.color = PrColor{0.1f, 0.4f, 0.9f, 1}});
        prDrawListPopGroup(list);
        agree(r.compare(list, PrUVec2{128, 64}, 16), 0.03, 0.05);
        prDrawListRelease(list);
        prDrawFilterRelease(filter);
    }
}

TEST_CASE("the canvas renderer casts drop and inner shadows") {
    Renderers r;
    PrDrawList* list = prDrawListCreate();
    PrDrawShadow drop{.shape = prCornerRectRound(PrRect{10, 10, 54, 50}, 8), .offset = PrVec2{3, 4}, .sigma = 4};
    prDrawListDropShadow(list, &drop, {.color = PrColor{0, 0, 0, 0.7f}});
    PrDrawShadow inner{
        .shape = prCornerRectRound(PrRect{72, 10, 118, 50}, 8),
        .offset = PrVec2{2, 3},
        .sigma = 3,
        .spread = 1,
    };
    prDrawListInnerShadow(list, &inner, {.color = PrColor{0.5f, 0, 0, 0.8f}});
    // A box's shadow may approximate its blur for speed. The context blurs with a Gaussian, and the CPU renderer's
    // faster shadow is a little denser in its tail, so their ink differs by several percent while few pixels differ
    // much.
    agree(r.compare(list, PrUVec2{128, 64}, 16), 0.1, 0.02);
    prDrawListRelease(list);
}

TEST_CASE("the canvas renderer masks a group by alpha and by luminance") {
    Renderers r;
    PrDrawColorStop stops[2] = {
        {.offset = 0, .color = PrColor{1, 1, 1, 1}},
        {.offset = 1, .color = PrColor{0, 0, 0, 0}},
    };
    for (PrDrawMaskMode mode : {PrDrawMaskMode_Alpha, PrDrawMaskMode_Luminance}) {
        for (bool invert : {false, true}) {
            CAPTURE(int(mode));
            CAPTURE(invert);
            PrDrawList* masking = prDrawListCreate();
            PrDrawBrush fade = prDrawBrushLinear(PrVec2{0, 0}, PrVec2{128, 0}, stops, 2);
            prDrawListEllipse(masking, PrVec2{64, 32}, PrVec2{56, 26}, 0, {.brush = &fade});
            prDrawListFreeze(masking);
            PrDrawMask mask = prDrawMaskFromList(masking, PrRect{0, 0, 128, 64}, mode, PrColorSpace_Srgb);
            mask.invert = invert;
            PrDrawList* list = prDrawListCreate();
            PrDrawGroup group = prDrawGroupDefault();
            group.bounds = PrRect{0, 0, 128, 64};
            group.mask = &mask;
            prDrawListPushGroup(list, &group);
            prDrawListRect(list, PrRect{0, 0, 128, 64}, {.color = PrColor{0.2f, 0.3f, 0.9f, 1}});
            prDrawListPopGroup(list);
            agree(r.compare(list, PrUVec2{128, 64}, 16), 0.02, 0.03);
            prDrawListRelease(list);
            prDrawListRelease(masking);
        }
    }
}

TEST_CASE("the canvas renderer draws glyphs, filled and stroked") {
    Renderers r;
    const PrFont* font = load("NotoSans[wdth,wght].ttf");
    PrDrawStroke stroke = prDrawStrokeDefault();
    stroke.shape.width = 1.5f;
    const PrDrawStroke* outlines[2] = {nullptr, &stroke};
    for (const PrDrawStroke* outline : outlines) {
        CAPTURE(outline != nullptr);
        PrDrawList* list = prDrawListCreate();
        std::vector<PrFontGlyph> glyphs;
        PrVec2 pen{6, 46};
        for (const char* c = "Hxg&Q"; *c != 0; ++c) {
            uint16_t id = prFontCodePointGlyph(font, uint32_t(*c));
            glyphs.push_back(PrFontGlyph{id, pen});
            pen.x += prFontGlyphAdvance(font, id, 0) * 32;
        }
        PrFontGlyphRun run{};
        run.font = font;
        run.size = 32;
        run.glyphs = glyphs.data();
        run.glyphCount = uint32_t(glyphs.size());
        prDrawListGlyphs(list, &run, PrVec2{}, {.color = PrColor{0, 0, 0, 1}, .stroke = outline});
        agree(r.compare(list, PrUVec2{128, 64}, 64), 0.03, 0.04);
        prDrawListRelease(list);
    }
    prFontRelease(font);
}
