// The Swift host's frames: a session of lib/surface over the SwiftUI application's layer renderer, and a device of
// lib/gpu over the layer renderer's Metal device, with no lib/app in the process. Each frame clears its color and
// depth. The test passes once its frames have presented, each with at least one view.

#include "pr/gpu.h"
#include "pr/gpu_metal.h"
#include "pr/print.h"
#include "pr/surface_compositor_services.h"

#import <CompositorServices/CompositorServices.h>

// What the SwiftUI application calls, as its bridging header declares it (swift_host.h).
extern "C" bool prSwiftHostRun(void* renderer);

using namespace pr;

namespace {

constexpr uint32_t kFrames = 120;

// Prints the test's outcome, which tools/integration reads from the output, and returns it.
bool finish(bool passed, PrStringView why) {
    print("integration: {}: {}\n", passed ? "passed" : "failed", why);
    return passed;
}

// Clears the frame's color and depth, and presents it.
bool drawFrame(PrGpuDevice* device, PrGpuSwapChain* swapChain) {
    PrGpuSwapChainFrame frame{};
    PrGpuCommands* commands = nullptr;
    if (prGpuSwapChainAcquire(swapChain, PrUVec2{0, 0}, &frame) != PrStatus_Ok || frame.viewCount == 0 ||
            !prGpuCommandsBegin(device, nullptr, &commands))
        return false;
    PrGpuSubresources all{};
    prGpuCommandsTextureBarrier(commands, frame.color, all, PrGpuAccessFlag_Discard, PrGpuAccessFlag_ColorAttachment);
    prGpuCommandsTextureBarrier(commands, frame.depth, all, PrGpuAccessFlag_Discard, PrGpuAccessFlag_DepthStencilWrite);
    PrGpuPassDesc desc = prGpuPassDescColor(frame.color, PrGpuLoad_Clear, PrColor{0.1f, 0.3f, 0.2f, 1});
    desc.depthStencil = PrGpuAttachment{.texture = frame.depth, .load = PrGpuLoad_Clear, .store = PrGpuStore_Keep};
    prGpuPassEnd(prGpuPassBegin(commands, &desc));
    prGpuCommandsTextureBarrier(commands, frame.color, all, PrGpuAccessFlag_ColorAttachment, PrGpuAccessFlag_Present);
    prGpuSwapChainPresent(swapChain, commands);
    prGpuCommandsSubmit(commands);
    return true;
}

} // namespace

bool prSwiftHostRun(void* renderer) {
    PrGpuMetalOptions options = prGpuMetalOptionsDefault();
    options.device = (__bridge void*)cp_layer_renderer_get_device((__bridge cp_layer_renderer_t)renderer);
    PrGpuDevice* device = nullptr;
    PrSurfaceSession* session = nullptr;
    PrGpuSwapChain* swapChain = nullptr;
    PrSurfaceSessionDesc sessionDesc{.native = renderer, .near = 0.1f, .far = 100};
    if (prGpuMetalCreate(&options, &device) != PrStatus_Ok)
        return finish(false, "no device");
    if (prSurfaceSessionCreate(PrSurfaceSessionBackend_CompositorServices, &sessionDesc, &session) != PrStatus_Ok)
        return finish(false, "no session");
    PrSurface surface = prSurfaceSessionSurface(session);
    PrGpuSwapChainDesc desc{.intent = PrGpuSwapChainIntent_Sdr};
    if (prGpuSwapChainCreate(device, &surface, &desc, &swapChain) != PrStatus_Ok)
        return finish(false, "no swap chain");
    PrSurfaceFrame frame{};
    for (uint32_t i = 0; i < kFrames; ++i) {
        if (prSurfaceSessionFrameBegin(session, &frame) != PrStatus_Ok)
            return finish(false, "the session ended");
        bool drawn = frame.viewCount != 0 && drawFrame(device, swapChain);
        prSurfaceSessionFrameEnd(session);
        if (!drawn)
            return finish(false, "a frame was not drawn");
    }
    // The process ends with the outcome, so the session and the device stay to the end.
    return finish(true, "frames presented");
}
