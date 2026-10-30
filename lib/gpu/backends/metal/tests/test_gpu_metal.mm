// The tests of what only the Metal backend has: recording into the host's command buffers and passes. The interface's
// own tests run on Metal as well.

#include "pr/gpu_metal.h"

#include "gpu/tests/kTestGpuHostShaders.h"

#include "doctest/doctest.h"

#include <cstdint>
#include <cstring>
#include <vector>

#import <Metal/Metal.h>

namespace pr_impl {

namespace {

// Metal's API validation, which every test of the binary runs under, as the Vulkan and D3D12 tests run under their
// validation layers. Metal reads the variable when it creates the first device, so it is set before main. A value that
// the environment gives is kept.
const int kValidation = setenv("MTL_DEBUG_LAYER", "1", 0);

// The root data of the test shaders, as test_gpu_host.slang declares it.
struct Push {
    uint32_t output;
    uint32_t count;
    uint32_t scale;
};

struct Uniforms {
    float color[4];
};

// A device on the system's default Metal device, and the test shaders. A test that finds no device that supports Metal
// 4 skips.
struct Fixture {
    PrGpuDevice* device = nullptr;
    PrGpuShaderLibrary* library = nullptr;

    Fixture() {
        PrGpuMetalOptions options = prGpuMetalOptionsDefault();
        options.validation = true;
        PrStatus status = prGpuMetalCreate(&options, &device);
        if (status == PrStatus_NotSupported) {
            MESSAGE("no Metal device that supports Metal 4");
            return;
        }
        REQUIRE(status == PrStatus_Ok);
        REQUIRE(prGpuShaderLibraryCreate(device, kTestGpuHostShaders, sizeof(kTestGpuHostShaders), &library) ==
            PrStatus_Ok);
    }

    ~Fixture() {
        if (device == nullptr)
            return;
        prGpuShaderLibraryDestroy(library);
        prGpuDeviceDestroy(device);
    }

    PrGpuBuffer buffer(uint64_t size, PrGpuBufferUsageFlags usage, PrGpuMemory memory = PrGpuMemory_Device) {
        PrGpuBufferDesc desc{.size = size, .usage = usage, .memory = memory};
        PrGpuBuffer made{};
        REQUIRE(prGpuBufferCreate(device, &desc, &made) == PrStatus_Ok);
        return made;
    }

    PrGpuTexture target() {
        PrGpuTextureUsageFlags usage = PrGpuTextureUsageFlag_RenderTarget;
        PrGpuTextureDesc desc = prGpuTextureDesc2d(PrGpuFormat_Rgba8, PrUVec2{8, 8}, usage);
        PrGpuTexture texture{};
        REQUIRE(prGpuTextureCreate(device, &desc, &texture) == PrStatus_Ok);
        return texture;
    }

    PrGpuPipeline upperHalf() {
        PrGpuGraphicsPipelineDesc desc = prGpuGraphicsPipelineDescDefault(library);
        desc.vertex = "upperHalf";
        desc.fragment = "solid";
        desc.topology = PrGpuTopology_TriangleStrip;
        desc.formats.colorCount = 1;
        desc.formats.color[0] = PrGpuFormat_Rgba8;
        PrGpuPipeline pipeline{};
        REQUIRE(prGpuPipelineCreateGraphics(device, &desc, &pipeline) == PrStatus_Ok);
        return pipeline;
    }

    // Copies the words of `buffer` into readback memory, waits for them, and returns them.
    std::vector<uint32_t> words(PrGpuBuffer buffer, uint32_t count) {
        PrGpuBuffer readback = this->buffer(uint64_t(count) * 4, 0, PrGpuMemory_Readback);
        PrGpuCommands* commands = nullptr;
        REQUIRE(prGpuCommandsBegin(device, nullptr, &commands));
        prGpuCommandsBufferBarrier(commands, buffer, PrGpuAccessFlag_ShaderWrite | PrGpuAccessFlag_CopyWrite,
            PrGpuAccessFlag_CopyRead);
        prGpuCommandsCopyBuffer(commands, buffer, 0, readback, 0, uint64_t(count) * 4);
        prGpuDeviceWait(device, prGpuCommandsSubmit(commands));
        std::vector<uint32_t> out(count);
        memcpy(out.data(), prGpuBufferMap(device, readback), size_t(count) * 4);
        prGpuBufferDestroy(device, readback);
        return out;
    }
};

// A host's own use of Metal on the device's MTLDevice: a command buffer that it begins and ends.
struct HostMetal {
    id<MTLDevice> device;
    id<MTL4CommandAllocator> allocator;
    id<MTL4CommandBuffer> buffer;

