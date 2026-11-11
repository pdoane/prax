// The tests of what the Canvas 2D backend adds to a memory device. The memory backend's tests cover the device itself.
// The tests run under Node.js, which has no canvases. A device of the backend is lost there, and Default passes over
// the backend and creates a memory device. The examples show the backend's canvases in a browser.

#include "pr/gpu.h"
#include "pr/gpu_canvas2d.h"

#include "tools/harness/harness_gpu.h"

#include "tools/harness/test.h"

using namespace harness;

TEST_CASE("a canvas2d device is lost without a page's canvases, and Default creates a memory device") {
    PrGpuDevice* device = prGpuCanvas2dCreate(nullptr);
    CHECK(prGpuDeviceIsReady(device));
    CHECK(prGpuDeviceLostStatus(device) == PrStatus_NotSupported);
    prGpuDeviceDestroy(device);
    REQUIRE(gpuDeviceCreate(PrGpuBackend_Default, nullptr, &device) == PrStatus_Ok);
    CHECK(prGpuDeviceInfo(device).backend == "memory");
    CHECK(!prGpuDeviceHasNative(device, PrGpuNativeKind_Canvas2d));
    prGpuDeviceDestroy(device);
}
