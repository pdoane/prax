// The app host: lib/app opens its immersive space beside a window, a session of lib/surface over the space's native
// object runs on a thread, and lib/gpu clears its frames. Once the thread has drawn its frames, the application closes
// the space. The test passes once the space has closed, and its session with it.

#include "pr/app.h"
#include "pr/atomic.h"
#include "pr/gpu.h"
#include "pr/print.h"
#include "pr/surface.h"
#include "pr/thread.h"

namespace {

using namespace pr;

constexpr uint64_t kFrames = 60;

struct Test {
    PrApp* app = nullptr;
    PrAppWindow* window = nullptr;
    PrGpuDevice* device = nullptr;
    PrGpuSwapChain* windowSwapChain = nullptr;
    PrSurfaceSession* session = nullptr;
    PrGpuSwapChain* swapChain = nullptr;
    PrThread thread{};
    bool drawing = false;               // the thread runs
    bool closing = false;               // the application asked for the space to close
    Atomic<uint64_t> frames;
};

// Prints the test's outcome, which tools/integration reads from the output, and stops the application.
void finish(Test& t, bool passed, PrStringView why) {
    print("integration: {}: {}\n", passed ? "passed" : "failed", why);
    prAppStop(t.app);
}

// Clears a frame of either swap chain, at `extent` for the window's.
bool clear(PrGpuDevice* device, PrGpuSwapChain* swapChain, PrUVec2 extent, PrColor color) {
    PrGpuSwapChainFrame frame{};
    PrGpuCommands* commands = nullptr;
    if (prGpuSwapChainAcquire(swapChain, extent, &frame) != PrStatus_Ok || frame.viewCount == 0 ||
            !prGpuCommandsBegin(device, nullptr, &commands))
        return false;
    PrGpuSubresources all{};
    prGpuCommandsTextureBarrier(commands, frame.color, all, PrGpuAccessFlag_Discard, PrGpuAccessFlag_ColorAttachment);
    PrGpuPassDesc desc = prGpuPassDescColor(frame.color, PrGpuLoad_Clear, color);
    prGpuPassEnd(prGpuPassBegin(commands, &desc));
    prGpuCommandsTextureBarrier(commands, frame.color, all, PrGpuAccessFlag_ColorAttachment, PrGpuAccessFlag_Present);
    prGpuSwapChainPresent(swapChain, commands);
    prGpuCommandsSubmit(commands);
    return true;
}

// The space's thread: draws until the session ends, and wakes the application once it has drawn its frames.
void run(void* context) {
    Test& t = *static_cast<Test*>(context);
    PrSurfaceFrame frame{};
    while (prSurfaceSessionFrameBegin(t.session, &frame) == PrStatus_Ok) {
        bool drawn = clear(t.device, t.swapChain, PrUVec2{0, 0}, PrColor{0.1f, 0.2f, 0.3f, 1});
        prSurfaceSessionFrameEnd(t.session);
        if (drawn && t.frames.fetchAdd(1) + 1 == kFrames)
            prAppWake(t.app);
    }
}

bool open(void*, PrAppWindow*, const PrAppFrame*) {
    return true;
}

PrAppOutput frame(void* context, PrAppWindow*, const PrAppFrame* f) {
    Test& t = *static_cast<Test*>(context);
    float ratio = f->display.pixelRatio;
    clear(t.device, t.windowSwapChain, PrUVec2{uint32_t(f->extent.x * ratio + 0.5f), uint32_t(f->extent.y * ratio + 0.5f)},
        PrColor{0.2f, 0.2f, 0.22f, 1});
    return PrAppOutput{};
}

void windowEvent(void*, PrAppWindow*, const PrAppEvent*) {}

bool start(void* context, PrApp* app) {
    Test& t = *static_cast<Test*>(context);
    t.app = app;
    PrAppWindowDesc desc{
        .title = "App Host",
        .extent = {320, 240},
        .handler = {.context = &t, .open = open, .frame = frame, .event = windowEvent},
    };
    PrSurface surface{};
    PrGpuSwapChainDesc swapChainDesc{.intent = PrGpuSwapChainIntent_Sdr};
    if (prGpuDeviceCreate(PrGpuBackend_Default, nullptr, &t.device) != PrStatus_Ok ||
            prAppWindowCreate(app, &desc, &t.window) != PrStatus_Ok ||
            prAppWindowSurface(t.window, &surface) != PrStatus_Ok ||
            prGpuSwapChainCreate(t.device, &surface, &swapChainDesc, &t.windowSwapChain) != PrStatus_Ok) {
        print("integration: failed: no window to draw\n");
        return false;
    }
    PrStatus status = prAppImmersiveOpen(app);
    if (status != PrStatus_Ok)
        finish(t, false, prStatusName(status));
    return true;
}

void event(void* context, PrApp*, const PrAppEvent* e) {
    Test& t = *static_cast<Test*>(context);
    if (e->kind == PrAppEventKind_ImmersiveOpened) {
        PrSurfaceSessionDesc desc{.native = e->immersive.native, .near = 0.1f, .far = 100};
        PrSurface surface{};
        PrGpuSwapChainDesc swapChainDesc{.intent = PrGpuSwapChainIntent_Sdr};
        if (prSurfaceSessionCreate(PrSurfaceSessionBackend_Default, &desc, &t.session) != PrStatus_Ok) {
            finish(t, false, "no session over the space");
            return;
        }
        surface = prSurfaceSessionSurface(t.session);
        if (prGpuSwapChainCreate(t.device, &surface, &swapChainDesc, &t.swapChain) != PrStatus_Ok) {
            finish(t, false, "no swap chain over the session");
            return;
        }
        t.drawing = true;
        prThreadStart(&t.thread, run, &t);
    } else if (e->kind == PrAppEventKind_Wake && !t.closing && t.frames.load() >= kFrames) {
        t.closing = true;
        prAppImmersiveClose(t.app);
    } else if (e->kind == PrAppEventKind_ImmersiveClosed) {
        if (t.drawing) {
            prThreadJoin(&t.thread);
            t.drawing = false;
        }
        bool passed = t.closing && t.frames.load() >= kFrames;
        finish(t, passed, passed ? PrStringView{"the space opened, drew and closed"} : "the space closed early");
    }
}

void shutdown(void* context, PrApp*) {
    Test& t = *static_cast<Test*>(context);
    if (t.drawing)
        prThreadJoin(&t.thread);
    prGpuSwapChainDestroy(t.swapChain);
    prSurfaceSessionDestroy(t.session);
    prGpuSwapChainDestroy(t.windowSwapChain);
    prGpuDeviceDestroy(t.device);
}

} // namespace

int main() {
    prCrashHandler();
    static Test t;
    PrAppOptions options = prAppOptionsDefault();
    options.windowModel = PrAppWindowModel_Multiple;
    PrApp* app = prAppCreate(PrAppBackend_Default, &options);
    prAppRun(app, {.context = &t, .start = start, .event = event, .shutdown = shutdown});
    prAppDestroy(app);
    return 0;
}
