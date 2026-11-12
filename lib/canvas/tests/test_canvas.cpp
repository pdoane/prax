// The context's state, its drawing and its pixels, rendered on the CPU.

#include "pr/canvas.h"
#include "pr/draw_cpu.h"
#include "pr/draw_renderer.h"
#include "pr/gpu.h"
#include "pr/string_ops.h"

#include "tools/harness/test.h"

#include <cmath>
#include <cstdlib>
#include <string>

extern "C" int prTestCanvasCApi(void);

namespace pr_impl {

namespace {

// A context on a memory device. Its pixels read as straight-alpha sRGB bytes.
struct MemoryCanvas {
    PrGpuDevice* device = nullptr;
    PrCanvas* handle;

    explicit MemoryCanvas(uint32_t width = 100, uint32_t height = 50,
            PrCanvasSettings settings = prCanvasSettingsDefault()) {
        REQUIRE(prGpuDeviceCreate(PrGpuBackend_Memory, nullptr, &device) == PrStatus_Ok);
        PrCanvasDesc desc{.extent = PrUVec2{width, height}, .settings = settings, .device = device};
        handle = prCanvasCreate(&desc);
    }
    ~MemoryCanvas() {
        prCanvasDestroy(handle);
        prGpuDeviceDestroy(device);
    }

    // Returns the pixel at (x, y) as "r,g,b,a".
    std::string pixel(int32_t x, int32_t y) {
        uint8_t bytes[4];
        PrCanvasImageData data{
            .extent = PrUVec2{1, 1},
            .format = PrCanvasPixelFormat_RgbaUnorm8,
            .colorSpace = PrColorSpace_Srgb,
        };
        prCanvasGetImageData(handle, PrIVec2{x, y}, &data, bytes);
        return std::to_string(bytes[0]) + "," + std::to_string(bytes[1]) + "," + std::to_string(bytes[2]) + "," +
            std::to_string(bytes[3]);
    }

    // Returns true if each channel of the pixel at (x, y) is within `tolerance` of r, g, b, a.
    bool near(int32_t x, int32_t y, int r, int g, int b, int a, int tolerance = 2) {
        uint8_t bytes[4];
        PrCanvasImageData data{
            .extent = PrUVec2{1, 1},
            .format = PrCanvasPixelFormat_RgbaUnorm8,
            .colorSpace = PrColorSpace_Srgb,
        };
        prCanvasGetImageData(handle, PrIVec2{x, y}, &data, bytes);
        const int expected[] = {r, g, b, a};
        for (uint32_t i = 0; i < 4; ++i) {
            if (std::abs(int(bytes[i]) - expected[i]) > tolerance)
                return false;
        }
        return true;
    }

    std::string fillStyle() {
        return std::string(toStd(prCanvasStyle(handle, PrCanvasStyleRole_Fill).color));
    }

