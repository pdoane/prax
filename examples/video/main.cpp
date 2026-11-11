// The video example: a video played with lib/media and drawn with lib/draw, in a window.
//
// main() is the composition root. Each frame asks the decoder for the video's frame at the clock's time, and makes a
// texture of the frame's image (prGpuTextureImport). Metal imports a CVPixelBuffer without a copy, and a web page's
// Canvas 2D device imports a VideoFrame without a copy. The list draws the texture as a source
// (PrDrawSourceKind_Texture), fitted to the window, under rounded corners and over a shadow, with the video's progress
// beneath it. Space pauses the clock, and Escape quits.
//
// Usage: video [--renderer NAME] [FILE]. NAME is the name of one of lib/draw's backends, and Default if it is not
// given. FILE is the video to play, and the example's own video if it is not given (./dev.sh fetch videos). On the web,
// the page's query passes the arguments.

#include "pr/app.h"
#include "pr/draw_list.h"
#include "pr/draw_renderer.h"
#include "pr/file.h"
#include "pr/float.h"
#include "pr/gpu.h"
#include "pr/media.h"
#include "pr/print.h"

namespace {

using namespace pr;

// The window, the device that it renders on, and the video that it plays.
struct Window {
    PrApp* app = nullptr;
    PrAppWindow* window = nullptr;
    PrGpuDevice* device = nullptr;
    PrGpuSwapChain* swapChain = nullptr;
    PrDrawRenderer* renderer = nullptr;
    PrDrawFrame* frame = nullptr;
    PrDrawList* list = nullptr;
    PrDrawRendererBackend backend = PrDrawRendererBackend_Default;

    PrByteView file{};
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
        prGpuTextureDestroy(w.device, w.texture);
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

// Makes the platform's device, a swap chain over the window, and the renderer. Returns false if the window has no
// device to render with.
bool openDevice(Window& w) {
    PrSurface surface{};
    PrGpuSwapChainDesc desc{.intent = PrGpuSwapChainIntent_Hdr};
    if (prGpuDeviceCreate(PrGpuBackend_Default, nullptr, &w.device) != PrStatus_Ok ||
            prAppWindowSurface(w.window, &surface) != PrStatus_Ok ||
            prGpuSwapChainCreate(w.device, &surface, &desc, &w.swapChain) != PrStatus_Ok ||
            prDrawRendererCreate(w.backend, w.device, nullptr, &w.renderer) != PrStatus_Ok) {
        closeDevice(w);
        return false;
    }
    w.frame = prDrawFrameCreate(w.renderer);
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
    PrDrawSource source = prDrawSourceTexture(prDrawGpuTexture(w.texture), extent,
        PrRect{0, 0, float(extent.x), float(extent.y)}, PrDrawAlpha_Opaque);
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
    // A lost device is made again, with everything on it.
    if (w.device != nullptr && prGpuDeviceIsLost(w.device))
        closeDevice(w);
    if (w.device == nullptr && !openDevice(w))
        return {};
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

// Opens the window and the device that renders to it. The window's close stops the application.
bool start(void* context, PrApp* app) {
    Window& w = *static_cast<Window*>(context);
    w.app = app;
    PrAppWindowDesc desc{
        .title = "Prax Video",
        .extent = {960, 720},
        .handler = {.context = &w, .open = open, .frame = frame, .event = event},
    };
    if (prAppWindowCreate(app, &desc, &w.window) != PrStatus_Ok)
        return false;
    if (!openDevice(w)) {
        printError("video: no GPU device to render with\n");
        return false;
    }
    print("video: rendering with {} on {}\n", prDrawRendererBackendName(w.backend), prGpuDeviceInfo(w.device).backend);
    return true;
}

void appEvent(void*, PrApp*, const PrAppEvent*) {}

void shutdown(void* context, PrApp*) {
    Window& w = *static_cast<Window*>(context);
    closeDevice(w);
    prMediaDecoderDestroy(w.decoder);
    prMediaCodecDestroy(w.codec);
    prFileUnmap(&w.file);
    prDrawListRelease(w.list);
}

} // namespace

int main(int argc, char** argv) {
    prCrashHandler();
    static Window w;
    PrStringView path = PR_VIDEO_FILE;
    for (int i = 1; i < argc; ++i) {
        PrStringView argument = argv[i];
        if (argument == "--renderer" && i + 1 < argc) {
            w.backend = prDrawRendererBackendFromName(argv[++i]);
            if (!prDrawRendererBackendIsBuilt(w.backend)) {
                printError("video: no renderer named {} in this build\n", PrStringView{argv[i]});
                return 1;
            }
        } else if (argument.size != 0 && argument.data[0] != '-') {
            path = argument;
        } else {
            printError("usage: video [--renderer NAME] [FILE]\n");
            return 1;
        }
    }
    if (!prFileMap(path, &w.file)) {
        printError("video: cannot read {}; ./dev.sh fetch videos fetches the example's own\n", path);
        return 1;
    }
    w.codec = prMediaCodecCreate(PrMediaCodecBackend_Default);
    if (w.codec == nullptr || prMediaDecoderCreate(w.codec, w.file, &w.decoder, &w.info) != PrStatus_Ok) {
        printError("video: no codec plays {}\n", path);
        return 1;
    }
    w.list = prDrawListCreate();

    PrApp* app = prAppCreate(PrAppBackend_Default, nullptr);
    prAppRun(app, {.context = &w, .start = start, .event = appEvent, .shutdown = shutdown});
    prAppDestroy(app);
    return 0;
}
