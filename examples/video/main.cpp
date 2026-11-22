// The video example: a video played with lib/media and drawn with lib/draw, in a window.
//
// main() is the composition root. Each frame asks the decoder for the video's frame at the clock's time, and makes a
// texture of the frame's image (prGpuTextureImport). Metal imports a CVPixelBuffer without a copy, and a web page's
// Canvas 2D device imports a VideoFrame without a copy. The list draws the texture as a source
// (PrDrawSourceKind_Texture), fitted to the window, under rounded corners and over a shadow, with the video's progress
// beneath it. Space pauses the clock, and Escape quits.
//
// Usage: video [FILE]. FILE is the video to play. Without it, the example plays its own video, the resource cat.mp4
// (./dev.sh fetch videos). On the web, the page's query passes the arguments. PR_BACKEND_RENDERER chooses the renderer.

#include "pr/app.h"
#include "pr/app_select.h"
#include "pr/draw_list.h"
#include "pr/draw_renderer.h"
#include "pr/draw_select.h"
#include "pr/file.h"
#include "pr/float.h"
#include "pr/gpu.h"
#include "pr/gpu_select.h"
#include "pr/media.h"
#include "pr/print.h"
#include "pr/resource.h"
#include "pr/status.h"
#include "pr/task.h"
#include "pr/texture.h"

namespace {

using namespace pr;

// The window, the device that it renders on, and the video that it plays.
struct Window {
    PrApp* app = nullptr;
    PrAppWindow* window = nullptr;
    PrTaskPool* pool = nullptr;
    PrGpuDevice* device = nullptr;
    PrGpuSwapChain* swapChain = nullptr;
    PrDrawRenderer* renderer = nullptr;
    PrDrawFrame* frame = nullptr;
    PrDrawList* list = nullptr;

    PrMediaCodec* codec = nullptr;
    PrMediaDecoder* decoder = nullptr;
    PrMediaInfo info{};
    double clock = 0;
    bool paused = false;

    // The texture of the frame that is shown, and that frame's serial.
    PrGpuTexture texture{};
    uint64_t frameSerial = 0;
    bool reported = false;          // true once the example has printed whether frames are imported or copied
};

// Frees what the window made on its device, and the device.
void closeDevice(Window& w) {
    if (w.device != nullptr)
        prGpuTextureRelease(w.device, w.texture);
    w.texture = PrGpuTexture{};
    w.frameSerial = 0;
    prDrawFrameDestroy(w.frame);
    prDrawRendererDestroy(w.renderer);
    prGpuSwapChainDestroy(w.swapChain);
    prGpuDeviceDestroy(w.device);
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
    w.device = prGpuDeviceCreate(PrGpuBackend_Default, &options);
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
            printError("video: no GPU device to render with ({})\n", prStatusName(prGpuDeviceLostStatus(w.device)));
        closeDevice(w);
    }
    requestDevice(w);
    if (w.device == nullptr || !prGpuDeviceIsReady(w.device) || prGpuDeviceIsLost(w.device))
        return false;
    if (w.swapChain != nullptr)
        return true;
    PrSurface surface{};
    PrGpuSwapChainDesc desc{.intent = PrGpuSwapChainIntent_Hdr};
    if (prAppWindowSurface(w.window, &surface) != PrStatus_Ok ||
            prGpuSwapChainCreate(w.device, &surface, &desc, &w.swapChain) != PrStatus_Ok ||
            prDrawRendererCreate(PrDrawRendererBackend_Default, w.device, nullptr, &w.renderer) != PrStatus_Ok) {
        printError("video: the device cannot render to the window\n");
        closeDevice(w);
        return false;
    }
    w.frame = prDrawFrameCreate(w.renderer);
    print("video: rendering with {} on {}\n", prDrawRendererName(w.renderer), prGpuDeviceInfo(w.device).backend);
    return true;
}