    static std::string toStd(PrStringView view) {
        return std::string(view.data != nullptr ? view.data : "", view.size);
    }
};

// Returns true if the build has a renderer that draws on a device, and the run tests on the platform's GPU
// (harness::gpuTests).
bool deviceRenderers() {
    return harness::gpuTests() && (prDrawRendererBackendIsBuilt(PrDrawRendererBackend_Fragment) ||
        prDrawRendererBackendIsBuilt(PrDrawRendererBackend_Triangles));
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("the public header compiles as C") {
    CHECK(prTestCanvasCApi() == 0);
}

TEST_CASE("a new bitmap is transparent black, or opaque black without alpha") {
    MemoryCanvas canvas;
    CHECK(canvas.pixel(0, 0) == "0,0,0,0");
    PrCanvasSettings opaque = prCanvasSettingsDefault();
    opaque.alpha = false;
    MemoryCanvas solid(10, 10, opaque);
    CHECK(solid.pixel(5, 5) == "0,0,0,255");
}

TEST_CASE("fillRect paints the fill style within the rectangle") {
    MemoryCanvas canvas;
    prCanvasSetStyleColor(canvas.handle, PrCanvasStyleRole_Fill, "#0f0");
    prCanvasFillRect(canvas.handle, 0, 0, 50, 50);
    CHECK(canvas.pixel(25, 25) == "0,255,0,255");
    CHECK(canvas.pixel(75, 25) == "0,0,0,0");
}

TEST_CASE("a color style reads back in its HTML-compatible serialization, and an invalid color is ignored") {
    MemoryCanvas canvas;
    CHECK(canvas.fillStyle() == "#000000");
    prCanvasSetStyleColor(canvas.handle, PrCanvasStyleRole_Fill, "rgb(255, 0, 255)");
    CHECK(canvas.fillStyle() == "#ff00ff");
    prCanvasSetStyleColor(canvas.handle, PrCanvasStyleRole_Fill, "#ff00ffed");
    CHECK(canvas.fillStyle() == "rgba(255, 0, 255, 0.93)");
    prCanvasSetStyleColor(canvas.handle, PrCanvasStyleRole_Fill, "hsl(120, 100%, 50%)");
    CHECK(canvas.fillStyle() == "#00ff00");
    prCanvasSetStyleColor(canvas.handle, PrCanvasStyleRole_Fill, "color(srgb 0 1 0)");
    CHECK(canvas.fillStyle() == "color(srgb 0 1 0)");
    prCanvasSetStyleColor(canvas.handle, PrCanvasStyleRole_Fill, "not a color");
    CHECK(canvas.fillStyle() == "color(srgb 0 1 0)");
    prCanvasSetStyleColor(canvas.handle, PrCanvasStyleRole_Fill, "transparent");
    CHECK(canvas.fillStyle() == "rgba(0, 0, 0, 0)");
}

TEST_CASE("restore returns the state that save pushed, and does nothing with no saved state") {
    MemoryCanvas canvas;
    prCanvasRestore(canvas.handle);
    prCanvasSave(canvas.handle);
    prCanvasSetStyleColor(canvas.handle, PrCanvasStyleRole_Fill, "red");
    prCanvasTranslate(canvas.handle, 10, 20);
    prCanvasSetLineWidth(canvas.handle, 4);
    prCanvasRestore(canvas.handle);
    CHECK(canvas.fillStyle() == "#000000");
    PrDAffine2 transform = prCanvasGetTransform(canvas.handle);
    CHECK(transform.e == 0);
    CHECK(transform.f == 0);
    CHECK(prCanvasLineWidth(canvas.handle) == 1);
}

TEST_CASE("the transform maps what is drawn") {
    MemoryCanvas canvas;
    prCanvasSetStyleColor(canvas.handle, PrCanvasStyleRole_Fill, "#00f");
    prCanvasTranslate(canvas.handle, 50, 0);
    prCanvasScale(canvas.handle, 2, 1);
    prCanvasFillRect(canvas.handle, 0, 0, 10, 10);
    CHECK(canvas.pixel(65, 5) == "0,0,255,255");
    CHECK(canvas.pixel(75, 5) == "0,0,0,0");
    CHECK(canvas.pixel(45, 5) == "0,0,0,0");
}

TEST_CASE("clearRect clears within the clipping region") {
    MemoryCanvas canvas;
    prCanvasSetStyleColor(canvas.handle, PrCanvasStyleRole_Fill, "#0f0");
    prCanvasFillRect(canvas.handle, 0, 0, 100, 50);
    prCanvasRect(canvas.handle, 0, 0, 50, 50);
    prCanvasClip(canvas.handle, nullptr, PrPathFillRule_NonZero);
    prCanvasClearRect(canvas.handle, 0, 0, 100, 50);
    CHECK(canvas.pixel(25, 25) == "0,0,0,0");
    CHECK(canvas.pixel(75, 25) == "0,255,0,255");
}

TEST_CASE("a clip lasts until the state that it was made in is restored") {
    MemoryCanvas canvas;
    prCanvasSave(canvas.handle);
    prCanvasRect(canvas.handle, 0, 0, 10, 10);
    prCanvasClip(canvas.handle, nullptr, PrPathFillRule_NonZero);
    prCanvasSetStyleColor(canvas.handle, PrCanvasStyleRole_Fill, "#f00");
    prCanvasFillRect(canvas.handle, 0, 0, 100, 50);
    prCanvasRestore(canvas.handle);
    prCanvasSetStyleColor(canvas.handle, PrCanvasStyleRole_Fill, "#00f");
    prCanvasFillRect(canvas.handle, 50, 0, 50, 50);
    CHECK(canvas.pixel(5, 5) == "255,0,0,255");
    CHECK(canvas.pixel(30, 30) == "0,0,0,0");
    CHECK(canvas.pixel(75, 25) == "0,0,255,255");
}

TEST_CASE("the global alpha multiplies what is drawn") {
    MemoryCanvas canvas;
    prCanvasSetGlobalAlpha(canvas.handle, 0.5);
    prCanvasSetGlobalAlpha(canvas.handle, 2);
    CHECK(prCanvasGlobalAlpha(canvas.handle) == 0.5);
    prCanvasSetStyleColor(canvas.handle, PrCanvasStyleRole_Fill, "#0f0");
    prCanvasFillRect(canvas.handle, 0, 0, 100, 50);
    CHECK(canvas.near(50, 25, 0, 255, 0, 128));
}

TEST_CASE("copy replaces what lies beneath across the clipping region, even where the source is transparent") {
    MemoryCanvas canvas;
    prCanvasSetStyleColor(canvas.handle, PrCanvasStyleRole_Fill, "#0f0");
    prCanvasFillRect(canvas.handle, 0, 0, 100, 50);
    prCanvasSetGlobalCompositeOperation(canvas.handle, "copy");
    CHECK(prCanvasGlobalCompositeOperation(canvas.handle) == "copy");
    prCanvasSetGlobalCompositeOperation(canvas.handle, "Copy");
    CHECK(prCanvasGlobalCompositeOperation(canvas.handle) == "copy");
    prCanvasSetStyleColor(canvas.handle, PrCanvasStyleRole_Fill, "#00f");
    prCanvasFillRect(canvas.handle, 0, 0, 50, 50);
    CHECK(canvas.pixel(25, 25) == "0,0,255,255");
    CHECK(canvas.pixel(75, 25) == "0,0,0,0");
}

TEST_CASE("clear clears only what the draw covers") {
    MemoryCanvas canvas;
    prCanvasSetStyleColor(canvas.handle, PrCanvasStyleRole_Fill, "#0f0");
    prCanvasFillRect(canvas.handle, 0, 0, 100, 50);
    prCanvasSetGlobalCompositeOperation(canvas.handle, "clear");
    prCanvasFillRect(canvas.handle, 0, 0, 50, 50);
    CHECK(canvas.pixel(25, 25) == "0,0,0,0");
    CHECK(canvas.pixel(75, 25) == "0,255,0,255");
}

TEST_CASE("a stroke is centered on its path at the line width") {
    MemoryCanvas canvas;
    prCanvasSetStyleColor(canvas.handle, PrCanvasStyleRole_Stroke, "#0f0");
    prCanvasSetLineWidth(canvas.handle, 10);
    prCanvasMoveTo(canvas.handle, 0, 25);
    prCanvasLineTo(canvas.handle, 100, 25);
    prCanvasStroke(canvas.handle, nullptr);
    CHECK(canvas.pixel(50, 21) == "0,255,0,255");
    CHECK(canvas.pixel(50, 28) == "0,255,0,255");
    CHECK(canvas.pixel(50, 31) == "0,0,0,0");
}

TEST_CASE("a stroke of the current default path takes its width from the transform at the time it strokes") {
    MemoryCanvas canvas;
    prCanvasSetStyleColor(canvas.handle, PrCanvasStyleRole_Stroke, "#0f0");
    prCanvasMoveTo(canvas.handle, 0, 25);
    prCanvasLineTo(canvas.handle, 100, 25);
    prCanvasScale(canvas.handle, 1, 10);
    prCanvasStroke(canvas.handle, nullptr);
    CHECK(canvas.pixel(50, 22) == "0,255,0,255");
    CHECK(canvas.pixel(50, 28) == "0,255,0,255");
}

TEST_CASE("setLineDash ignores a negative or non-finite segment, and repeats an odd list") {
    MemoryCanvas canvas;
    const double odd[] = {1, 2, 3};
    prCanvasSetLineDash(canvas.handle, odd, 3);
    REQUIRE(prCanvasLineDashCount(canvas.handle) == 6);
    CHECK(prCanvasGetLineDash(canvas.handle)[3] == 1);
    const double bad[] = {1, -1};
    prCanvasSetLineDash(canvas.handle, bad, 2);
    CHECK(prCanvasLineDashCount(canvas.handle) == 6);
}

TEST_CASE("a shadow is drawn offset and in its color, beneath the shape") {
    MemoryCanvas canvas;
    prCanvasSetShadowColor(canvas.handle, "#0f0");
    CHECK(MemoryCanvas::toStd(prCanvasShadowColor(canvas.handle)) == "#00ff00");
    prCanvasSetShadowOffset(canvas.handle, PrDVec2{50, 0});
    prCanvasSetStyleColor(canvas.handle, PrCanvasStyleRole_Fill, "#f00");
    prCanvasFillRect(canvas.handle, 0, 0, 50, 50);
    CHECK(canvas.pixel(25, 25) == "255,0,0,255");
    CHECK(canvas.pixel(75, 25) == "0,255,0,255");
}

TEST_CASE("a linear gradient interpolates its stops along its line") {
    MemoryCanvas canvas;
    PrCanvasGradient* gradient = prCanvasGradientCreateLinear(0, 0, 100, 0);
    CHECK(prCanvasGradientAddColorStop(gradient, 0, "#f00") == PrStatus_Ok);
    CHECK(prCanvasGradientAddColorStop(gradient, 1, "#00f") == PrStatus_Ok);
    CHECK(prCanvasGradientAddColorStop(gradient, 1.5, "#00f") == PrStatus_OutOfRange);
    CHECK(prCanvasGradientAddColorStop(gradient, 0.5, "bogus") == PrStatus_Syntax);
    prCanvasSetStyleGradient(canvas.handle, PrCanvasStyleRole_Fill, gradient);
    prCanvasGradientRelease(gradient);
    prCanvasFillRect(canvas.handle, 0, 0, 100, 50);
    CHECK(canvas.near(0, 25, 255, 0, 0, 255, 4));
    CHECK(canvas.near(50, 25, 127, 0, 128, 255, 4));
    CHECK(canvas.near(99, 25, 0, 0, 255, 255, 4));
}

TEST_CASE("putImageData replaces pixels, and getImageData reads them back") {
    MemoryCanvas canvas;
    uint8_t pixels[2 * 2 * 4] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 10, 20, 30, 255};
    PrCanvasImageData data{
        .extent = PrUVec2{2, 2},
        .format = PrCanvasPixelFormat_RgbaUnorm8,
        .colorSpace = PrColorSpace_Srgb,
    };
    prCanvasPutImageData(canvas.handle, &data, pixels, PrIVec2{10, 10}, 0, 0, 2, 2);
    CHECK(canvas.pixel(10, 10) == "255,0,0,255");
    CHECK(canvas.pixel(11, 11) == "10,20,30,255");
    CHECK(canvas.pixel(-1, 0) == "0,0,0,0");
    // The dirty rectangle limits what is written.
    prCanvasPutImageData(canvas.handle, &data, pixels, PrIVec2{20, 20}, 1, 1, -1, -1);
    CHECK(canvas.pixel(20, 20) == "255,0,0,255");
    CHECK(canvas.pixel(21, 21) == "0,0,0,0");
}

TEST_CASE("drawImage draws a source rectangle into a destination rectangle") {
    MemoryCanvas source(10, 10);
    prCanvasSetStyleColor(source.handle, PrCanvasStyleRole_Fill, "#0f0");
    prCanvasFillRect(source.handle, 0, 0, 5, 10);
    const PrDrawImage* image = prCanvasSnapshot(source.handle);
    REQUIRE(image != nullptr);
    MemoryCanvas canvas;
    prCanvasSetImageSmoothingEnabled(canvas.handle, false);
    PrCanvasImageSource from{.image = image, .extent = PrVec2{10, 10}, .originClean = true};
    prCanvasDrawImage(canvas.handle, &from, 0, 0, 10, 10, 0, 0, 100, 50);
    CHECK(canvas.pixel(25, 25) == "0,255,0,255");
    CHECK(canvas.pixel(75, 25) == "0,0,0,0");
    CHECK(prCanvasIsOriginClean(canvas.handle));
    PrCanvasImageSource tainted{.image = image, .extent = PrVec2{10, 10}, .originClean = false};
    prCanvasDrawImage(canvas.handle, &tainted, 0, 0, 10, 10, 0, 0, 10, 10);
    CHECK(!prCanvasIsOriginClean(canvas.handle));
    prDrawImageRelease(image);
}

TEST_CASE("setting the extent clears the bitmap and resets the state") {
    MemoryCanvas canvas;
    prCanvasSetStyleColor(canvas.handle, PrCanvasStyleRole_Fill, "#0f0");
    prCanvasFillRect(canvas.handle, 0, 0, 100, 50);
    uint64_t version = prCanvasVersion(canvas.handle);
    prCanvasSetExtent(canvas.handle, PrUVec2{100, 50});
    CHECK(prCanvasVersion(canvas.handle) != version);
    CHECK(canvas.pixel(50, 25) == "0,0,0,0");
    CHECK(canvas.fillStyle() == "#000000");
}

TEST_CASE("isPointInPath counts points on the path's edge as inside") {
    MemoryCanvas canvas;
    prCanvasRect(canvas.handle, 0, 0, 20, 20);
    CHECK(prCanvasIsPointInPath(canvas.handle, nullptr, 0, 0, PrPathFillRule_NonZero));
    CHECK(prCanvasIsPointInPath(canvas.handle, nullptr, 20, 10, PrPathFillRule_NonZero));
    CHECK(prCanvasIsPointInPath(canvas.handle, nullptr, 10, 10, PrPathFillRule_NonZero));
    CHECK(!prCanvasIsPointInPath(canvas.handle, nullptr, 21, 10, PrPathFillRule_NonZero));
    CHECK(!prCanvasIsPointInPath(canvas.handle, nullptr, std::nan(""), 10, PrPathFillRule_NonZero));
}

TEST_CASE("isPointInStroke tests the stroke at the line width") {
    MemoryCanvas canvas;
    prCanvasSetLineWidth(canvas.handle, 10);
    prCanvasMoveTo(canvas.handle, 0, 25);
    prCanvasLineTo(canvas.handle, 100, 25);
    CHECK(prCanvasIsPointInStroke(canvas.handle, nullptr, 50, 29));
    CHECK(!prCanvasIsPointInStroke(canvas.handle, nullptr, 50, 31));
}

TEST_CASE("a bitmap on a device is drawn by the renderer that presents it there, and reads back what it drew" *
        doctest::skip(!deviceRenderers())) {
    PrGpuDevice* device = nullptr;
    REQUIRE(prGpuDeviceCreate(PrGpuBackend_Default, nullptr, &device) == PrStatus_Ok);
    PrDrawRenderer* renderer = nullptr;
    REQUIRE(prDrawRendererCreate(PrDrawRendererBackend_Default, device, nullptr, &renderer) == PrStatus_Ok);
    PrCanvasDesc desc{.extent = PrUVec2{8, 8}, .settings = prCanvasSettingsDefault(), .device = device};
    PrCanvas* canvas = prCanvasCreate(&desc);
    prCanvasSetStyleColor(canvas, PrCanvasStyleRole_Fill, "#f00");
    prCanvasFillRect(canvas, 0, 0, 4, 8);

    // The renderer draws the recording into the bitmap as it prepares the frame that presents it.
    PrCanvasPresentation presentation = prCanvasPresent(canvas, PrDrawSampling_Nearest);
    REQUIRE(presentation.painter.paint != nullptr);
    PrDrawList* list = prDrawListCreate();
    presentation.painter.paint(presentation.painter.context, PrVec2{8, 8}, list);
    prDrawListFreeze(list);
    PrGpuTexture target{};
    PrGpuTextureDesc targetDesc = prGpuTextureDesc2d(prGpuFormatFromPixelFormat(PrPixelFormat_Rgba8), PrUVec2{8, 8},
        PrGpuTextureUsageFlag_Sampled | PrGpuTextureUsageFlag_RenderTarget);
    REQUIRE(prGpuTextureCreate(device, &targetDesc, &target) == PrStatus_Ok);
    PrDrawFrame* frame = prDrawFrameCreate(renderer);
    PrGpuCommands* commands = nullptr;
    REQUIRE(prGpuCommandsBegin(device, nullptr, &commands));
    PrDrawTarget into{.extent = PrUVec2{8, 8}, .format = PrPixelFormat_Rgba8, .commands = commands};
    REQUIRE(prDrawFramePrepare(frame, list, &into) == PrStatus_Ok);
    PrGpuSubresources all{};
    prGpuCommandsTextureBarrier(commands, target, all, PrGpuAccessFlag_ShaderRead, PrGpuAccessFlag_ColorAttachment);
    PrGpuPassDesc pass = prGpuPassDescColor(target, PrGpuLoad_Clear, PrColor{0, 0, 0, 0});
    PrGpuPass* encoder = prGpuPassBegin(commands, &pass);
    prDrawFrameRender(frame, encoder, PrIVec2{0, 0});
    prGpuPassEnd(encoder);
    prGpuCommandsTextureBarrier(commands, target, all, PrGpuAccessFlag_ColorAttachment, PrGpuAccessFlag_ShaderRead);
    prGpuCommandsSubmitAndWait(commands);
    uint8_t shown[8 * 8 * 4] = {};
    prGpuTextureRead(device, target, PrGpuAccessFlag_ShaderRead, PrIVec2{0, 0}, PrPixels{shown, 8 * 4, {8, 8}});
    CHECK(shown[(1 * 8 + 1) * 4 + 0] == 255);
    CHECK(shown[(1 * 8 + 1) * 4 + 3] == 255);
    CHECK(shown[(1 * 8 + 6) * 4 + 3] == 0);

    // A read returns what the presenting renderer drew into the bitmap.
    uint8_t read[4] = {};
    PrCanvasImageData data{
        .extent = PrUVec2{1, 1},
        .format = PrCanvasPixelFormat_RgbaUnorm8,
        .colorSpace = PrColorSpace_Srgb,
    };
    prCanvasGetImageData(canvas, PrIVec2{1, 1}, &data, read);
    CHECK(read[0] == 255);
    CHECK(read[3] == 255);

    prDrawFrameDestroy(frame);
    prDrawListRelease(list);
    prLayoutPainterRelease(presentation.painter);
    prCanvasDestroy(canvas);
    prGpuTextureDestroy(device, target);
    prDrawRendererDestroy(renderer);
    prGpuDeviceDestroy(device);
}

TEST_CASE("a bitmap on a device reads back what the context drew before any presentation, which draws it no more" *
        doctest::skip(!deviceRenderers())) {
    PrGpuDevice* device = nullptr;
    REQUIRE(prGpuDeviceCreate(PrGpuBackend_Default, nullptr, &device) == PrStatus_Ok);
    PrCanvasDesc desc{.extent = PrUVec2{8, 8}, .settings = prCanvasSettingsDefault(), .device = device};
    PrCanvas* canvas = prCanvasCreate(&desc);
    PrCanvasImageData data{
        .extent = PrUVec2{1, 1},
        .format = PrCanvasPixelFormat_RgbaUnorm8,
        .colorSpace = PrColorSpace_Srgb,
    };
    auto alphaAt = [&](int32_t x, int32_t y) {
        uint8_t read[4] = {};
        prCanvasGetImageData(canvas, PrIVec2{x, y}, &data, read);
        return int(read[3]);
    };
    // A half-opaque fill, which would be more opaque if it drew twice.
    prCanvasSetStyleColor(canvas, PrCanvasStyleRole_Fill, "rgba(0, 0, 255, 0.5)");
    prCanvasFillRect(canvas, 0, 0, 8, 8);
    CHECK(std::abs(alphaAt(1, 1) - 128) <= 1);

    // The presenting renderer draws the bitmap that the read drew into, and none of what the read drew again.
    PrDrawRenderer* renderer = nullptr;
    REQUIRE(prDrawRendererCreate(PrDrawRendererBackend_Default, device, nullptr, &renderer) == PrStatus_Ok);
    PrCanvasPresentation presentation = prCanvasPresent(canvas, PrDrawSampling_Nearest);
    REQUIRE(presentation.painter.paint != nullptr);
    PrDrawList* list = prDrawListCreate();
    presentation.painter.paint(presentation.painter.context, PrVec2{8, 8}, list);
    prDrawListFreeze(list);
    PrDrawFrame* frame = prDrawFrameCreate(renderer);
    PrGpuCommands* commands = nullptr;
    REQUIRE(prGpuCommandsBegin(device, nullptr, &commands));
    PrDrawTarget into{.extent = PrUVec2{8, 8}, .format = PrPixelFormat_Rgba8, .commands = commands};
    REQUIRE(prDrawFramePrepare(frame, list, &into) == PrStatus_Ok);
    prGpuCommandsSubmitAndWait(commands);
    CHECK(std::abs(alphaAt(1, 1) - 128) <= 1);

    // A write follows the drawing before it, which the write's read draws first.
    prCanvasFillRect(canvas, 0, 0, 8, 8);
    uint8_t red[4] = {255, 0, 0, 255};
    prCanvasPutImageData(canvas, &data, red, PrIVec2{2, 2}, 0, 0, 1, 1);
    uint8_t read[4] = {};
    prCanvasGetImageData(canvas, PrIVec2{2, 2}, &data, read);
    CHECK(read[0] == 255);
    CHECK(read[3] == 255);
    CHECK(alphaAt(1, 1) > 128 + 32);

    prDrawFrameDestroy(frame);
    prDrawListRelease(list);
    prLayoutPainterRelease(presentation.painter);
    prCanvasDestroy(canvas);
    prDrawRendererDestroy(renderer);
    prGpuDeviceDestroy(device);
}
