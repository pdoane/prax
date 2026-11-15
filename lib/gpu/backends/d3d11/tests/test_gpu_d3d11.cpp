// The tests of what only the Direct3D 11 backend does: a device of slots alone, which refuses the entry points that
// reach resources by handle. The interface's own tests run on Direct3D 11 as well.

#include "pr/gpu_d3d11.h"

#include "gpu/tests/kTestGpuShaders.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

// A device on the system's high-performance adapter, with the debug layer where it is installed. A test that finds no
// adapter with the features that the backend needs skips.
struct Fixture {
    PrGpuDevice* device = nullptr;

    Fixture() {
        PrGpuD3d11Options options = prGpuD3d11OptionsDefault();
        options.validation = true;
        // Without a pool, the device's creation is done when the constructor returns.
        device = prGpuD3d11Create(&options);
        PrStatus status = prGpuDeviceLostStatus(device);
        if (status == PrStatus_NotSupported) {
            MESSAGE("no Direct3D 11 adapter with the features that the backend needs");
            prGpuDeviceDestroy(device);
            device = nullptr;
            return;
        }
        REQUIRE(status == PrStatus_Ok);
    }

    ~Fixture() {
        prGpuDeviceDestroy(device);
    }
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("d3d11 binds resources only to slots") {
    Fixture f;
    if (f.device == nullptr)
        return;
    PrGpuDeviceInfo info = prGpuDeviceInfo(f.device);
    CHECK(info.backend == "Direct3D 11");
    CHECK((info.features & PrGpuFeatureFlag_Bindless) == 0);
    CHECK(info.maxResourceHandles == 0);
    CHECK(info.maxBufferSlots == 4);
    CHECK(info.maxTextureSlots == 8);
}

TEST_CASE("d3d11 refuses a pipeline whose shaders reach resources by handle, and makes one that binds slots") {
    Fixture f;
    if (f.device == nullptr)
        return;
    PrGpuShaderLibrary* library = nullptr;
    REQUIRE(prGpuShaderLibraryCreate(f.device, kTestGpuShaders, sizeof(kTestGpuShaders), &library) == PrStatus_Ok);
    PrGpuGraphicsPipelineDesc desc = prGpuGraphicsPipelineDescDefault(library);
    desc.vertex = "quadVertex";
    desc.fragment = "followed";
    desc.formats.colorCount = 1;
    desc.formats.color[0] = PrGpuFormat_Rgba8;
    PrGpuPipeline pipeline{};
    CHECK(prGpuPipelineCreateGraphics(f.device, &desc, &pipeline) == PrStatus_NotSupported);
    CHECK(pipeline.id == 0);
    desc.fragment = "slotSampled";
    REQUIRE(prGpuPipelineCreateGraphics(f.device, &desc, &pipeline) == PrStatus_Ok);
    prGpuPipelineDestroy(f.device, pipeline);
    prGpuShaderLibraryDestroy(library);
}
