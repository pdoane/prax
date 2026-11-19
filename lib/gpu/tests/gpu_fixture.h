// The fixture of the GPU tests: a device with the test shaders, on each backend that the tests run on.

#pragma once

#include "pr/array.h"
#include "pr/gpu.h"
#include "pr/gpu_select.h"
#include "pr/mem.h"

#include "gpu/tests/kTestGpuShaders.h"
#include "tools/harness/harness_gpu.h"

#include "tools/harness/test.h"

#if defined(PR_GPU_TEST_WEBGPU)
#include "pr/gpu_webgpu.h"
#endif

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

using namespace harness;
using namespace pr;

// The push data of the test shaders (test_gpu.slang).
struct Push {
    float color[4];
    uint32_t texture;
    uint32_t sampler;
    uint32_t buffer;
    uint32_t output;
    float rect[4];
};

// A device, the test shaders on it, and a target to draw into.
struct Fixture {
    PrGpuBackend backend = PrGpuBackend_Default;
    PrGpuDevice* device = nullptr;
    PrGpuShaderLibrary* library = nullptr;
    bool bindless = false;          // the device has PrGpuFeatureFlag_Bindless

    // A device that runs no shaders, such as a memory device, has no fixture, because the tests draw and dispatch. So
    // has a backend that has no device of slots only.
    Fixture(PrGpuBackend backend, bool slotsOnly) : backend(backend) {
        PrGpuDeviceOptions options{.validation = true, .slotsOnly = slotsOnly};
        if (gpuDeviceCreate(backend, &options, &device) != PrStatus_Ok)
            return;
        bindless = (prGpuDeviceInfo(device).features & PrGpuFeatureFlag_Bindless) != 0;
        PrStatus status = prGpuShaderLibraryCreate(device, kTestGpuShaders, sizeof(kTestGpuShaders), &library);
        if (status == PrStatus_NotSupported) {
            prGpuDeviceDestroy(device);
            device = nullptr;
            return;
        }
        REQUIRE(status == PrStatus_Ok);
    }

    // A device that counts its validation errors has none once the test is done.
    ~Fixture() {
#if defined(PR_GPU_TEST_WEBGPU)
        if (device != nullptr && backend == PrGpuBackend_Webgpu) {
            prGpuDeviceWaitIdle(device);
            CHECK(prGpuWebgpuValidationErrors(device) == 0);
        }
#endif
        prGpuShaderLibraryDestroy(library);
        prGpuDeviceDestroy(device);
    }

    PrGpuTexture target(PrUVec2 extent, PrGpuFormat format = PrGpuFormat_Rgba8) {
        PrGpuTextureDesc desc = prGpuTextureDesc2d(
            format, extent, PrGpuTextureUsageFlag_RenderTarget | PrGpuTextureUsageFlag_Sampled);
        PrGpuTexture texture{};
        REQUIRE(prGpuTextureCreate(device, &desc, &texture) == PrStatus_Ok);
        return texture;
    }

    PrGpuPipeline pipeline(const char* vertex, const char* fragment, PrGpuFormat format = PrGpuFormat_Rgba8,
            PrGpuTopology topology = PrGpuTopology_TriangleStrip, bool readsTarget = false) {
        PrGpuGraphicsPipelineDesc desc = description(vertex, fragment, format, topology, readsTarget);
        PrGpuPipeline p{};
        REQUIRE(prGpuPipelineCreateGraphics(device, &desc, &p) == PrStatus_Ok);
        return p;
    }

    PrGpuGraphicsPipelineDesc description(const char* vertex, const char* fragment,
            PrGpuFormat format = PrGpuFormat_Rgba8,
            PrGpuTopology topology = PrGpuTopology_TriangleStrip, bool readsTarget = false) {
        PrGpuGraphicsPipelineDesc desc = prGpuGraphicsPipelineDescDefault(library);
        desc.vertex = vertex;
        desc.fragment = fragment;
        desc.topology = topology;
        desc.formats.colorCount = 1;
        desc.formats.color[0] = format;
        desc.readsTarget = readsTarget;
        return desc;
    }

    // Returns the texels of a texture of 8-bit RGBA that its passes left.
    Array<uint8_t> read(PrGpuTexture texture, uint32_t access = PrGpuAccessFlag_ShaderRead) {
        PrUVec2 extent = prGpuTextureExtent(device, texture);
        Array<uint8_t> texels(extent.x * extent.y * 4);
        gpuTextureRead(device, texture, access, PrIVec2{0, 0}, PrPixels{texels.data(), size_t(extent.x) * 4, extent});
        return texels;
    }