// Makes the window's texture show a decoded frame that has not been shown yet. The device imports the frame's image in
// place if it can, and otherwise copies its texels into the same texture.
void showFrame(Window& w, const PrMediaFrame& f) {
    if (f.serial == w.frameSerial && w.texture.id != 0)
        return;
    if (prGpuTextureImport(w.device, &f.image, nullptr, nullptr, &w.texture) != PrStatus_Ok)
        return;
    w.frameSerial = f.serial;
    if (!w.reported) {
        bool inPlace = prGpuDeviceImportsInPlace(w.device, f.image.kind);
        print("video: {} {} each frame\n", prGpuDeviceInfo(w.device).backend, inPlace ? "imports" : "copies");
        w.reported = true;
    }
}

// Records the frame: the video fitted to the window over its shadow, under rounded corners, and its progress beneath.
void record(Window& w, float width, float height, double seconds) {
    PrDrawList* list = w.list;
    prDrawListRect(list, PrRect{0, 0, width, height}, {.color = PrColor{0.09f, 0.08f, 0.1f, 1}});
    if (w.texture.id == 0)
        return;
    PrUVec2 extent = prGpuTextureExtent(w.device, w.texture);
    float margin = 32;
    float bar = 24;
    float scale = prF32Min((width - 2 * margin) / float(extent.x), (height - 2 * margin - bar) / float(extent.y));
    if (!(scale > 0))
        return;
    float shownWidth = float(extent.x) * scale;
    float shownHeight = float(extent.y) * scale;
    float left = (width - shownWidth) / 2;
    float top = (height - bar - shownHeight) / 2;
    PrRect area{left, top, left + shownWidth, top + shownHeight};
    PrCornerRect shape = prCornerRectRound(area, 18);

    PrDrawShadow shadow{.shape = shape, .offset = PrVec2{0, 10}, .sigma = 16};
    prDrawListDropShadow(list, &shadow, {.color = PrColor{0, 0, 0, 0.6f}});
    PrDrawSource source = prDrawSourceTexture(PrTexture{.texture = w.texture, .alpha = PrTextureAlpha_None}, extent,
        PrRect{0, 0, float(extent.x), float(extent.y)});
    PrDrawBrush brush = prDrawBrushImage(source, area, PrDrawSampling_Linear);
    prDrawListCornerRect(list, shape, {.brush = &brush});

    // The progress through the video, beneath it.
    float trackTop = area.bottom + bar - 8;
    PrRect track{left, trackTop, area.right, trackTop + 4};
    prDrawListCornerRect(list, prCornerRectRound(track, 2), {.color = PrColor{1, 1, 1, 0.18f}});
    double duration = w.info.duration;
    if (prF64IsFinite(duration) && duration > 0) {
        float done = float(prF64Min(prF64Max(seconds / duration, 0), 1));
        PrRect fill{left, trackTop, left + shownWidth * done, trackTop + 4};
        prDrawListCornerRect(list, prCornerRectRound(fill, 2), {.color = PrColor{0.98f, 0.62f, 0.27f, 1}});
    }
}

