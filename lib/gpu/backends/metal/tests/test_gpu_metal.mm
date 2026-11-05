// The tests of what only the Metal backend has: recording into the application's command buffers and passes. The
// interface's own tests run on Metal as well.

#include "pr/gpu_metal.h"

#include "gpu/tests/kTestGpuNativeShaders.h"

#include "tools/harness/test.h"

#include <cstdint>
#include <cstring>
#include <vector>

#import <AppKit/AppKit.h>
#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

namespace pr_impl {

namespace {

// Metal's API validation, which every test of the binary runs under, as the Vulkan and D3D12 tests run under their
// validation layers. Metal reads the variable when it creates the first device, so it is set before main. A value that
// the environment gives is kept.
const int kValidation = setenv("MTL_DEBUG_LAYER", "1", 0);

// The root data of the test shaders, as test_gpu_native.slang declares it.
struct Push {
    uint32_t output;
    uint32_t count;
    uint32_t scale;
};

struct Uniforms {
    float color[4];
};

// A device on the system's default Metal device, and the test shaders. A test that finds no device that supports Metal
// 3 skips.
struct Fixture {
    PrGpuDevice* device = nullptr;
    PrGpuShaderLibrary* library = nullptr;

    Fixture() {
        PrGpuMetalOptions options = prGpuMetalOptionsDefault();
        options.validation = true;
        PrStatus status = prGpuMetalCreate(&options, &device);
        if (status == PrStatus_NotSupported) {
            MESSAGE("no Metal device that supports Metal 3");
            return;
        }
        REQUIRE(status == PrStatus_Ok);
        REQUIRE(prGpuShaderLibraryCreate(device, kTestGpuNativeShaders, sizeof(kTestGpuNativeShaders), &library) ==
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

// An application's own use of Metal on the device's queue: a command buffer that the device's submission commits.
struct AppMetal {
    id<MTLCommandBuffer> buffer;

    explicit AppMetal(PrGpuDevice* gpu) {
        buffer = [(__bridge id<MTLCommandQueue>)prGpuMetalQueue(gpu) commandBuffer];
    }
};

// Clears the swap chain's next drawable and presents it.
PrGpuFence presentFrame(PrGpuDevice* device, PrGpuSwapChain* swapChain) {
    PrGpuTexture texture{};
    REQUIRE(prGpuSwapChainAcquire(swapChain, PrUVec2{32, 32}, &texture) == PrStatus_Ok);
    PrGpuCommands* commands = nullptr;
    REQUIRE(prGpuCommandsBegin(device, nullptr, &commands));
    prGpuCommandsTextureBarrier(commands, texture, PrGpuSubresources{}, PrGpuAccessFlag_Discard,
        PrGpuAccessFlag_ColorAttachment);
    PrGpuPassDesc desc = prGpuPassDescColor(texture, PrGpuLoad_Clear, PrColor{0, 1, 0, 1});
    prGpuPassEnd(prGpuPassBegin(commands, &desc));
    prGpuCommandsTextureBarrier(commands, texture, PrGpuSubresources{}, PrGpuAccessFlag_ColorAttachment,
        PrGpuAccessFlag_Present);
    prGpuSwapChainPresent(swapChain, commands);
    return prGpuCommandsSubmit(commands);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("metal records into the application's command buffer, around the application's own work, and submits it") {
    Fixture f;
    if (f.device == nullptr)
        return;
    AppMetal app(f.device);

    // An upload made before the submission is visible to all of the submission's work, including the work in the
    // application's native command buffer.
    PrGpuBuffer output = f.buffer(16, PrGpuBufferUsageFlag_Storage);
    PrGpuPipeline pipeline{};
    REQUIRE(prGpuPipelineCreateCompute(f.device, f.library, "fill", "fill", &pipeline) == PrStatus_Ok);
    PrGpuCommandsOptions options = prGpuCommandsOptionsDefault();
    options.native = (__bridge void*)app.buffer;
    PrGpuCommands* commands = nullptr;
    REQUIRE(prGpuCommandsBegin(f.device, &options, &commands));
    uint32_t seed[4] = {9, 9, 9, 9};
    prGpuBufferUpload(f.device, output, 0, seed, sizeof(seed));
    Push push{.output = prGpuBufferStorageHandle(f.device, output), .count = 2, .scale = 11};
    prGpuCommandsSetPipeline(commands, pipeline);
    prGpuCommandsSetPush(commands, &push, sizeof(push));
    prGpuCommandsDispatch(commands, PrGpuExtent{1, 1, 1});
    // The device's state ends with its encoder, so the application sets nothing again.
    CHECK(prGpuNativeStateSync(commands) == 0);
    PrGpuFence fence = prGpuCommandsSubmit(commands);
    prGpuDeviceWait(f.device, fence);
    CHECK(prGpuDeviceIsComplete(f.device, fence));
    CHECK(f.words(output, 4) == std::vector<uint32_t>{0, 11, 9, 9});
    prGpuPipelineDestroy(f.device, pipeline);
    prGpuBufferDestroy(f.device, output);
}

TEST_CASE("metal draws into the application's pass, and says which of the pass's state its draws changed") {
    Fixture f;
    if (f.device == nullptr)
        return;
    AppMetal app(f.device);
    PrGpuTexture target = f.target();
    PrGpuPipeline pipeline = f.upperHalf();

    PrGpuCommandsOptions options = prGpuCommandsOptionsDefault();
    options.native = (__bridge void*)app.buffer;
    PrGpuCommands* commands = nullptr;
    REQUIRE(prGpuCommandsBegin(f.device, &options, &commands));
    prGpuCommandsTextureBarrier(commands, target, PrGpuSubresources{}, PrGpuAccessFlag_Discard,
        PrGpuAccessFlag_ColorAttachment);
    prGpuNativeStateSync(commands);

    // The application begins its pass, which clears the target to blue, and the device draws the upper half red into
    // it.
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor new];
    void* native = nullptr;
    REQUIRE(prGpuTextureNative(f.device, target, PrGpuNativeKind_Metal, &native));
    rp.colorAttachments[0].texture = (__bridge id<MTLTexture>)native;
    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 1, 1);
    id<MTLRenderCommandEncoder> encoder = [app.buffer renderCommandEncoderWithDescriptor:rp];
    PrGpuPassDesc passDesc{};
    passDesc.native = (__bridge void*)encoder;
    passDesc.nativeFormats.colorCount = 1;
    passDesc.nativeFormats.color[0] = PrGpuFormat_Rgba8;
    passDesc.nativeFormats.samples = 1;
    passDesc.nativeExtent = PrUVec2{8, 8};
    PrGpuPass* pass = prGpuPassBegin(commands, &passDesc);
    Uniforms uniforms{.color = {1, 0, 0, 1}};
    prGpuCommandsSetPipeline(commands, pipeline);
    prGpuCommandsSetUniforms(commands, &uniforms, sizeof(uniforms));
    prGpuPassDraw(pass, 4, 1, 0, 0);
    PrGpuNativeStateFlags changed = prGpuNativeStateSync(commands);
    CHECK((changed & PrGpuNativeStateFlag_Pipeline) != 0);
    CHECK((changed & PrGpuNativeStateFlag_Bindings) != 0);
    CHECK((changed & PrGpuNativeStateFlag_IndexBuffer) == 0);
    CHECK(prGpuNativeStateSync(commands) == 0);
    prGpuPassEnd(pass);
    [encoder endEncoding];

    prGpuCommandsTextureBarrier(commands, target, PrGpuSubresources{}, PrGpuAccessFlag_ColorAttachment,
        PrGpuAccessFlag_CopyRead);
    prGpuNativeStateSync(commands);
    prGpuDeviceWait(f.device, prGpuCommandsSubmit(commands));
    uint32_t pixels[8 * 8]{};
    prGpuTextureRead(f.device, target, PrGpuAccessFlag_CopyRead, PrIVec2{0, 0},
        PrPixels{reinterpret_cast<uint8_t*>(pixels), 8 * 4, PrUVec2{8, 8}});
    CHECK(pixels[0] == 0xff0000ffu);
    CHECK(pixels[7 * 8 + 7] == 0xffff0000u);
    prGpuPipelineDestroy(f.device, pipeline);
    prGpuTextureDestroy(f.device, target);
}

TEST_CASE("metal hands a native draw the pass's encoder, and draws after it with its own state again") {
    Fixture f;
    if (f.device == nullptr)
        return;
    REQUIRE(prGpuDeviceHasNative(f.device, PrGpuNativeKind_Metal));
    PrGpuTexture target = f.target();
    PrGpuPipeline pipeline = f.upperHalf();
    PrGpuCommands* commands = nullptr;
    REQUIRE(prGpuCommandsBegin(f.device, nullptr, &commands));
    prGpuCommandsTextureBarrier(commands, target, PrGpuSubresources{}, PrGpuAccessFlag_Discard,
        PrGpuAccessFlag_ColorAttachment);
    PrGpuPassDesc desc = prGpuPassDescColor(target, PrGpuLoad_Clear, PrColor{0, 0, 1, 1});
    PrGpuPass* pass = prGpuPassBegin(commands, &desc);

    // The native draw narrows the scissor to the left half, which the device's draw after it does not keep.
    bool drawn = false;
    prGpuPassNativeDraw(pass, PrGpuNativeKind_Metal, [](void* context, const void* native) {
        const PrGpuMetalNativePass& objects = *static_cast<const PrGpuMetalNativePass*>(native);
        CHECK(objects.commandBuffer != nullptr);
        id<MTLRenderCommandEncoder> encoder = (__bridge id<MTLRenderCommandEncoder>)objects.encoder;
        [encoder setScissorRect:MTLScissorRect{0, 0, 4, 8}];
        *static_cast<bool*>(context) = true;
    }, &drawn);
    CHECK(drawn);
    Uniforms uniforms{.color = {1, 0, 0, 1}};
    prGpuCommandsSetPipeline(commands, pipeline);
    prGpuCommandsSetUniforms(commands, &uniforms, sizeof(uniforms));
    prGpuPassDraw(pass, 4, 1, 0, 0);
    prGpuPassEnd(pass);
    prGpuCommandsTextureBarrier(commands, target, PrGpuSubresources{}, PrGpuAccessFlag_ColorAttachment,
        PrGpuAccessFlag_CopyRead);
    prGpuDeviceWait(f.device, prGpuCommandsSubmit(commands));
    uint32_t pixels[8 * 8]{};
    prGpuTextureRead(f.device, target, PrGpuAccessFlag_CopyRead, PrIVec2{0, 0},
        PrPixels{reinterpret_cast<uint8_t*>(pixels), 8 * 4, PrUVec2{8, 8}});
    CHECK(pixels[7] == 0xff0000ffu);
    CHECK(pixels[7 * 8] == 0xffff0000u);
    prGpuPipelineDestroy(f.device, pipeline);
    prGpuTextureDestroy(f.device, target);
}

TEST_CASE("metal imports a CVPixelBuffer as a texture without copying it, and releases it once the texture is gone") {
    Fixture f;
    if (f.device == nullptr)
        return;
    REQUIRE(prGpuDeviceImportsInPlace(f.device, PrGpuImportKind_CoreVideo));
    // A buffer of 4 by 2 BGRA texels backed by an IOSurface, its first row blue and its second red.
    NSDictionary* attributes = @{
        (id)kCVPixelBufferIOSurfacePropertiesKey: @{},
        (id)kCVPixelBufferMetalCompatibilityKey: @YES,
    };
    CVPixelBufferRef buffer = nullptr;
    REQUIRE(CVPixelBufferCreate(kCFAllocatorDefault, 4, 2, kCVPixelFormatType_32BGRA,
        (__bridge CFDictionaryRef)attributes, &buffer) == kCVReturnSuccess);
    CVPixelBufferLockBaseAddress(buffer, 0);
    uint8_t* texels = static_cast<uint8_t*>(CVPixelBufferGetBaseAddress(buffer));
    size_t stride = CVPixelBufferGetBytesPerRow(buffer);
    for (uint32_t y = 0; y < 2; ++y) {
        for (uint32_t x = 0; x < 4; ++x) {
            const uint8_t bgra[4] = {uint8_t(y == 0 ? 255 : 0), 0, uint8_t(y == 1 ? 255 : 0), 255};
            memcpy(texels + y * stride + x * 4, bgra, 4);
        }
    }
    CVPixelBufferUnlockBaseAddress(buffer, 0);

    bool released = false;
    PrGpuImport image{.kind = PrGpuImportKind_CoreVideo, .handle = buffer};
    PrGpuTexture texture{};
    REQUIRE(prGpuTextureImport(f.device, &image, [](void* context) { *static_cast<bool*>(context) = true; },
        &released, &texture) == PrStatus_Ok);
    CVPixelBufferRelease(buffer);
    PrGpuTextureDesc desc = prGpuTextureDesc(f.device, texture);
    CHECK(desc.format == PrGpuFormat_Bgra8);
    CHECK(desc.extent.width == 4);
    CHECK(desc.extent.height == 2);
    uint32_t read[4 * 2]{};
    prGpuTextureRead(f.device, texture, PrGpuAccessFlag_ShaderRead, PrIVec2{0, 0},
        PrPixels{reinterpret_cast<uint8_t*>(read), 4 * 4, PrUVec2{4, 2}});
    CHECK(read[0] == 0xff0000ffu);
    CHECK(read[4] == 0xffff0000u);
    CHECK(!released);
    prGpuTextureDestroy(f.device, texture);
    prGpuDeviceWaitIdle(f.device);
    CHECK(released);
}

TEST_CASE("metal presents frames, with the transaction always unless threaded") {
    Fixture f;
    if (f.device == nullptr)
        return;
    @autoreleasepool {
        NSView* view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 32, 32)];
        PrAppSurface surface{PrAppSurfaceKind_NsView, (__bridge void*)view};
        PrGpuSwapChainDesc desc{.intent = PrGpuSwapChainIntent_Sdr};
        PrGpuSwapChain* transactional = nullptr;
        REQUIRE(prGpuSwapChainCreate(f.device, &surface, &desc, &transactional) == PrStatus_Ok);
        CAMetalLayer* layer = (CAMetalLayer*)view.layer;
        CHECK(layer.presentsWithTransaction);
        for (int frame = 0; frame < 4; ++frame) {
            PrGpuFence fence = presentFrame(f.device, transactional);
            CHECK(layer.presentsWithTransaction == true);
            prGpuDeviceWait(f.device, fence);
        }
        prGpuSwapChainDestroy(transactional);

        NSView* threadedView = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 32, 32)];
        PrAppSurface threadedSurface{PrAppSurfaceKind_NsView, (__bridge void*)threadedView};
        desc.flags = PrGpuSwapChainFlag_Threaded;
        PrGpuSwapChain* threaded = nullptr;
        REQUIRE(prGpuSwapChainCreate(f.device, &threadedSurface, &desc, &threaded) == PrStatus_Ok);
        CAMetalLayer* threadedLayer = (CAMetalLayer*)threadedView.layer;
        for (int frame = 0; frame < 4; ++frame) {
            PrGpuFence fence = presentFrame(f.device, threaded);
            CHECK_FALSE(threadedLayer.presentsWithTransaction);
            prGpuDeviceWait(f.device, fence);
        }
        prGpuSwapChainDestroy(threaded);
    }
}
