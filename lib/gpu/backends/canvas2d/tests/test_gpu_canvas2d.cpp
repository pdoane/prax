// The tests of what the Canvas 2D backend adds to a memory device. The memory backend's tests cover the device itself.
// The tests run under Node.js, which has no canvases, so the backend creates no device there and the default device is
// the memory backend's. The examples show the backend's canvases in a browser.

#include "pr/gpu.h"
#include "pr/gpu_canvas2d.h"

#include "tools/harness/test.h"

TEST_CASE("a canvas2d device is not created without a page's canvases, and Default falls back to memory") {
    PrGpuDevice* device = nullptr;
    CHECK(prGpuCanvas2dCreate(&device) == PrStatus_NotSupported);
    CHECK(device == nullptr);
    REQUIRE(prGpuDeviceCreate(PrGpuBackend_Default, nullptr, &device) == PrStatus_Ok);
    CHECK(prGpuDeviceInfo(device).backend == "memory");
    CHECK(!prGpuDeviceHasNative(device, PrGpuNativeKind_Canvas2d));
    prGpuDeviceDestroy(device);
}
