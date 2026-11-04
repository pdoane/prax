// The draw example: animated scenes drawn with lib/draw alone, in a window.
//
// main() is the composition root. Each frame records the scene into a draw list at the window's size and renders the
// list with lib/draw's fragment renderer into the window's swap chain, which is made for HDR. Where the swap chain
// holds more than 8-bit sRGB, as it does for a display that shows more, the list renders into a texture of half floats,
// which the display shader then maps to the display (pr/draw_display.h); elsewhere into the swap chain itself. Left and
// Right turn through the scenes, and Space pauses the clock, as the caption under each scene says. The pointer and the
// primary button reach the scenes, which follow the pointer and answer a held button.

#include "pr/app.h"
#include "pr/assert.h"
#include "pr/draw_display.h"
#include "pr/draw_list.h"
#include "pr/draw_renderer.h"
#include "pr/float.h"
#include "pr/gpu.h"
#include "pr/print.h"
#include "pr/text.h"

#include "examples/draw/scenes.h"

namespace {

using namespace pr;
using namespace scenes;

// The scenes in a window, and the device that they render on.
struct Window {
    const Scene* scenes = nullptr;
    uint32_t sceneCount = 0;
    uint32_t scene = 0;
    double clock = 0;           // the scene's time, in seconds, which stands while paused
    PrVec2 pointer{};           // where the pointer last moved, in logical pixels
    bool pointed = false;       // the pointer has moved over the window
    bool held = false;          // the primary button is held
    float hold = 0;             // eases toward 1 while the button is held, and toward 0 after
    bool paused = false;
    PrDrawList* list = nullptr;

    PrApp* app = nullptr;
    PrGpuDevice* device = nullptr;
    PrGpuSwapChain* swapChain = nullptr;
    PrDrawRenderer* renderer = nullptr;
    PrDrawFrame* frame = nullptr;

