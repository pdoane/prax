// The draw gallery: animated scenes drawn with lib/draw alone, in a window.
//
// main() is the composition root. Each frame records the scene into a draw list at the window's size, and renders the
// list into the window's swap chain, which is made for HDR, with lib/draw's default renderer for the device. If the
// swap chain holds more than 8-bit sRGB, as it does for a display that shows more, the list renders into a texture of
// half floats, and the display shader then maps that texture to the display (pr/draw_display.h). Otherwise the list
// renders into the swap chain itself. Left and Right step through the scenes, and Space pauses the clock, as the
// caption under each scene says. The scenes follow the pointer and respond to a held primary button.
//
// Every few seconds, the gallery prints the device and the renderer that it draws with, and the time that a frame takes
// on the CPU, from recording the list to submitting it, and on the GPU where the device measures it.
//
// Usage: draw [--gpu NAME] [--renderer NAME] [--scene NUMBER]. The NAMEs are those of one of lib/gpu's backends and one
// of lib/draw's, and Default if they are not given, which the environment variables PR_GPU_BACKEND and PR_DRAW_RENDERER
// also choose. NUMBER is the scene shown first, counted from 1. On the web, the page's query passes the arguments.

#include "pr/app.h"
#include "pr/assert.h"
#include "pr/atomic.h"
#include "pr/draw_display.h"
#include "pr/draw_list.h"
#include "pr/draw_renderer.h"
#include "pr/float.h"
#include "pr/gpu.h"
#include "pr/integer.h"
#include "pr/print.h"
#include "pr/status.h"
#include "pr/task.h"
#include "pr/text.h"
#include "pr/time.h"

#include "examples/draw_gallery/scenes.h"

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
    PrGpuBackend gpu = PrGpuBackend_Default;
    PrDrawRendererBackend backend = PrDrawRendererBackend_Default;

    PrApp* app = nullptr;
    PrAppWindow* window = nullptr;
    PrGpuDevice* device = nullptr;
    PrGpuSwapChain* swapChain = nullptr;
    PrDrawRenderer* renderer = nullptr;
    PrDrawFrame* frame = nullptr;

    // If the swap chain holds more than 8-bit sRGB, these are the texture of half floats that the list renders into,
    // and the shader that maps it to the display, which is made for the first frame that needs it.
    PrGpuTexture rendered{};
    PrDrawDisplay* display = nullptr;

    PrTaskPool* pool = nullptr;

    // The frames since the last report, and their time on the CPU. The device's thread adds each frame's time on the
    // GPU once its work is done.
    double reported = 0;        // the frame time of the last report, in seconds
    uint32_t frames = 0;
    double cpuMilliseconds = 0;
    Atomic<uint64_t> gpuNanoseconds;    // starts zero, as the window is static
    Atomic<uint32_t> gpuFrames;
};

// The seconds between reports of the frames' times.
constexpr double kReportSeconds = 2;

void announce(const Window& w) {
    print("{}: {}\n", w.scenes[w.scene].name, w.scenes[w.scene].description);
}

