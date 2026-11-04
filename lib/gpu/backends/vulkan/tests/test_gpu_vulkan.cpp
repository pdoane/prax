// The tests of what only the Vulkan backend has: recording into the host's command buffers and passes, presenting to a
// window's swap chain, and a clean run under the validation layer. The interface's own tests run on Vulkan as well.

#include "pr/gpu_vulkan.h"

#include "gpu/tests/kTestGpuHostShaders.h"

#include "tools/harness/test.h"

#define VK_NO_PROTOTYPES
#include <cstdint>
#include <cstring>
#include <vector>
#include <vulkan/vulkan_core.h>

#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace pr_impl {

namespace {

// The root data of the test shaders, as test_gpu_host.slang declares it.
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
        REQUIRE(prGpuShaderLibraryCreate(device, kTestGpuHostShaders, sizeof(kTestGpuHostShaders), &library) ==
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

// A host's own use of Vulkan on the device's VkDevice: a command pool whose command buffers it records, and views of
// the images that it renders into.
struct HostVulkan {
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

    explicit HostVulkan(PrGpuDevice* gpu) {
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
        VkCommandPoolCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        REQUIRE(createPool(device, &info, nullptr, &pool) == VK_SUCCESS);
    }

    ~HostVulkan() {
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

TEST_CASE("vulkan records into the host's command buffer, around the host's own work, and submits it") {
    Fixture f;
    if (f.device == nullptr)
        return;
    HostVulkan host(f.device);
    VkCommandBuffer native = host.begin();

    // An upload made before the submission is visible to all of its work, the host's native command buffer's too.
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
    PrGpuHostStateFlags changed = prGpuHostStateSync(commands);
    CHECK((changed & PrGpuHostStateFlag_Pipeline) != 0);
    CHECK((changed & PrGpuHostStateFlag_Push) != 0);
    host.endBuffer(native);
    PrGpuFence fence = prGpuCommandsSubmit(commands);
    prGpuDeviceWait(f.device, fence);
    CHECK(prGpuDeviceIsComplete(f.device, fence));
    CHECK(f.words(output, 4) == std::vector<uint32_t>{0, 11, 9, 9});
    prGpuPipelineDestroy(f.device, pipeline);
    prGpuBufferDestroy(f.device, output);
}

TEST_CASE("vulkan draws into the host's pass, and says which of the pass's state its draws changed") {
    Fixture f;
    if (f.device == nullptr)
        return;
    HostVulkan host(f.device);
    PrGpuTexture target = f.target();
    PrGpuPipeline pipeline = f.upperHalf();
    PrGpuBuffer readback = f.buffer(8 * 8 * 4, 0, PrGpuMemory_Readback);
    VkImageView view = host.view(reinterpret_cast<VkImage>(prGpuVulkanImage(f.device, target)));

    VkCommandBuffer native = host.begin();
    PrGpuCommandsOptions options = prGpuCommandsOptionsDefault();
    options.native = native;
    PrGpuCommands* commands = nullptr;
    REQUIRE(prGpuCommandsBegin(f.device, &options, &commands));
    prGpuCommandsTextureBarrier(commands, target, PrGpuSubresources{}, PrGpuAccessFlag_Discard,
        PrGpuAccessFlag_ColorAttachment);
    prGpuHostStateSync(commands);

    // The host begins its pass, which clears the target to blue, and the device draws the upper half red into it.
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
    host.cmdBeginRendering(native, &rendering);
    PrGpuPassDesc passDesc{};
    passDesc.host = native;
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
    CHECK((changed & PrGpuHostStateFlag_Viewport) != 0);
    CHECK((changed & PrGpuHostStateFlag_Scissor) != 0);
    CHECK((changed & PrGpuHostStateFlag_IndexBuffer) == 0);
    CHECK(prGpuHostStateSync(commands) == 0);
    prGpuPassEnd(pass);
    host.cmdEndRendering(native);

    prGpuCommandsTextureBarrier(commands, target, PrGpuSubresources{}, PrGpuAccessFlag_ColorAttachment,
        PrGpuAccessFlag_CopyRead);
    PrGpuTextureRegion region = prGpuTextureRegion2d(PrUVec2{0, 0}, PrUVec2{8, 8});
    prGpuCommandsCopyTextureToBuffer(commands, target, &region, readback, 0, 32, 0);
    prGpuHostStateSync(commands);
    host.endBuffer(native);
    prGpuDeviceWait(f.device, prGpuCommandsSubmit(commands));
    const uint32_t* pixels = static_cast<const uint32_t*>(prGpuBufferMap(f.device, readback));
    CHECK(pixels[0] == 0xff0000ffu);
    CHECK(pixels[7 * 8 + 7] == 0xffff0000u);
    host.destroyImageView(host.device, view, nullptr);
    prGpuBufferDestroy(f.device, readback);
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
    PrAppSurface surface{PrAppSurfaceKind_Hwnd, window};
    PrGpuSwapChainDesc desc{.intent = PrGpuSwapChainIntent_Sdr};
    PrGpuSwapChain* swapChain = nullptr;
    REQUIRE(prGpuSwapChainCreate(f.device, &surface, &desc, &swapChain) == PrStatus_Ok);
    CHECK(prGpuSwapChainInfo(swapChain).space == PrColorSpace_Srgb);
    const PrUVec2 extents[] = {{320, 240}, {320, 240}, {200, 100}, {200, 100}, {200, 100}};
    for (PrUVec2 extent : extents) {
        SetWindowPos(window, nullptr, 0, 0, int(extent.x), int(extent.y), SWP_NOMOVE | SWP_NOZORDER);
        prGpuSwapChainWait(swapChain);
        PrGpuTexture texture{};
        REQUIRE(prGpuSwapChainAcquire(swapChain, extent, &texture) == PrStatus_Ok);
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