    explicit HostMetal(PrGpuDevice* gpu) {
        device = (__bridge id<MTLDevice>)prGpuMetalDevice(gpu);
        allocator = [device newCommandAllocator];
        buffer = [device newCommandBuffer];
        [buffer beginCommandBufferWithAllocator:allocator];
    }
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("metal records into the host's command buffer, around the host's own work, and submits it") {
    Fixture f;
    if (f.device == nullptr)
        return;
    HostMetal host(f.device);

    // An upload made before the submission is visible to all of its work, the host's native command buffer's too.
    PrGpuBuffer output = f.buffer(16, PrGpuBufferUsageFlag_Storage);
    PrGpuPipeline pipeline{};
    REQUIRE(prGpuPipelineCreateCompute(f.device, f.library, "fill", "fill", &pipeline) == PrStatus_Ok);
    PrGpuCommandsOptions options = prGpuCommandsOptionsDefault();
    options.native = (__bridge void*)host.buffer;
    PrGpuCommands* commands = nullptr;
    REQUIRE(prGpuCommandsBegin(f.device, &options, &commands));
    uint32_t seed[4] = {9, 9, 9, 9};
    prGpuBufferUpload(f.device, output, 0, seed, sizeof(seed));
    Push push{.output = prGpuBufferStorageHandle(f.device, output), .count = 2, .scale = 11};
    prGpuCommandsSetPipeline(commands, pipeline);
    prGpuCommandsSetPush(commands, &push, sizeof(push));
    prGpuCommandsDispatch(commands, PrGpuExtent{1, 1, 1});
    // The device's state ends with its encoder, so the host sets nothing again.
    CHECK(prGpuHostStateSync(commands) == 0);
    [host.buffer endCommandBuffer];
    PrGpuFence fence = prGpuCommandsSubmit(commands);
    prGpuDeviceWait(f.device, fence);
    CHECK(prGpuDeviceIsComplete(f.device, fence));
    CHECK(f.words(output, 4) == std::vector<uint32_t>{0, 11, 9, 9});
    prGpuPipelineDestroy(f.device, pipeline);
    prGpuBufferDestroy(f.device, output);
}

TEST_CASE("metal draws into the host's pass, and says which of the pass's state its draws changed") {
    Fixture f;
    if (f.device == nullptr)
        return;
    HostMetal host(f.device);
    PrGpuTexture target = f.target();
    PrGpuPipeline pipeline = f.upperHalf();

    PrGpuCommandsOptions options = prGpuCommandsOptionsDefault();
    options.native = (__bridge void*)host.buffer;
    PrGpuCommands* commands = nullptr;
    REQUIRE(prGpuCommandsBegin(f.device, &options, &commands));
    prGpuCommandsTextureBarrier(commands, target, PrGpuSubresources{}, PrGpuAccessFlag_Discard,
        PrGpuAccessFlag_ColorAttachment);
    prGpuHostStateSync(commands);

    // The host begins its pass, which clears the target to blue, and the device draws the upper half red into it.
    MTL4RenderPassDescriptor* rp = [MTL4RenderPassDescriptor new];
    rp.colorAttachments[0].texture = (__bridge id<MTLTexture>)prGpuMetalTexture(f.device, target);
    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 1, 1);
    id<MTL4RenderCommandEncoder> encoder = [host.buffer renderCommandEncoderWithDescriptor:rp];
    PrGpuPassDesc passDesc{};
    passDesc.host = (__bridge void*)encoder;
    passDesc.hostFormats.colorCount = 1;
    passDesc.hostFormats.color[0] = PrGpuFormat_Rgba8;
    passDesc.hostFormats.samples = 1;
    passDesc.hostExtent = PrUVec2{8, 8};
    PrGpuPass* pass = prGpuPassBegin(commands, &passDesc);
    Uniforms uniforms{.color = {1, 0, 0, 1}};
    prGpuCommandsSetPipeline(commands, pipeline);
    prGpuCommandsSetUniforms(commands, &uniforms, sizeof(uniforms));
    prGpuPassDraw(pass, 4, 1, 0, 0);
    PrGpuHostStateFlags changed = prGpuHostStateSync(commands);
    CHECK((changed & PrGpuHostStateFlag_Pipeline) != 0);
    CHECK((changed & PrGpuHostStateFlag_Bindings) != 0);
    CHECK((changed & PrGpuHostStateFlag_IndexBuffer) == 0);
    CHECK(prGpuHostStateSync(commands) == 0);
    prGpuPassEnd(pass);
    [encoder endEncoding];

    prGpuCommandsTextureBarrier(commands, target, PrGpuSubresources{}, PrGpuAccessFlag_ColorAttachment,
        PrGpuAccessFlag_CopyRead);
    prGpuHostStateSync(commands);
    [host.buffer endCommandBuffer];
    prGpuDeviceWait(f.device, prGpuCommandsSubmit(commands));
    uint32_t pixels[8 * 8]{};
    prGpuTextureRead(f.device, target, PrGpuAccessFlag_CopyRead, PrIVec2{0, 0},
        PrPixels{reinterpret_cast<uint8_t*>(pixels), 8 * 4, PrUVec2{8, 8}});
    CHECK(pixels[0] == 0xff0000ffu);
    CHECK(pixels[7 * 8 + 7] == 0xffff0000u);
    prGpuPipelineDestroy(f.device, pipeline);
    prGpuTextureDestroy(f.device, target);
}
