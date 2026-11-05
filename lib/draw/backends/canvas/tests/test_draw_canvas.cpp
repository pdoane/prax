// The tests of the Canvas 2D renderer that run without a page, under Node.js. What it draws is seen in a browser,
// through the examples.

#include "pr/draw_canvas.h"
#include "pr/gpu.h"

#include "tools/harness/test.h"

TEST_CASE("a Canvas 2D renderer needs a canvas device, and Default passes over it") {
    PrGpuDevice* device = nullptr;
    REQUIRE(prGpuDeviceCreate(PrGpuBackend_Memory, nullptr, &device) == PrStatus_Ok);
    PrDrawRenderer* renderer = nullptr;
    CHECK(prDrawCanvasCreate(device, nullptr, &renderer) == PrStatus_NotSupported);
    CHECK(renderer == nullptr);
    REQUIRE(prDrawRendererCreate(PrDrawRendererBackend_Default, device, nullptr, &renderer) == PrStatus_Ok);
    prDrawRendererDestroy(renderer);
    prGpuDeviceDestroy(device);
}