// Frees what the window made on its device, and the device.
void closeDevice(Window& w) {
    if (w.device != nullptr)
        prGpuTextureDestroy(w.device, w.rendered);
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

// Creates the platform's device on the pool, if the window has none.
void requestDevice(Window& w) {
    if (w.device != nullptr)
        return;
    PrGpuDeviceOptions options = prGpuDeviceOptionsDefault();
    options.pool = w.pool;
    w.device = prGpuDeviceCreate(w.gpu, &options);
}

// Returns true while the device is being created.
bool creatingDevice(const Window& w) {
    return w.device != nullptr && !prGpuDeviceIsReady(w.device);
}

// Returns true if the window can render. Frees a lost device, which includes one whose creation failed, with everything
// made on it, and creates another. Makes the swap chain for HDR and the renderer once the device is ready.
bool openDevice(Window& w) {
    if (w.device != nullptr && prGpuDeviceIsLost(w.device)) {
        if (w.swapChain == nullptr)
            printError("draw: no GPU device to render with ({})\n", prStatusName(prGpuDeviceLostStatus(w.device)));
        closeDevice(w);
    }
    requestDevice(w);
    if (w.device == nullptr || !prGpuDeviceIsReady(w.device) || prGpuDeviceIsLost(w.device))
        return false;
    if (w.swapChain != nullptr)
        return true;
    PrAppSurface surface{};
    PrGpuSwapChainDesc desc{.intent = PrGpuSwapChainIntent_Hdr};
    if (prAppWindowSurface(w.window, &surface) != PrStatus_Ok ||
            prGpuSwapChainCreate(w.device, &surface, &desc, &w.swapChain) != PrStatus_Ok ||
            prDrawRendererCreate(w.backend, w.device, nullptr, &w.renderer) != PrStatus_Ok) {
        printError("draw: the device cannot render to the window\n");
        closeDevice(w);
        return false;
    }
    w.frame = prDrawFrameCreate(w.renderer);
    PrGpuDeviceInfo info = prGpuDeviceInfo(w.device);
    print("draw: rendering with {} on {} ({})\n", prDrawRendererBackendName(w.backend), info.backend, info.adapter);
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

// Adds a frame's time on the GPU to the window's.
void frameDone(void* PR_NULLABLE context, double seconds) {
    Window& w = *static_cast<Window*>(context);
    w.gpuNanoseconds.fetchAdd(uint64_t(seconds * 1e9));
    w.gpuFrames.fetchAdd(1);
}

// Prints the frames' average times since the last report, every kReportSeconds. A device without timestamps reports no
// time on the GPU.
void report(Window& w, double time) {
    if (time - w.reported < kReportSeconds || w.frames == 0)
        return;
    uint64_t gpuNanoseconds = w.gpuNanoseconds.exchange(0);
    uint32_t gpuFrames = w.gpuFrames.exchange(0);
    PrGpuDeviceInfo info = prGpuDeviceInfo(w.device);
    PrStringView renderer = prDrawRendererBackendName(w.backend);
    double cpu = w.cpuMilliseconds / w.frames;
    if ((info.features & PrGpuFeatureFlag_Timestamps) != 0 && gpuFrames != 0) {
        double gpu = double(gpuNanoseconds) / 1e6 / gpuFrames;
        print("draw: {} on {}: cpu {:.2f} ms, gpu {:.2f} ms a frame\n", renderer, info.backend, cpu, gpu);
    } else {
        print("draw: {} on {}: cpu {:.2f} ms a frame\n", renderer, info.backend, cpu);
    }
    w.reported = time;
    w.frames = 0;
    w.cpuMilliseconds = 0;
}

// Renders the list into the swap chain's texture at `extent` target pixels and presents it.
void render(Window& w, PrUVec2 extent, const PrAppDisplay& display) {
    PrGpuTexture drawable{};
    PrGpuCommands* commands = nullptr;
    if (prGpuSwapChainAcquire(w.swapChain, extent, &drawable) != PrStatus_Ok ||
            !prGpuCommandsBegin(w.device, nullptr, &commands))
        return;
    PrGpuSwapChainInfo info = prGpuSwapChainInfo(w.swapChain);
    // The list renders straight into an 8-bit sRGB drawable, and into any other drawable through the display shader.
    PrPixelFormat direct{};
    bool mapped = info.space != PrColorSpace_Srgb || !prGpuFormatToPixelFormat(info.format, &direct) ||
        direct == PrPixelFormat_RgbaF16;
    if (mapped && w.display == nullptr && prDrawDisplayCreate(w.device, &w.display) != PrStatus_Ok) {
        prGpuCommandsSubmit(commands);
        return;
    }
    PrGpuTexture target = mapped ? renderedFor(w, extent) : drawable;
    PrDrawTarget into{
        .extent = extent,
        .format = mapped ? PrPixelFormat(PrPixelFormat_RgbaF16) : direct,
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
    prGpuCommandsOnDone(commands, frameDone, &w);
    prGpuCommandsSubmit(commands);
}

bool open(void*, PrAppWindow*, const PrAppFrame*) {
    return true;
}

PrAppOutput frame(void* context, PrAppWindow*, const PrAppFrame* f) {
    Window& w = *static_cast<Window*>(context);
    // Frames continue while the device is being created.
    if (!openDevice(w))
        return PrAppOutput{.wantsFrame = creatingDevice(w)};
    prGpuSwapChainWait(w.swapChain);
    if (!w.paused)
        w.clock += f->delta;
    // The hold eases in a tenth of a second or so.
    float ease = 1 - prF32Exp(-float(f->delta) * 18);
    w.hold += ((w.held ? 1.0f : 0.0f) - w.hold) * ease;

    // The scene draws in logical pixels, and the list's transform takes them to the display's target pixels.
    PrTimePerf began = prTimePerfNow();
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
    w.cpuMilliseconds += prTimePerfMilliseconds(began, prTimePerfNow());
    ++w.frames;
    report(w, f->time);
    return PrAppOutput{.wantsFrame = true};
}

void event(void* context, PrAppWindow*, const PrAppEvent* e) {
    Window& w = *static_cast<Window*>(context);
    if (e->kind == PrAppEventKind_CloseRequested) {
        prAppStop(w.app);
        return;
    }
    if (e->kind == PrAppEventKind_PointerMove || e->kind == PrAppEventKind_PointerDown ||
            e->kind == PrAppEventKind_PointerUp) {
        w.pointer = e->pointer.position;
        w.pointed = true;
        if (e->pointer.button == 0 && e->kind != PrAppEventKind_PointerMove)
            w.held = e->kind == PrAppEventKind_PointerDown;
        return;
    }
    if (e->kind != PrAppEventKind_KeyDown)
        return;
    if (e->keyboard.key == PrAppKey_Right || e->keyboard.key == PrAppKey_Left) {
        w.scene = (w.scene + (e->keyboard.key == PrAppKey_Right ? 1 : w.sceneCount - 1)) % w.sceneCount;
        announce(w);
    } else if (e->keyboard.key == ' ') {
        w.paused = !w.paused;
    } else if (e->keyboard.key == PrAppKey_Escape) {
        prAppStop(w.app);
    }
}

// Opens the window. The window renders once the device is ready, and its close stops the application.
bool start(void* context, PrApp* app) {
    Window& w = *static_cast<Window*>(context);
    w.app = app;
    PrAppWindowDesc desc{
        .title = "Prax Draw",
        .extent = {960, 720},
        .handler = {.context = &w, .open = open, .frame = frame, .event = event},
    };
    if (prAppWindowCreate(app, &desc, &w.window) != PrStatus_Ok)
        return false;
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
    uint32_t first = 1;
    for (int i = 1; i < argc; ++i) {
        PrStringView argument = argv[i];
        if (argument == "--gpu" && i + 1 < argc) {
            w.gpu = prGpuBackendFromName(argv[++i]);
            if (!prGpuBackendIsBuilt(w.gpu)) {
                printError("draw: no GPU backend named {} in this build\n", PrStringView{argv[i]});
                return 1;
            }
        } else if (argument == "--renderer" && i + 1 < argc) {
            w.backend = prDrawRendererBackendFromName(argv[++i]);
            if (!prDrawRendererBackendIsBuilt(w.backend)) {
                printError("draw: no renderer named {} in this build\n", PrStringView{argv[i]});
                return 1;
            }
        } else if (argument == "--scene" && i + 1 < argc) {
            PrStringView number = argv[++i];
            uint64_t parsed = 0;
            first = prU64Parse(number.data, number.size, &parsed, 10) && parsed <= UINT32_MAX ? uint32_t(parsed) : 0;
        } else {
            printError("usage: draw [--gpu NAME] [--renderer NAME] [--scene NUMBER]\n");
            return 1;
        }
    }
    // The device is created on the pool while the fonts load and the window opens.
    w.pool = prTaskPoolCreate(PrTaskPoolBackend_Default, 1);
    requestDevice(w);
    PrTextShaper* shaper = prTextShaperCreate(PrTextShaperBackend_Default);
    scenesOpen(shaper);
    w.scenes = allScenes(&w.sceneCount);
    w.scene = first >= 1 && first <= w.sceneCount ? first - 1 : 0;
    w.list = prDrawListCreate();

    PrApp* app = prAppCreate(PrAppBackend_Default, nullptr);
    prAppRun(app, {.context = &w, .start = start, .event = appEvent, .shutdown = shutdown});
    prAppDestroy(app);

    prTaskPoolDestroy(w.pool);
    prDrawListRelease(w.list);
    scenesClose();
    prTextShaperDestroy(shaper);
    return 0;
}