    // Where the swap chain holds more than 8-bit sRGB: the texture of half floats that the list renders into, and the
    // shader that maps it to the display.
    PrGpuTexture rendered{};
    PrDrawDisplay* display = nullptr;
};

void announce(const Window& w) {
    print("{}: {}\n", w.scenes[w.scene].name, w.scenes[w.scene].description);
}

// Frees what the window made on its device, and the device.
void closeDevice(Window& w) {
    if (w.device != nullptr) {
        prGpuDeviceWaitIdle(w.device);
        prGpuTextureDestroy(w.device, w.rendered);
    }
    prDrawDisplayDestroy(w.display);
    prDrawFrameDestroy(w.frame);
    prDrawRendererDestroy(w.renderer);
    prGpuSwapChainDestroy(w.swapChain);
    prGpuDeviceDestroy(w.device);
    w.rendered = PrGpuTexture{};
    w.display = nullptr;
    w.frame = nullptr;
    w.renderer = nullptr;
    w.swapChain = nullptr;
    w.device = nullptr;
}

// Makes the platform's device, a swap chain over the window for HDR, and the renderer. Returns false if the window has
// no device to render with.
bool openDevice(Window& w) {
    PrAppSurface surface{};
    PrGpuSwapChainDesc desc{.intent = PrGpuSwapChainIntent_Hdr};
    if (prGpuDeviceCreate(PrGpuBackend_Default, nullptr, &w.device) != PrStatus_Ok ||
            prAppSurface(w.app, &surface) != PrStatus_Ok ||
            prGpuSwapChainCreate(w.device, &surface, &desc, &w.swapChain) != PrStatus_Ok ||
            prDrawRendererCreate(PrDrawRendererBackend_Default, w.device, nullptr, &w.renderer) != PrStatus_Ok ||
            prDrawDisplayCreate(w.device, &w.display) != PrStatus_Ok) {
        closeDevice(w);
        return false;
    }
    w.frame = prDrawFrameCreate(w.renderer);
    return true;
}

// Returns the texture of half floats that a frame of `extent` renders into, made again when the window grows past it.
PrGpuTexture renderedFor(Window& w, PrUVec2 extent) {
    PrUVec2 held = w.rendered.id != 0 ? prGpuTextureExtent(w.device, w.rendered) : PrUVec2{};
    if (w.rendered.id == 0 || extent.x > held.x || extent.y > held.y) {
        prGpuTextureDestroy(w.device, w.rendered);
        PrGpuTextureDesc desc = prGpuTextureDesc2d(PrGpuFormat_Rgba16F, extent,
            PrGpuTextureUsageFlag_Sampled | PrGpuTextureUsageFlag_RenderTarget);
        w.rendered = PrGpuTexture{};
        prGpuTextureCreate(w.device, &desc, &w.rendered);
    }
    return w.rendered;
}

// Renders the list into the swap chain's texture at `extent` target pixels and presents it.
void render(Window& w, PrUVec2 extent, const PrAppDisplay& display) {
    PrGpuTexture drawable{};
    PrGpuCommands* commands = nullptr;
    if (prGpuSwapChainAcquire(w.swapChain, extent, &drawable) != PrStatus_Ok ||
            !prGpuCommandsBegin(w.device, nullptr, &commands))
        return;
    PrGpuSwapChainInfo info = prGpuSwapChainInfo(w.swapChain);
    bool mapped = info.format != PrGpuFormat_Bgra8 || info.space != PrColorSpace_Srgb;
    PrGpuTexture target = mapped ? renderedFor(w, extent) : drawable;
    PrDrawTarget into{
        .extent = extent,
        .format = PrPixelFormat(mapped ? PrPixelFormat_RgbaF16 : PrPixelFormat_Bgra8),
        .commands = commands,
    };
    prDrawFramePrepare(w.frame, w.list, &into);
    // The half floats rest in the accesses of shader reads between frames, and the drawable holds nothing yet.
    PrGpuSubresources all{};
    prGpuCommandsTextureBarrier(commands, target, all, mapped ? PrGpuAccessFlag_ShaderRead : PrGpuAccessFlag_Discard,
        PrGpuAccessFlag_ColorAttachment);
    PrGpuPassDesc pass = prGpuPassDescColor(target, PrGpuLoad_Clear, PrColor{0, 0, 0, 1});
    PrGpuPass* encoder = prGpuPassBegin(commands, &pass);
    prDrawFrameRender(w.frame, encoder, PrIVec2{0, 0});
    prGpuPassEnd(encoder);
    if (mapped) {
        prGpuCommandsTextureBarrier(commands, target, all, PrGpuAccessFlag_ColorAttachment, PrGpuAccessFlag_ShaderRead);
        prGpuCommandsTextureBarrier(commands, drawable, all, PrGpuAccessFlag_Discard, PrGpuAccessFlag_ColorAttachment);
        PrGpuPassDesc shown = prGpuPassDescColor(drawable, PrGpuLoad_Discard, PrColor{});
        encoder = prGpuPassBegin(commands, &shown);
        PrDrawDisplayDesc mapping{.target = info.space, .headroom = display.headroom, .gamut = display.gamut};
        prDrawDisplayDraw(w.display, encoder, target, &mapping);
        prGpuPassEnd(encoder);
    }
    prGpuCommandsTextureBarrier(commands, drawable, all, PrGpuAccessFlag_ColorAttachment, PrGpuAccessFlag_Present);
    prGpuSwapChainPresent(w.swapChain, commands);
    prGpuCommandsSubmit(commands);
}

void start(void* context, PrApp* app, const PrAppFrame*) {
    Window& w = *static_cast<Window*>(context);
    w.app = app;
    if (!openDevice(w)) {
        printError("draw: no GPU device to render with\n");
        prAppStop(app);
        return;
    }
    announce(w);
    prAppRequestFrame(app);
}

PrAppOutput frame(void* context, PrApp*, const PrAppFrame* f) {
    Window& w = *static_cast<Window*>(context);
    // A lost device is made again, with everything on it.
    if (w.device != nullptr && prGpuDeviceIsLost(w.device))
        closeDevice(w);
    if (w.device == nullptr && !openDevice(w))
        return {};
    prGpuSwapChainWait(w.swapChain);
    if (!w.paused)
        w.clock += f->delta;
    // The hold eases in a tenth of a second or so.
    float ease = 1 - prF32Exp(-float(f->delta) * 18);
    w.hold += ((w.held ? 1.0f : 0.0f) - w.hold) * ease;

    // The scene draws in logical pixels, and the list's transform takes them to the display's target pixels.
    float ratio = f->display.pixelRatio;
    prDrawListClear(w.list);
    prDrawListPushTransformAffine2(w.list, prAffine2Scale(PrVec2{ratio, ratio}));
    bool over =
        w.pointed && w.pointer.x >= 0 && w.pointer.y >= 0 && w.pointer.x < f->extent.x && w.pointer.y < f->extent.y;
    SceneInput input{.pointer = w.pointer, .over = over, .hold = w.hold};
    w.scenes[w.scene].draw(w.list, f->extent.x, f->extent.y, w.clock, input);
    drawCaption(w.list, f->extent.x, f->extent.y, w.scene, w.paused);
    prDrawListPopTransform(w.list);

    PrUVec2 extent{uint32_t(f->extent.x * ratio + 0.5f), uint32_t(f->extent.y * ratio + 0.5f)};
    if (extent.x != 0 && extent.y != 0)
        render(w, extent, f->display);
    return PrAppOutput{.wantsFrame = true};
}

void event(void* context, PrApp* app, const PrAppEvent* e) {
    Window& w = *static_cast<Window*>(context);
    if (e->kind == PrAppEventKind_PointerMove || e->kind == PrAppEventKind_PointerDown ||
            e->kind == PrAppEventKind_PointerUp) {
        w.pointer = e->position;
        w.pointed = true;
        if (e->button == 0 && e->kind != PrAppEventKind_PointerMove)
            w.held = e->kind == PrAppEventKind_PointerDown;
        return;
    }
    if (e->kind != PrAppEventKind_KeyDown)
        return;
    if (e->key == PrAppKey_Right || e->key == PrAppKey_Left) {
        w.scene = (w.scene + (e->key == PrAppKey_Right ? 1 : w.sceneCount - 1)) % w.sceneCount;
        announce(w);
    } else if (e->key == ' ') {
        w.paused = !w.paused;
    } else if (e->key == PrAppKey_Escape) {
        prAppStop(app);
    }
}

void shutdown(void* context, PrApp*) {
    closeDevice(*static_cast<Window*>(context));
}

} // namespace

int main() {
    prCrashHandler();
    static Window w;
    PrTextShaper* shaper = prTextShaperCreate(PrTextShaperBackend_Default);
    scenesOpen(shaper, PR_FONT_BUNDLE_DIR);
    w.scenes = allScenes(&w.sceneCount);
    w.list = prDrawListCreate();

    PrAppDesc desc{.title = "Prax Draw", .extent = {960, 720}};
    PrApp* app = prAppCreate(PrAppBackend_Default, &desc);
    PrAppHandler handler{&w, start, frame, event, shutdown};
    prAppRun(app, &handler);
    prAppDestroy(app);

    prDrawListRelease(w.list);
    scenesClose();
    prTextShaperDestroy(shaper);
    return 0;
}
