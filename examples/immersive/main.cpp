// The immersive example: a spinning cube over a floor, in an immersive space around the user of a headset, beside a
// window that opens and closes the space.
//
// main() is the composition root. lib/app opens the window and the space. Once the space opens, its native object is
// the ground of a session of lib/surface, and a thread draws the session's frames with lib/gpu until the space closes.
// A frame of a layered layout holds every view in a layer of one texture, which one pass of the frame's views draws.
// Any other frame draws each view on its own, into its viewport. The window shows whether the space is open, and a
// press in it opens or closes the space. On a platform without immersive spaces, the window says so.

#include "pr/app.h"
#include "pr/atomic.h"
#include "pr/float.h"
#include "pr/gpu.h"
#include "pr/print.h"
#include "pr/surface.h"
#include "pr/thread.h"

#include "examples/immersive/kImmersiveShaders.h"

namespace {

using namespace pr;

// The shaders' uniforms (immersive.slang).
struct Uniforms {
    PrMatrix4 viewProjection[kPrGpuMaxViews];
    PrMatrix4 model;
    float floor[4];             // the floor's height in the world in x
};

// The shaders' push data.
struct Push {
    uint32_t firstView;
};

// The pipelines of a frame's textures and views.
struct Pipelines {
    PrGpuAttachmentFormats formats;
    PrGpuPipeline cube;
    PrGpuPipeline floor;
};

// The immersive space's session and what draws it, which its thread uses while the space is open.
struct Space {
    bool open = false;
    PrGpuDevice* device = nullptr;
    PrGpuShaderLibrary* library = nullptr;
    PrSurfaceSession* session = nullptr;
    PrGpuSwapChain* swapChain = nullptr;
    Pipelines pipelines{};
    PrThread thread{};
    Atomic<bool> stopping;
    bool placed = false;            // the cube and the floor are placed from the first frame's views
    PrVec3 cube{};
    float floor = 0;
    double start = 0;
    uint64_t frames = 0;
};

// The application: its window and the device that draws it and the space.
struct Example {
    PrApp* app = nullptr;
    PrAppWindow* window = nullptr;
    PrGpuDevice* device = nullptr;
    PrGpuShaderLibrary* library = nullptr;
    PrGpuSwapChain* swapChain = nullptr;
    bool supported = true;          // the platform has immersive spaces
    bool opening = false;
    Space space;
};

bool sameFormats(const PrGpuAttachmentFormats& a, const PrGpuAttachmentFormats& b) {
    return a.colorCount == b.colorCount && a.color[0] == b.color[0] && a.depthStencil == b.depthStencil &&
        a.samples == b.samples && a.viewCount == b.viewCount;
}

void pipelinesDestroy(Space& s) {
    prGpuPipelineDestroy(s.device, s.pipelines.cube);
    prGpuPipelineDestroy(s.device, s.pipelines.floor);
    s.pipelines = Pipelines{};
}

// Makes the pipelines for `formats` if they are not made for them.
bool pipelinesFor(Space& s, const PrGpuAttachmentFormats& formats) {
    if (s.pipelines.cube.id != 0 && sameFormats(s.pipelines.formats, formats))
        return true;
    pipelinesDestroy(s);
    PrGpuGraphicsPipelineDesc desc = prGpuGraphicsPipelineDescDefault(s.library);
    desc.formats = formats;
    // The compositor's depth is reversed: 1 at the near plane and 0 at the far.
    desc.depthTest = true;
    desc.depthWrite = true;
    desc.depthCompare = PrGpuCompare_Greater;
    desc.vertex = "cubeVertex";
    desc.fragment = "cubeFragment";
    desc.name = "cube";
    if (prGpuPipelineCreateGraphics(s.device, &desc, &s.pipelines.cube) != PrStatus_Ok)
        return false;
    desc.vertex = "floorVertex";
    desc.fragment = "floorFragment";
    desc.name = "floor";
    if (prGpuPipelineCreateGraphics(s.device, &desc, &s.pipelines.floor) != PrStatus_Ok)
        return false;
    s.pipelines.formats = formats;
    return true;
}

// Returns true if one pass of the frame's views draws them: each view is the whole of its own layer, in order.
bool layered(const Space& s, const PrGpuSwapChainFrame& frame) {
    PrGpuTextureDesc color = prGpuTextureDesc(s.device, frame.color);
    if (frame.viewCount > prGpuDeviceInfo(s.device).maxViews || color.kind != PrGpuTextureKind_2dArray)
        return false;
    for (uint8_t i = 0; i < frame.viewCount; ++i) {
        const PrIRect& v = frame.views[i].viewport;
        if (frame.views[i].layer != i || v.left != 0 || v.top != 0 || v.right != int32_t(color.extent.width) ||
                v.bottom != int32_t(color.extent.height))
            return false;
    }
    return true;
}

PrVec3 column(const PrMatrix4& m, int c) {
    return PrVec3{m.m[0][c], m.m[1][c], m.m[2][c]};
}

// Places the cube ahead of the first view and to its right, beside the application's window, at the eye's height, and
// the floor beneath. Without tracking, the eye is at the world's origin, and the floor is placed at a standing eye's
// height below it.
void place(Space& s, const PrSurfaceFrame& frame) {
    PrMatrix4 worldFromView;
    if (!prMatrix4Invert(&frame.views[0].view, &worldFromView))
        worldFromView = prMatrix4Identity();
    PrVec3 eye = column(worldFromView, 3);
    PrVec3 back = column(worldFromView, 2);
    float flat = prF32Hypot(back.x, back.z);
    PrVec3 ahead = flat > 0.01f ? PrVec3{-back.x / flat, 0, -back.z / flat} : PrVec3{0, 0, -1};
    PrVec3 right{-ahead.z, 0, ahead.x};
    s.cube = PrVec3{eye.x + ahead.x * 1.4f + right.x * 0.7f, eye.y, eye.z + ahead.z * 1.4f + right.z * 0.7f};
    s.floor = eye.y > 0.5f ? 0 : eye.y - 1.6f;
    s.start = frame.time;
    s.placed = true;
}

Uniforms uniformsOf(const Space& s, const PrSurfaceFrame& frame) {
    Uniforms u{};
    for (uint8_t i = 0; i < frame.viewCount && i < kPrGpuMaxViews; ++i)
        u.viewProjection[i] = prMatrix4Concat(&frame.views[i].view, &frame.views[i].projection);
    float seconds = float((frame.time - s.start) / 1000);
    PrMatrix4 scale = prMatrix4Scale(PrVec3{0.2f, 0.2f, 0.2f});
    PrMatrix4 tilt = prMatrix4Rotate(PrVec3{1, 0, 0}, seconds * 0.6f);
    PrMatrix4 spin = prMatrix4Rotate(PrVec3{0, 1, 0}, seconds);
    PrMatrix4 move = prMatrix4Translate(s.cube);
    PrMatrix4 tilted = prMatrix4Concat(&scale, &tilt);
    PrMatrix4 spun = prMatrix4Concat(&tilted, &spin);
    u.model = prMatrix4Concat(&spun, &move);
    u.floor[0] = s.floor;
    return u;
}

void drawScene(Space& s, PrGpuCommands* commands, PrGpuPass* pass, uint32_t firstView) {
    Push push{firstView};
    prGpuCommandsSetPush(commands, &push, sizeof(push));
    prGpuCommandsSetPipeline(commands, s.pipelines.floor);
    prGpuPassDraw(pass, 6, 1, 0, 0);
    prGpuCommandsSetPipeline(commands, s.pipelines.cube);
    prGpuPassDraw(pass, 36, 1, 0, 0);
}

PrGpuPassDesc passOf(const PrGpuSwapChainFrame& frame, uint8_t views, uint16_t layer, bool clear) {
    PrGpuLoad load = clear ? PrGpuLoad_Clear : PrGpuLoad_Keep;
    PrGpuPassDesc desc = prGpuPassDescColor(frame.color, load, PrColor{0.02f, 0.025f, 0.04f, 1});
    desc.viewCount = views;
    desc.color[0].layer = layer;
    desc.depthStencil = PrGpuAttachment{.texture = frame.depth, .layer = layer, .load = load, .store = PrGpuStore_Keep};
    desc.clearDepth = 0;
    desc.name = "immersive";
    return desc;
}

PrRect rectOf(PrIRect r) {
    return PrRect{float(r.left), float(r.top), float(r.right), float(r.bottom)};
}

// Draws one frame that the session began.
void drawFrame(Space& s, const PrSurfaceFrame& surfaceFrame) {
    PrGpuSwapChainFrame frame{};
    if (prGpuSwapChainAcquire(s.swapChain, PrUVec2{0, 0}, &frame) != PrStatus_Ok)
        return;
    bool onePass = layered(s, frame);
    PrGpuAttachmentFormats formats{
        .colorCount = 1,
        .color = {prGpuTextureDesc(s.device, frame.color).format},
        .depthStencil = prGpuTextureDesc(s.device, frame.depth).format,
        .samples = 1,
        .viewCount = uint8_t(onePass ? frame.viewCount : 1),
    };
    PrGpuCommands* commands = nullptr;
    if (!pipelinesFor(s, formats) || !prGpuCommandsBegin(s.device, nullptr, &commands))
        return;
    if (!s.placed)
        place(s, surfaceFrame);
    Uniforms uniforms = uniformsOf(s, surfaceFrame);
    PrGpuSubresources all{};
    prGpuCommandsTextureBarrier(commands, frame.color, all, PrGpuAccessFlag_Discard, PrGpuAccessFlag_ColorAttachment);
    prGpuCommandsTextureBarrier(commands, frame.depth, all, PrGpuAccessFlag_Discard, PrGpuAccessFlag_DepthStencilWrite);
    prGpuCommandsSetUniforms(commands, &uniforms, sizeof(uniforms));
    if (onePass) {
        PrGpuPassDesc desc = passOf(frame, frame.viewCount, 0, true);
        PrGpuPass* pass = prGpuPassBegin(commands, &desc);
        drawScene(s, commands, pass, 0);
        prGpuPassEnd(pass);
    } else {
        // Each view clears its layer only if no view before it drew there.
        for (uint8_t i = 0; i < frame.viewCount; ++i) {
            bool first = true;
            for (uint8_t j = 0; j < i; ++j)
                first = first && frame.views[j].layer != frame.views[i].layer;
            PrGpuPassDesc desc = passOf(frame, 1, frame.views[i].layer, first);
            PrGpuPass* pass = prGpuPassBegin(commands, &desc);
            prGpuPassSetViewport(pass, rectOf(frame.views[i].viewport), 0, 1);
            prGpuPassSetScissor(pass, frame.views[i].viewport);
            drawScene(s, commands, pass, i);
            prGpuPassEnd(pass);
        }
    }
    prGpuCommandsTextureBarrier(commands, frame.color, all, PrGpuAccessFlag_ColorAttachment, PrGpuAccessFlag_Present);
    prGpuSwapChainPresent(s.swapChain, commands);
    prGpuCommandsSubmit(commands);
}

// The space's thread: draws the session's frames until the session ends or the application stops it.
void runSpace(void* context) {
    Space& s = *static_cast<Space*>(context);
    PrSurfaceFrame frame{};
    while (!s.stopping.load() && prSurfaceSessionFrameBegin(s.session, &frame) == PrStatus_Ok) {
        drawFrame(s, frame);
        prSurfaceSessionFrameEnd(s.session);
        if (++s.frames % 600 == 0)
            print("immersive: {} frames of {} views\n", s.frames, frame.viewCount);
    }
}

// Starts drawing the space that opened, over its native object.
void spaceOpen(Example& e, void* native) {
    Space& s = e.space;
    s.device = e.device;
    s.library = e.library;
    s.stopping.store(false);
    s.placed = false;
    s.frames = 0;
    PrSurfaceSessionDesc desc{.native = native, .near = 0.1f, .far = 100};
    PrStatus status = prSurfaceSessionCreate(PrSurfaceSessionBackend_Default, &desc, &s.session);
    if (status == PrStatus_Ok) {
        PrSurface surface = prSurfaceSessionSurface(s.session);
        PrGpuSwapChainDesc swapChainDesc{.intent = PrGpuSwapChainIntent_Sdr};
        status = prGpuSwapChainCreate(e.device, &surface, &swapChainDesc, &s.swapChain);
    }
    if (status != PrStatus_Ok) {
        printError("immersive: no session for the space: {}\n", prStatusName(status));
        prSurfaceSessionDestroy(s.session);
        s.session = nullptr;
        prAppImmersiveClose(e.app);
        return;
    }
    s.open = true;
    prThreadStart(&s.thread, runSpace, &s);
}

// Stops drawing the space, once it closed or the application stops.
void spaceClose(Example& e) {
    Space& s = e.space;
    if (!s.open)
        return;
    s.stopping.store(true);
    prThreadJoin(&s.thread);
    pipelinesDestroy(s);
    prGpuSwapChainDestroy(s.swapChain);
    prSurfaceSessionDestroy(s.session);
    s.swapChain = nullptr;
    s.session = nullptr;
    s.open = false;
    print("immersive: the space closed after {} frames\n", s.frames);
}

// Asks for the space, if the platform has one.
void requestSpace(Example& e) {
    PrStatus status = prAppImmersiveOpen(e.app);
    e.supported = status != PrStatus_NotSupported;
    e.opening = status == PrStatus_Ok;
    if (!e.supported)
        print("immersive: this platform has no immersive space\n");
}

bool open(void*, PrAppWindow*, const PrAppFrame*) {
    return true;
}

// Fills the window with a color that says whether the space is open: green while it is, amber while it opens, gray when
// it is closed, and red on a platform without one.
PrAppOutput frame(void* context, PrAppWindow*, const PrAppFrame* f) {
    Example& e = *static_cast<Example*>(context);
    float ratio = f->display.pixelRatio;
    PrUVec2 extent{uint32_t(f->extent.x * ratio + 0.5f), uint32_t(f->extent.y * ratio + 0.5f)};
    PrGpuSwapChainFrame swapFrame{};
    PrGpuCommands* commands = nullptr;
    if (prGpuSwapChainAcquire(e.swapChain, extent, &swapFrame) != PrStatus_Ok ||
            !prGpuCommandsBegin(e.device, nullptr, &commands))
        return PrAppOutput{.wantsFrame = true};
    PrColor color{0.2f, 0.2f, 0.22f, 1};
    if (!e.supported)
        color = PrColor{0.45f, 0.12f, 0.12f, 1};
    else if (e.space.open)
        color = PrColor{0.12f, 0.4f, 0.22f, 1};
    else if (e.opening)
        color = PrColor{0.5f, 0.38f, 0.1f, 1};
    PrGpuSubresources all{};
    prGpuCommandsTextureBarrier(commands, swapFrame.color, all, PrGpuAccessFlag_Discard,
        PrGpuAccessFlag_ColorAttachment);
    PrGpuPassDesc desc = prGpuPassDescColor(swapFrame.color, PrGpuLoad_Clear, color);
    prGpuPassEnd(prGpuPassBegin(commands, &desc));
    prGpuCommandsTextureBarrier(commands, swapFrame.color, all, PrGpuAccessFlag_ColorAttachment,
        PrGpuAccessFlag_Present);
    prGpuSwapChainPresent(e.swapChain, commands);
    prGpuCommandsSubmit(commands);
    return PrAppOutput{};
}

void event(void* context, PrAppWindow*, const PrAppEvent* ev) {
    Example& e = *static_cast<Example*>(context);
    if (ev->kind == PrAppEventKind_CloseRequested) {
        prAppStop(e.app);
    } else if (ev->kind == PrAppEventKind_PointerUp) {
        if (e.space.open)
            prAppImmersiveClose(e.app);
        else if (!e.opening)
            requestSpace(e);
        prAppWindowRequestFrame(e.window);
    }
}

// Opens the window and the device, and asks for the space.
bool start(void* context, PrApp* app) {
    Example& e = *static_cast<Example*>(context);
    e.app = app;
    if (prGpuDeviceCreate(PrGpuBackend_Default, nullptr, &e.device) != PrStatus_Ok ||
            prGpuShaderLibraryCreate(e.device, kImmersiveShaders, sizeof(kImmersiveShaders), &e.library) != PrStatus_Ok) {
        printError("immersive: no GPU device to draw with\n");
        return false;
    }
    PrAppWindowDesc desc{
        .title = "Prax Immersive",
        .extent = {480, 320},
        .handler = {.context = &e, .open = open, .frame = frame, .event = event},
    };
    PrSurface surface{};
    PrGpuSwapChainDesc swapChainDesc{.intent = PrGpuSwapChainIntent_Sdr};
    if (prAppWindowCreate(app, &desc, &e.window) != PrStatus_Ok || prAppWindowSurface(e.window, &surface) != PrStatus_Ok ||
            prGpuSwapChainCreate(e.device, &surface, &swapChainDesc, &e.swapChain) != PrStatus_Ok)
        return false;
    requestSpace(e);
    return true;
}

void appEvent(void* context, PrApp*, const PrAppEvent* ev) {
    Example& e = *static_cast<Example*>(context);
    if (ev->kind == PrAppEventKind_ImmersiveOpened) {
        e.opening = false;
        spaceOpen(e, ev->immersive.native);
    } else if (ev->kind == PrAppEventKind_ImmersiveClosed) {
        e.opening = false;
        spaceClose(e);
    } else {
        return;
    }
    if (e.window != nullptr)
        prAppWindowRequestFrame(e.window);
}

void shutdown(void* context, PrApp*) {
    Example& e = *static_cast<Example*>(context);
    spaceClose(e);
    prGpuSwapChainDestroy(e.swapChain);
    prGpuShaderLibraryDestroy(e.library);
    prGpuDeviceDestroy(e.device);
}

} // namespace

int main() {
    prCrashHandler();
    static Example e;
    PrAppOptions options = prAppOptionsDefault();
    options.windowModel = PrAppWindowModel_Multiple;
    PrApp* app = prAppCreate(PrAppBackend_Default, &options);
    prAppRun(app, {.context = &e, .start = start, .event = appEvent, .shutdown = shutdown});
    prAppDestroy(app);
    return 0;
}