    // Returns the bytes of a buffer, copied through a readback buffer.
    Array<uint8_t> read(PrGpuBuffer buffer, uint64_t size) {
        PrGpuBufferDesc desc{.size = size, .memory = PrGpuMemory_Readback};
        PrGpuBuffer readback{};
        REQUIRE(prGpuBufferCreate(device, &desc, &readback) == PrStatus_Ok);
        PrGpuCommands* commands = nullptr;
        REQUIRE(prGpuCommandsBegin(device, nullptr, &commands));
        prGpuCommandsBufferBarrier(commands, buffer, PrGpuAccessFlag_ShaderRead | PrGpuAccessFlag_ShaderWrite,
            PrGpuAccessFlag_CopyRead);
        prGpuCommandsCopyBuffer(commands, buffer, 0, readback, 0, size);
        gpuSubmitAndWait(commands);
        Array<uint8_t> bytes(size);
        prMemCopy(bytes.data(), prGpuBufferMap(device, readback), size_t(size));
        prGpuBufferRelease(device, readback);
        return bytes;
    }

    // Draws a quad over the whole target with the pipeline and push data, into a pass that loads as `load` says and
    // clears to `clear`, and leaves the target to be sampled.
    void draw(PrGpuTexture target, PrGpuPipeline pipeline, Push push, PrGpuLoad load = PrGpuLoad_Clear,
            PrColor clear = PrColor{0, 0, 0, 0}, const float* uniforms = nullptr) {
        PrGpuCommands* commands = nullptr;
        REQUIRE(prGpuCommandsBegin(device, nullptr, &commands));
        PrGpuSubresources all{};
        prGpuCommandsTextureBarrier(commands, target, all, PrGpuAccessFlag_ShaderRead, PrGpuAccessFlag_ColorAttachment);
        PrGpuPassDesc desc = prGpuPassDescColor(target, load, clear);
        PrGpuPass* pass = prGpuPassBegin(commands, &desc);
        prGpuCommandsSetPipeline(commands, pipeline);
        prGpuCommandsSetPush(commands, &push, sizeof(push));
        if (uniforms != nullptr)
            prGpuCommandsSetUniforms(commands, uniforms, sizeof(float) * 4);
        prGpuPassDraw(pass, 4, 1, 0, 0);
        prGpuPassEnd(pass);
        prGpuCommandsTextureBarrier(commands, target, all, PrGpuAccessFlag_ColorAttachment, PrGpuAccessFlag_ShaderRead);
        gpuSubmitAndWait(commands);
    }
};

// Push data that draws `color` over the whole target.
inline Push fill(float r, float g, float b, float a) {
    return Push{.color = {r, g, b, a}, .rect = {-1, 1, 1, -1}};
}

// Returns the texel at (x, y) of 8-bit RGBA texels `width` wide.
inline const uint8_t* texel(const Array<uint8_t>& texels, uint32_t width, uint32_t x, uint32_t y) {
    return &texels[(y * width + x) * 4];
}

// Returns true if each channel of the texel `t` is within `tolerance` of the given value.
inline bool near(const uint8_t* t, uint8_t r, uint8_t g, uint8_t b, uint8_t a, int tolerance = 1) {
    auto close = [&](uint8_t x, uint8_t y) { return (x > y ? x - y : y - x) <= tolerance; };
    return close(t[0], r) && close(t[1], g) && close(t[2], b) && close(t[3], a);
}

// Runs `test` on a fixture for each backend that the build has and the system has a device for, and again on a device
// of slots only where the backend makes one. A run without the platform's GPU (harness::gpuTests) tests the memory
// backend alone.
template <typename Test>
void onEachBackend(Test test) {
    const bool slotModes[] = {false, true};
    for (PrGpuBackend backend = PrGpuBackend_Default + 1; backend < PrGpuBackend_Count; ++backend) {
        if (!gpuTests() && backend != PrGpuBackend_Memory)
            continue;
        for (bool slotsOnly : slotModes) {
            Fixture f(backend, slotsOnly);
            if (f.device == nullptr)
                continue;
            PrStringView name = prBackendName(prGpuSelectInterface(), backend);
            CAPTURE(name);
            CAPTURE(slotsOnly);
            test(f);
        }
    }
}

// Runs `test` as onEachBackend does, on the devices that have PrGpuFeatureFlag_Bindless.
template <typename Test>
void onEachBindless(Test test) {
    onEachBackend([&](Fixture& f) {
        if (f.bindless)
            test(f);
    });
}

} // namespace pr_impl

PR_ASSUME_NONNULL_END
