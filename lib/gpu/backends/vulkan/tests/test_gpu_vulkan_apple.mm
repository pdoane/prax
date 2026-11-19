// The tests of the Vulkan backend that present to a view of AppKit, through MoltenVK, under the validation layer.

#include "pr/gpu_vulkan.h"
#include "pr/mem.h"
#include "pr/surface.h"

#include "tools/harness/test.h"

#import <AppKit/AppKit.h>

TEST_CASE("vulkan captures a frame into the swap chain's own texture, which never takes a present's layout") {
    PrGpuVulkanOptions options = prGpuVulkanOptionsDefault();
    options.validation = true;
    PrGpuDevice* device = prGpuVulkanCreate(&options);
    if (prGpuDeviceLostStatus(device) == PrStatus_NotSupported) {
        MESSAGE("no Vulkan device with the features that the backend needs");
        prGpuDeviceDestroy(device);
        return;
    }
    @autoreleasepool {
        // The view is in no window, so its layer shows nothing.
        NSView* view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 32, 32)];
        PrSurface surface{PrSurfaceKind_NsView, (__bridge void*)view};
        PrGpuSwapChainDesc desc{.intent = PrGpuSwapChainIntent_Sdr};
        PrGpuSwapChain* swapChain = nullptr;
        REQUIRE(prGpuSwapChainCreate(device, &surface, &desc, &swapChain) == PrStatus_Ok);
        struct Captured {
            uint32_t calls = 0;
            PrGpuFormat format = PrGpuFormat_None;
            PrUVec2 extent{};
            uint8_t first[4] = {};
        } captured;
        prGpuSwapChainRequestCapture(swapChain, [](void* PR_NULLABLE context, PrPixelView texels, PrGpuFormat format) {
            Captured& c = *static_cast<Captured*>(context);
            ++c.calls;
            c.format = format;
            c.extent = texels.extent;
            if (texels.data != nullptr)
                prMemCopy(c.first, texels.data, 4);
        }, &captured);
        prGpuSwapChainWait(swapChain);
        PrGpuSwapChainFrame frame{};
        REQUIRE(prGpuSwapChainAcquire(swapChain, PrUVec2{24, 16}, &frame) == PrStatus_Ok);
        PrGpuTexture texture = frame.color;
        PrGpuCommands* commands = nullptr;
        REQUIRE(prGpuCommandsBegin(device, nullptr, &commands));
        prGpuCommandsTextureBarrier(commands, texture, PrGpuSubresources{}, PrGpuAccessFlag_Discard,
            PrGpuAccessFlag_ColorAttachment);
        PrGpuPassDesc pass = prGpuPassDescColor(texture, PrGpuLoad_Clear, PrColor{0, 1, 0, 1});
        prGpuPassEnd(prGpuPassBegin(commands, &pass));
        prGpuCommandsTextureBarrier(commands, texture, PrGpuSubresources{}, PrGpuAccessFlag_ColorAttachment,
            PrGpuAccessFlag_Present);
        prGpuSwapChainPresent(swapChain, commands);
        prGpuDeviceWait(device, prGpuCommandsSubmit(commands));
        prGpuDeviceWaitIdle(device);
        CHECK(captured.calls == 1);
        CHECK(captured.extent.x == 24);
        CHECK(captured.extent.y == 16);
        // Green, in the swap chain's format, whose blue and red channels are where its format places them.
        CHECK(captured.first[1] == 255);
        CHECK(captured.first[3] == 255);
        CHECK(captured.first[0] + captured.first[2] == 0);
        prGpuSwapChainDestroy(swapChain);
    }
    CHECK(prGpuVulkanValidationErrors(device) == 0);
    prGpuDeviceDestroy(device);
}