// Renders the list into the swap chain's texture at `extent` target pixels and presents it.
void render(Window& w, PrUVec2 extent) {
    PrGpuSwapChainFrame frame{};
    PrGpuCommands* commands = nullptr;
    if (prGpuSwapChainAcquire(w.swapChain, extent, &frame) != PrStatus_Ok ||
            !prGpuCommandsBegin(w.device, nullptr, &commands))
        return;
    PrGpuSwapChainInfo info = prGpuSwapChainInfo(w.swapChain);
    PrPixelFormat format{};
    if (!prGpuFormatToPixelFormat(info.format, &format)) {
        prGpuCommandsSubmit(commands);
        return;
    }
    PrDrawTarget target{.extent = extent, .format = format, .commands = commands};
    prDrawFramePrepare(w.frame, w.list, &target);
    PrGpuSubresources all{};
    prGpuCommandsTextureBarrier(commands, frame.color, all, PrGpuAccessFlag_Discard, PrGpuAccessFlag_ColorAttachment);
    PrGpuPassDesc pass = prGpuPassDescColor(frame.color, PrGpuLoad_Clear, PrColor{0, 0, 0, 1});
    PrGpuPass* encoder = prGpuPassBegin(commands, &pass);
    prDrawFrameRender(w.frame, encoder, PrIVec2{0, 0});
    prGpuPassEnd(encoder);
    prGpuCommandsTextureBarrier(commands, frame.color, all, PrGpuAccessFlag_ColorAttachment, PrGpuAccessFlag_Present);
    prGpuSwapChainPresent(w.swapChain, commands);
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
    // The video loops.
    double seconds = w.clock;
    if (prF64IsFinite(w.info.duration) && w.info.duration > 0)
        seconds = prF64FloorMod(w.clock, w.info.duration);
    PrMediaFrame decoded{};
    if (prMediaDecoderFrame(w.decoder, seconds, &decoded))
        showFrame(w, decoded);

    // The list is in logical pixels, and its transform takes them to the display's target pixels.
    float ratio = f->display.pixelRatio;
    prDrawListClear(w.list);
    prDrawListPushTransformAffine2(w.list, prAffine2Scale(PrVec2{ratio, ratio}));
    record(w, f->extent.x, f->extent.y, seconds);
    prDrawListPopTransform(w.list);
    PrUVec2 extent{uint32_t(f->extent.x * ratio + 0.5f), uint32_t(f->extent.y * ratio + 0.5f)};
    if (extent.x != 0 && extent.y != 0)
        render(w, extent);
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
    if (e->keyboard.key == ' ')
        w.paused = !w.paused;
    else if (e->keyboard.key == PrAppKey_Escape)
        prAppStop(w.app);
}

// Opens the window. The window renders once the device is ready, and its close stops the application.
bool start(void* context, PrApp* app) {
    Window& w = *static_cast<Window*>(context);
    w.app = app;
    PrAppWindowDesc desc{
        .title = "Prax Video",
        .extent = {960, 720},
        .handler = {.context = &w, .open = open, .frame = frame, .event = event},
    };
    return prAppWindowCreate(app, &desc, &w.window) == PrStatus_Ok;
}

void appEvent(void*, PrApp*, const PrAppEvent*) {}

void shutdown(void* context, PrApp*) {
    Window& w = *static_cast<Window*>(context);
    closeDevice(w);
    prMediaDecoderDestroy(w.decoder);
    prMediaCodecDestroy(w.codec);
    prDrawListRelease(w.list);
}

} // namespace

int main(int argc, char** argv) {
    prCrashHandler();
    static Window w;
    PrStringView path{};
    for (int i = 1; i < argc; ++i) {
        PrStringView argument = argv[i];
        if (argument.size != 0 && argument.data[0] != '-') {
            path = argument;
        } else {
            printError("usage: video [FILE]\n");
            return 1;
        }
    }
    // The device is created on the pool while the video and the window open.
    w.pool = prTaskPoolCreate(PrTaskPoolBackend_Default, 1);
    requestDevice(w);
    // The example's own video is its resource, and a video that the arguments name is a file of the host.
    PrBlob* video = path.isEmpty() ? prResourceOpen("cat.mp4") : prFileOpen(path);
    if (path.isEmpty())
        path = "cat.mp4";
    if (video == nullptr) {
        printError("video: cannot read {}; ./dev.sh fetch videos fetches the example's own\n", path);
        closeDevice(w);
        prTaskPoolDestroy(w.pool);
        return 1;
    }
    w.codec = prMediaCodecCreate(PrMediaCodecBackend_Default);
    PrStatus opened = PrStatus_NotSupported;
    if (w.codec != nullptr)
        opened = prMediaDecoderCreate(w.codec, video, &w.decoder, &w.info);
    prBlobRelease(video);
    if (opened != PrStatus_Ok) {
        printError("video: no codec plays {}\n", path);
        closeDevice(w);
        prTaskPoolDestroy(w.pool);
        return 1;
    }
    w.list = prDrawListCreate();

    PrApp* app = prAppCreate(PrAppBackend_Default, nullptr);
    prAppRun(app, {.context = &w, .start = start, .event = appEvent, .shutdown = shutdown});
    prAppDestroy(app);
    prTaskPoolDestroy(w.pool);
    return 0;
}
