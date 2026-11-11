// The tests of what only the Vulkan backend has: recording into the application's command buffers and passes,
// presenting to a window's swap chain, and a clean run under the validation layer. The interface's own tests run on
// Vulkan as well.

#include "pr/array.h"
#include "pr/gpu_vulkan.h"
#include "pr/mem.h"

#include "gpu/tests/kTestGpuNativeShaders.h"

#include "tools/harness/test.h"

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace pr_impl {

using namespace pr;

namespace {

// The root data of the test shaders, as test_gpu_native.slang declares it.
struct Push {
    uint32_t output;
    uint32_t count;
    uint32_t scale;
};

struct Uniforms {
    float color[4];
};

// A device on the default adapter, with validation, and the test shaders. A test that finds no Vulkan device skips. The
// device's validation layer reports no error by the end of a test.
struct Fixture {
    PrGpuDevice* device = nullptr;
    PrGpuShaderLibrary* library = nullptr;

    Fixture() {
        PrGpuVulkanOptions options = prGpuVulkanOptionsDefault();
        options.validation = true;
        PrStatus status = prGpuVulkanCreate(&options, &device);
        if (status == PrStatus_NotFound || status == PrStatus_NotSupported) {
            MESSAGE("no Vulkan device with the features that the backend needs");
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
        uint32_t errors = prGpuVulkanValidationErrors(device);
        prGpuDeviceDestroy(device);
        CHECK(errors == 0);
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

// An application's own use of Vulkan on the device's VkDevice: a command pool whose command buffers it records, and
// views of the images that it renders into.
struct AppVulkan {
    VkDevice device = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    PFN_vkDestroyCommandPool destroyPool = nullptr;
    PFN_vkAllocateCommandBuffers allocate = nullptr;
    PFN_vkBeginCommandBuffer beginBuffer = nullptr;
    PFN_vkEndCommandBuffer endBuffer = nullptr;
    PFN_vkCreateImageView createView = nullptr;
    PFN_vkDestroyImageView destroyImageView = nullptr;
    PFN_vkCmdBeginRendering cmdBeginRendering = nullptr;
    PFN_vkCmdEndRendering cmdEndRendering = nullptr;
    PFN_vkCmdSetScissor cmdSetScissor = nullptr;

    explicit AppVulkan(PrGpuDevice* gpu) {
        PFN_vkGetInstanceProcAddr getInstanceProcAddr =
            reinterpret_cast<PFN_vkGetInstanceProcAddr>(prGpuVulkanGetInstanceProcAddr(gpu));
        VkInstance instance = static_cast<VkInstance>(prGpuVulkanInstance(gpu));
        device = static_cast<VkDevice>(prGpuVulkanDevice(gpu));
        PFN_vkGetDeviceProcAddr get =
            reinterpret_cast<PFN_vkGetDeviceProcAddr>(getInstanceProcAddr(instance, "vkGetDeviceProcAddr"));
        PFN_vkCreateCommandPool createPool =
            reinterpret_cast<PFN_vkCreateCommandPool>(get(device, "vkCreateCommandPool"));
        destroyPool = reinterpret_cast<PFN_vkDestroyCommandPool>(get(device, "vkDestroyCommandPool"));
        allocate = reinterpret_cast<PFN_vkAllocateCommandBuffers>(get(device, "vkAllocateCommandBuffers"));
        beginBuffer = reinterpret_cast<PFN_vkBeginCommandBuffer>(get(device, "vkBeginCommandBuffer"));
        endBuffer = reinterpret_cast<PFN_vkEndCommandBuffer>(get(device, "vkEndCommandBuffer"));
        createView = reinterpret_cast<PFN_vkCreateImageView>(get(device, "vkCreateImageView"));
        destroyImageView = reinterpret_cast<PFN_vkDestroyImageView>(get(device, "vkDestroyImageView"));
        cmdBeginRendering = reinterpret_cast<PFN_vkCmdBeginRendering>(get(device, "vkCmdBeginRendering"));
        cmdEndRendering = reinterpret_cast<PFN_vkCmdEndRendering>(get(device, "vkCmdEndRendering"));
        cmdSetScissor = reinterpret_cast<PFN_vkCmdSetScissor>(get(device, "vkCmdSetScissor"));
        VkCommandPoolCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        REQUIRE(createPool(device, &info, nullptr, &pool) == VK_SUCCESS);
    }

    ~AppVulkan() {
        destroyPool(device, pool, nullptr);
    }

    // Returns a command buffer of the pool, recording.
    VkCommandBuffer begin() {
        VkCommandBufferAllocateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        info.commandPool = pool;
        info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        info.commandBufferCount = 1;
        VkCommandBuffer buffer = VK_NULL_HANDLE;
        REQUIRE(allocate(device, &info, &buffer) == VK_SUCCESS);
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginBuffer(buffer, &begin);
        return buffer;
    }

    // Returns a view of an 8-bit RGBA image's first level and layer.
    VkImageView view(VkImage image) {
        VkImageViewCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        info.image = image;
        info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        info.format = VK_FORMAT_R8G8B8A8_UNORM;
        info.subresourceRange = VkImageSubresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView made = VK_NULL_HANDLE;
        REQUIRE(createView(device, &info, nullptr, &made) == VK_SUCCESS);
        return made;
    }
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("vulkan records into the application's command buffer, around the application's own work, and submits it") {
    Fixture f;
    if (f.device == nullptr)
        return;
    AppVulkan app(f.device);
    VkCommandBuffer native = app.begin();

    // An upload made before the submission is visible to all of the submission's work, including the work in the
    // application's native command buffer.
    PrGpuBuffer output = f.buffer(16, PrGpuBufferUsageFlag_Storage);
    PrGpuPipeline pipeline{};
    REQUIRE(prGpuPipelineCreateCompute(f.device, f.library, "fill", "fill", &pipeline) == PrStatus_Ok);
    PrGpuCommandsOptions options = prGpuCommandsOptionsDefault();
    options.native = native;
    PrGpuCommands* commands = nullptr;
    REQUIRE(prGpuCommandsBegin(f.device, &options, &commands));
    uint32_t seed[4] = {9, 9, 9, 9};
    prGpuBufferUpload(f.device, output, 0, seed, sizeof(seed));
    Push push{.output = prGpuBufferStorageHandle(f.device, output), .count = 2, .scale = 11};
    prGpuCommandsSetPipeline(commands, pipeline);
    prGpuCommandsSetPush(commands, &push, sizeof(push));
    prGpuCommandsDispatch(commands, PrGpuExtent{1, 1, 1});
    PrGpuNativeStateFlags changed = prGpuNativeStateSync(commands);
    CHECK((changed & PrGpuNativeStateFlag_Pipeline) != 0);
    CHECK((changed & PrGpuNativeStateFlag_Push) != 0);
    app.endBuffer(native);
    PrGpuFence fence = prGpuCommandsSubmit(commands);
    prGpuDeviceWait(f.device, fence);
    CHECK(prGpuDeviceIsComplete(f.device, fence));
    CHECK(f.words(output, 4) == Array<uint32_t>({0, 11, 9, 9}));
    prGpuPipelineDestroy(f.device, pipeline);
    prGpuBufferDestroy(f.device, output);
}

TEST_CASE("vulkan draws into the application's pass, and says which of the pass's state its draws changed") {
    Fixture f;
    if (f.device == nullptr)
        return;
    AppVulkan app(f.device);
    PrGpuTexture target = f.target();
    PrGpuPipeline pipeline = f.upperHalf();
    PrGpuBuffer readback = f.buffer(8 * 8 * 4, 0, PrGpuMemory_Readback);
    uint64_t image = 0;
    REQUIRE(prGpuTextureNative(f.device, target, PrGpuNativeKind_Vulkan, &image));
    VkImageView view = app.view(reinterpret_cast<VkImage>(image));

    VkCommandBuffer native = app.begin();
    PrGpuCommandsOptions options = prGpuCommandsOptionsDefault();
    options.native = native;
    PrGpuCommands* commands = nullptr;
    REQUIRE(prGpuCommandsBegin(f.device, &options, &commands));
    prGpuCommandsTextureBarrier(commands, target, PrGpuSubresources{}, PrGpuAccessFlag_Discard,
        PrGpuAccessFlag_ColorAttachment);
    prGpuNativeStateSync(commands);

    // The application begins its pass, which clears the target to blue, and the device draws the upper half red into
    // it.
    VkRenderingAttachmentInfo attachment{};
    attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    attachment.imageView = view;
    attachment.imageLayout = VkImageLayout(prGpuVulkanLayout(PrGpuAccessFlag_ColorAttachment));
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.clearValue.color = VkClearColorValue{{0, 0, 1, 1}};
    VkRenderingInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea = VkRect2D{{0, 0}, {8, 8}};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &attachment;
    app.cmdBeginRendering(native, &rendering);
    PrGpuPassDesc passDesc{};
    passDesc.native = native;
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
    CHECK((changed & PrGpuNativeStateFlag_Viewport) != 0);
    CHECK((changed & PrGpuNativeStateFlag_Scissor) != 0);
    CHECK((changed & PrGpuNativeStateFlag_IndexBuffer) == 0);
    CHECK(prGpuNativeStateSync(commands) == 0);
    prGpuPassEnd(pass);
    app.cmdEndRendering(native);

    prGpuCommandsTextureBarrier(commands, target, PrGpuSubresources{}, PrGpuAccessFlag_ColorAttachment,
        PrGpuAccessFlag_CopyRead);
    PrGpuTextureRegion region = prGpuTextureRegion2d(PrUVec2{0, 0}, PrUVec2{8, 8});
    prGpuCommandsCopyTextureToBuffer(commands, target, &region, readback, 0, 32, 0);
    prGpuNativeStateSync(commands);
    app.endBuffer(native);
    prGpuDeviceWait(f.device, prGpuCommandsSubmit(commands));
    const uint32_t* pixels = static_cast<const uint32_t*>(prGpuBufferMap(f.device, readback));
    CHECK(pixels[0] == 0xff0000ffu);
    CHECK(pixels[7 * 8 + 7] == 0xffff0000u);
    app.destroyImageView(app.device, view, nullptr);
    prGpuBufferDestroy(f.device, readback);
    prGpuPipelineDestroy(f.device, pipeline);
    prGpuTextureDestroy(f.device, target);
}

TEST_CASE("vulkan hands a native draw the pass's command buffer, and draws after it with its own state again") {
    Fixture f;
    if (f.device == nullptr)
        return;
    REQUIRE(prGpuDeviceHasNative(f.device, PrGpuNativeKind_Vulkan));
    AppVulkan app(f.device);
    PrGpuTexture target = f.target();
    PrGpuPipeline pipeline = f.upperHalf();
    PrGpuCommands* commands = nullptr;
    REQUIRE(prGpuCommandsBegin(f.device, nullptr, &commands));
    prGpuCommandsTextureBarrier(commands, target, PrGpuSubresources{}, PrGpuAccessFlag_Discard,
        PrGpuAccessFlag_ColorAttachment);
    PrGpuPassDesc desc = prGpuPassDescColor(target, PrGpuLoad_Clear, PrColor{0, 0, 1, 1});
    PrGpuPass* pass = prGpuPassBegin(commands, &desc);

    // The native draw narrows the scissor to the left half, which the device's draw after it does not keep.
    struct Drawn {
        AppVulkan* app;
        bool drawn;
    } drawn{&app, false};
    prGpuPassNativeDraw(pass, PrGpuNativeKind_Vulkan, [](void* context, const void* native) {
        const PrGpuVulkanNativePass& objects = *static_cast<const PrGpuVulkanNativePass*>(native);
        Drawn& d = *static_cast<Drawn*>(context);
        CHECK(objects.pipelineLayout != 0);
        VkRect2D left{{0, 0}, {4, 8}};
        d.app->cmdSetScissor(reinterpret_cast<VkCommandBuffer>(objects.commandBuffer), 0, 1, &left);
        d.drawn = true;
    }, &drawn);
    CHECK(drawn.drawn);
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

#if defined(_WIN32)
TEST_CASE("vulkan presents frames to a window's swap chain, and resizes it") {
    Fixture f;
    if (f.device == nullptr)
        return;
    WNDCLASSW type{};
    type.lpfnWndProc = DefWindowProcW;
    type.hInstance = GetModuleHandleW(nullptr);
    type.lpszClassName = L"PrGpuVulkanTest";
    RegisterClassW(&type);
    // A popup window's client area is the whole window, so the surface's extent is the window's size.
    HWND window = CreateWindowExW(0, type.lpszClassName, L"Vulkan test", WS_POPUP, 0, 0, 320, 240, nullptr, nullptr,
        type.hInstance, nullptr);
    REQUIRE(window != nullptr);
    PrSurface surface{PrSurfaceKind_Hwnd, window};
    PrGpuSwapChainDesc desc{.intent = PrGpuSwapChainIntent_Sdr};
    PrGpuSwapChain* swapChain = nullptr;
    REQUIRE(prGpuSwapChainCreate(f.device, &surface, &desc, &swapChain) == PrStatus_Ok);
    CHECK(prGpuSwapChainInfo(swapChain).space == PrColorSpace_Srgb);
    const PrUVec2 extents[] = {{320, 240}, {320, 240}, {200, 100}, {200, 100}, {200, 100}};
    for (PrUVec2 extent : extents) {
        SetWindowPos(window, nullptr, 0, 0, int(extent.x), int(extent.y), SWP_NOMOVE | SWP_NOZORDER);
        prGpuSwapChainWait(swapChain);
        PrGpuSwapChainFrame frame{};
        REQUIRE(prGpuSwapChainAcquire(swapChain, extent, &frame) == PrStatus_Ok);
        PrGpuTexture texture = frame.color;
        CHECK(prGpuTextureExtent(f.device, texture).x == extent.x);
        PrGpuCommands* commands = nullptr;
        REQUIRE(prGpuCommandsBegin(f.device, nullptr, &commands));
        prGpuCommandsTextureBarrier(commands, texture, PrGpuSubresources{}, PrGpuAccessFlag_Discard,
            PrGpuAccessFlag_ColorAttachment);
        PrGpuPassDesc pass = prGpuPassDescColor(texture, PrGpuLoad_Clear, PrColor{0.2f, 0.4f, 0.6f, 1});
        prGpuPassEnd(prGpuPassBegin(commands, &pass));
        prGpuCommandsTextureBarrier(commands, texture, PrGpuSubresources{}, PrGpuAccessFlag_ColorAttachment,
            PrGpuAccessFlag_Present);
        prGpuSwapChainPresent(swapChain, commands);
        prGpuCommandsSubmit(commands);
    }
    prGpuDeviceWaitIdle(f.device);
    prGpuSwapChainDestroy(swapChain);
    DestroyWindow(window);
}
#endif
