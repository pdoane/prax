// The tests of what only the Metal 4 backend has: recording into the application's command buffers and passes. The
// interface's own tests run on Metal 4 as well.

#include "pr/array.h"
#include "pr/gpu_metal4.h"
#include "pr/mem.h"

#include "gpu/tests/kTestGpuNativeShaders.h"

#include "tools/harness/test.h"

#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

namespace pr_impl {

using namespace pr;

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
// 4 skips.
struct Fixture {
    PrGpuDevice* device = nullptr;
    PrGpuShaderLibrary* library = nullptr;

    Fixture() {
        PrGpuMetal4Options options = prGpuMetal4OptionsDefault();
        options.validation = true;
        PrStatus status = prGpuMetal4Create(&options, &device);
        if (status == PrStatus_NotSupported) {
            MESSAGE("no Metal device that supports Metal 4");
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
    Array<uint32_t> words(PrGpuBuffer buffer, uint32_t count) {
        PrGpuBuffer readback = this->buffer(uint64_t(count) * 4, 0, PrGpuMemory_Readback);
        PrGpuCommands* commands = nullptr;
        REQUIRE(prGpuCommandsBegin(device, nullptr, &commands));
        prGpuCommandsBufferBarrier(commands, buffer, PrGpuAccessFlag_ShaderWrite | PrGpuAccessFlag_CopyWrite,
            PrGpuAccessFlag_CopyRead);
        prGpuCommandsCopyBuffer(commands, buffer, 0, readback, 0, uint64_t(count) * 4);
        prGpuDeviceWait(device, prGpuCommandsSubmit(commands));
        Array<uint32_t> out(count);
        prMemCopy(out.data(), prGpuBufferMap(device, readback), size_t(count) * 4);
        prGpuBufferDestroy(device, readback);
        return out;
    }
};

// An application's own use of Metal on the device's MTLDevice: a command buffer that it begins and ends.
struct AppMetal {
    id<MTLDevice> device;
    id<MTL4CommandAllocator> allocator;
    id<MTL4CommandBuffer> buffer;

    explicit AppMetal(PrGpuDevice* gpu) {
        device = (__bridge id<MTLDevice>)prGpuMetal4Device(gpu);
        allocator = [device newCommandAllocator];
        buffer = [device newCommandBuffer];
        [buffer beginCommandBufferWithAllocator:allocator];
    }
};

// Clears the swap chain's next drawable and presents it.
PrGpuFence presentFrame(PrGpuDevice* device, PrGpuSwapChain* swapChain) {
    PrGpuSwapChainFrame frame{};
    REQUIRE(prGpuSwapChainAcquire(swapChain, PrUVec2{32, 32}, &frame) == PrStatus_Ok);
    PrGpuTexture texture = frame.color;
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

TEST_CASE("metal4 records into the application's command buffer, around the application's own work, and submits it") {
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
    [app.buffer endCommandBuffer];
    PrGpuFence fence = prGpuCommandsSubmit(commands);
    prGpuDeviceWait(f.device, fence);
    CHECK(prGpuDeviceIsComplete(f.device, fence));
    CHECK(f.words(output, 4) == Array<uint32_t>({0, 11, 9, 9}));
    prGpuPipelineDestroy(f.device, pipeline);
    prGpuBufferDestroy(f.device, output);
}

TEST_CASE("metal4 draws into the application's pass, and says which of the pass's state its draws changed") {
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
    MTL4RenderPassDescriptor* rp = [MTL4RenderPassDescriptor new];
    void* native = nullptr;
    REQUIRE(prGpuTextureNative(f.device, target, PrGpuNativeKind_Metal4, &native));
    rp.colorAttachments[0].texture = (__bridge id<MTLTexture>)native;
    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 1, 1);
    id<MTL4RenderCommandEncoder> encoder = [app.buffer renderCommandEncoderWithDescriptor:rp];
    PrGpuPassDesc passDesc{};
    passDesc.native = (__bridge void*)encoder;
    passDesc.nativeFormats.colorCount = 1;
    passDesc.nativeFormats.color[0] = PrGpuFormat_Rgba8;
    passDesc.nativeFormats.samples = 1;
    passDesc.nativeFormats.viewCount = 1;
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
    [app.buffer endCommandBuffer];
    prGpuDeviceWait(f.device, prGpuCommandsSubmit(commands));
    uint32_t pixels[8 * 8]{};
    prGpuTextureRead(f.device, target, PrGpuAccessFlag_CopyRead, PrIVec2{0, 0},
        PrPixels{reinterpret_cast<uint8_t*>(pixels), 8 * 4, PrUVec2{8, 8}});
    CHECK(pixels[0] == 0xff0000ffu);
    CHECK(pixels[7 * 8 + 7] == 0xffff0000u);
    prGpuPipelineDestroy(f.device, pipeline);
    prGpuTextureDestroy(f.device, target);
}

TEST_CASE("metal4 hands a native draw the pass's encoder, and draws after it with its own state again") {
    Fixture f;
    if (f.device == nullptr)
        return;
    REQUIRE(prGpuDeviceHasNative(f.device, PrGpuNativeKind_Metal4));
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
    prGpuPassNativeDraw(pass, PrGpuNativeKind_Metal4, [](void* context, const void* native) {
        const PrGpuMetal4NativePass& objects = *static_cast<const PrGpuMetal4NativePass*>(native);
        CHECK(objects.commandBuffer != nullptr);
        id<MTL4RenderCommandEncoder> encoder = (__bridge id<MTL4RenderCommandEncoder>)objects.encoder;
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

TEST_CASE("metal4 presents frames, with the transaction only in a live resize unless threaded") {
    Fixture f;
    if (f.device == nullptr)
        return;
    @autoreleasepool {
        NSView* view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 32, 32)];
        PrSurface surface{PrSurfaceKind_NsView, (__bridge void*)view};
        PrGpuSwapChainDesc desc{.intent = PrGpuSwapChainIntent_Sdr};
        PrGpuSwapChain* transactional = nullptr;
        REQUIRE(prGpuSwapChainCreate(f.device, &surface, &desc, &transactional) == PrStatus_Ok);
        CAMetalLayer* layer = (CAMetalLayer*)view.layer;
        CHECK(layer.presentsWithTransaction);
        for (int frame = 0; frame < 4; ++frame) {
            PrGpuFence fence = presentFrame(f.device, transactional);
            CHECK(layer.presentsWithTransaction == false);
            prGpuDeviceWait(f.device, fence);
        }
        prGpuSwapChainDestroy(transactional);

        NSView* threadedView = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 32, 32)];
        PrSurface threadedSurface{PrSurfaceKind_NsView, (__bridge void*)threadedView};
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

TEST_CASE("metal4's HDR swap chain reports what its layer holds, for the display that shows it") {
    Fixture f;
    if (f.device == nullptr)
        return;
    @autoreleasepool {
        // A view outside a window is shown on the main screen.
        NSView* view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 32, 32)];
        PrSurface surface{PrSurfaceKind_NsView, (__bridge void*)view};
        PrGpuSwapChainDesc desc{.intent = PrGpuSwapChainIntent_Hdr};
        PrGpuSwapChain* swapChain = nullptr;
        REQUIRE(prGpuSwapChainCreate(f.device, &surface, &desc, &swapChain) == PrStatus_Ok);
        NSScreen* screen = NSScreen.mainScreen;
        bool beyondSdr = screen != nil && screen.maximumPotentialExtendedDynamicRangeColorComponentValue > 1;
        CAMetalLayer* layer = (CAMetalLayer*)view.layer;
        prGpuDeviceWait(f.device, presentFrame(f.device, swapChain));
        PrGpuSwapChainInfo info = prGpuSwapChainInfo(swapChain);
        CHECK(info.format == (beyondSdr ? PrGpuFormat_Rgba16F : PrGpuFormat_Bgra8));
        CHECK(info.space == PrColorSpace_Srgb);
        CHECK(layer.pixelFormat == (beyondSdr ? MTLPixelFormatRGBA16Float : MTLPixelFormatBGRA8Unorm));
        CHECK(layer.wantsExtendedDynamicRangeContent == beyondSdr);
        prGpuSwapChainDestroy(swapChain);
    }
}
