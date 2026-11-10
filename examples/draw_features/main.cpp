// draw_features: the drawings of lib/draw's features and workloads, in a window.
//
// main() is the composition root. Each frame records the drawing into a draw list at the window's size and renders the
// list with lib/draw's fragment renderer into the window's swap chain, which is made for HDR. Where the swap chain
// holds more than 8-bit sRGB, as it does for a display that shows more, the list renders into a texture of half floats,
// which the display shader then maps to the display (pr/draw_display.h); elsewhere into the swap chain itself. Left and
// Right turn through the drawings, Space pauses the clock, and a caption under the drawing names it.
//
// `draw_features [word]` opens on the first drawing whose name contains the word.

#include "pr/app.h"
#include "pr/assert.h"
#include "pr/draw_display.h"
#include "pr/draw_list.h"
#include "pr/draw_renderer.h"
#include "pr/gpu.h"
#include "pr/print.h"
#include "pr/string.h"
#include "pr/string_ops.h"
#include "pr/text.h"

#include "examples/draw_features/drawing.h"
#include "examples/draw_features/features.h"
#include "examples/draw_features/workloads.h"

namespace {

using namespace pr;
using namespace drawings;

// The height of the band under the drawing that holds its caption, in logical pixels.
constexpr float kCaptionHeight = 28;

// The drawings in a window, and the device that they render on.
struct Window {
    Array<Drawing> drawings;
    uint32_t drawing = 0;
    double clock = 0;           // the drawing's time, in seconds, which stands while paused
    bool paused = false;
    PrDrawList* list = nullptr;
    PrTextShaper* shaper = nullptr;

    PrApp* app = nullptr;
    PrAppWindow* window = nullptr;
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
    print("{}: {}\n", w.drawings[w.drawing].name, w.drawings[w.drawing].description);
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
            prAppWindowSurface(w.window, &surface) != PrStatus_Ok ||
            prGpuSwapChainCreate(w.device, &surface, &desc, &w.swapChain) != PrStatus_Ok ||
            prDrawRendererCreate(PrDrawRendererBackend_Fragment, w.device, nullptr, &w.renderer) != PrStatus_Ok ||
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

// Renders the list over the drawing's canvas into the swap chain's texture at `extent` target pixels and presents it.
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
    PrGpuPassDesc pass = prGpuPassDescColor(target, PrGpuLoad_Clear, drawingCanvas(w.drawings[w.drawing]));
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

bool open(void*, PrAppWindow*, const PrAppFrame*) {
    return true;
}

PrAppOutput frame(void* context, PrAppWindow*, const PrAppFrame* f) {
    Window& w = *static_cast<Window*>(context);
    // A lost device is made again, with everything on it.
    if (w.device != nullptr && prGpuDeviceIsLost(w.device))
        closeDevice(w);
    if (w.device == nullptr && !openDevice(w))
        return {};
    prGpuSwapChainWait(w.swapChain);
    if (!w.paused)
        w.clock += f->delta;

    // The drawing draws in logical pixels above the caption, and the list's transform takes them to the display's
    // target pixels. The caption is a label, which the drawing draws over what it drew.
    float ratio = f->display.pixelRatio;
    prDrawListClear(w.list);
    prDrawListPushTransformAffine2(w.list, prAffine2Scale(PrVec2{ratio, ratio}));
    const Drawing& drawing = w.drawings[w.drawing];
    String caption = format("{} of {}: {}{}. Left and Right turn, Space pauses.", w.drawing + 1, w.drawings.count(),
        drawing.name, w.paused ? ", paused" : "");
    label(caption, PrVec2{12, f->extent.y - 10}, 13, kInk);
    drawDrawing(drawing, w.list, w.shaper, nullptr, f->display, f->extent.x, f->extent.y - kCaptionHeight, w.clock);
    prDrawListPopTransform(w.list);

    PrUVec2 extent{uint32_t(f->extent.x * ratio + 0.5f), uint32_t(f->extent.y * ratio + 0.5f)};
    if (extent.x != 0 && extent.y != 0)
        render(w, extent, f->display);
    return PrAppOutput{.wantsFrame = true};
}

void event(void* context, PrAppWindow*, const PrAppEvent* e) {
    Window& w = *static_cast<Window*>(context);
    if (e->kind == PrAppEventKind_CloseRequested) {
        prAppStop(w.app);
        return;
    }
    if (e->kind != PrAppEventKind_KeyDown)
        return;
    uint32_t count = w.drawings.count();
    if (e->keyboard.key == PrAppKey_Right || e->keyboard.key == PrAppKey_Left) {
        w.drawing = (w.drawing + (e->keyboard.key == PrAppKey_Right ? 1 : count - 1)) % count;
        announce(w);
    } else if (e->keyboard.key == ' ') {
        w.paused = !w.paused;
    } else if (e->keyboard.key == PrAppKey_Escape) {
        prAppStop(w.app);
    }
}

// Opens the window and the device that renders to it. The window's close stops the application.
bool start(void* context, PrApp* app) {
    Window& w = *static_cast<Window*>(context);
    w.app = app;
    PrAppWindowDesc desc{
        .title = "Prax Draw Features",
        .extent = {1100, 828},
        .handler = {.context = &w, .open = open, .frame = frame, .event = event},
    };
    if (prAppWindowCreate(app, &desc, &w.window) != PrStatus_Ok)
        return false;
    if (!openDevice(w)) {
        printError("draw_features: no GPU device to render with\n");
        return false;
    }
    announce(w);
    return true;
}

void appEvent(void*, PrApp*, const PrAppEvent*) {}

void shutdown(void* context, PrApp*) {
    closeDevice(*static_cast<Window*>(context));
}

} // namespace

int main(int argc, char** argv) {
    prCrashHandler();
    static Window w;
    w.drawings.append(allFeatures().span());
    w.drawings.append(workloadDrawings().span());
    for (uint32_t i = 0; argc > 1 && i < w.drawings.count(); ++i) {
        if (prStringContains(w.drawings[i].name, argv[1])) {
            w.drawing = i;
            break;
        }
    }
    w.shaper = prTextShaperCreate(PrTextShaperBackend_Default);
    w.list = prDrawListCreate();

    PrApp* app = prAppCreate(PrAppBackend_Default, nullptr);
    prAppRun(app, {.context = &w, .start = start, .event = appEvent, .shutdown = shutdown});
    prAppDestroy(app);

    prDrawListRelease(w.list);
    releaseDrawings();
    prTextShaperDestroy(w.shaper);
    return 0;
}
