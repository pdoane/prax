// The video example: a video played with lib/media and drawn with lib/draw, in a window.
//
// main() is the composition root. Each frame asks the decoder for the video's frame at the clock's time, and makes a
// texture of the frame's image (prGpuTextureImport). Metal imports a CVPixelBuffer without a copy, and a web page's
// Canvas 2D device imports a VideoFrame without a copy. The list draws the texture as a source
// (PrDrawSourceKind_Texture), fitted to the window, under rounded corners and over a shadow, with the video's progress
// beneath it. Space pauses the clock, and Escape quits.
//
// T shows the video as the fill of text instead. A frame's texture is a source that a brush paints with, and a brush
// fills glyphs as it fills any shape, so the letters are windows onto the video.
//
// Usage: video [FILE]. FILE is the video to play. Without it, the example plays its own video, the resource cat.mp4
// (./dev.sh fetch videos). On the web, the page's query passes the arguments. PR_BACKEND_RENDERER chooses the renderer.

#include "pr/app.h"
#include "pr/app_select.h"
#include "pr/array.h"
#include "pr/atomic.h"
#include "pr/blob_ref.h"
#include "pr/draw_host_texture.h"
#include "pr/draw_list.h"
#include "pr/draw_renderer.h"
#include "pr/draw_select.h"
#include "pr/file.h"
#include "pr/float.h"
#include "pr/font.h"
#include "pr/gpu.h"
#include "pr/gpu_select.h"
#include "pr/media.h"
#include "pr/print.h"
#include "pr/resource.h"
#include "pr/status.h"
#include "pr/task.h"
#include "pr/text.h"
#include "pr/texture.h"

namespace {

using namespace pr;

// The window, the device that it renders on, and the video that it plays.
struct Window {
    PrApp* app = nullptr;
    Atomic<PrApp*> wakes;           // the application, for the thread that creates the device
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

    // The frame that is shown, as a host texture, and that frame's serial.
    PrDrawHostTexture* shown = nullptr;
    uint64_t frameSerial = 0;
    bool reported = false;          // true once the example has printed whether frames are imported or copied

    // The text that the video fills while `lettered` is set, and what shapes it: a heavy font, the shaper and its
    // glyphs.
    bool lettered = false;
    const PrFont* font = nullptr;
    PrTextShaper* shaper = nullptr;
    PrTextGlyphBuffer* glyphs = nullptr;
};

// The text that the video fills.
constexpr PrStringView kLetters = "PRAX";

// Frees what the window made on its device, and the device.
void closeDevice(Window& w) {
    // The list holds the frame that it drew, so it is emptied before the device goes.
    if (w.list != nullptr)
        prDrawListClear(w.list);
    prDrawHostTextureRelease(w.shown);
    w.shown = nullptr;
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
    // The window draws nothing until the device is ready, and asks for no frame. The end of the creation wakes the
    // application, which asks the window for a frame.
    options.ready = [](void* PR_NULLABLE context, PrGpuDevice*) {
        // A creation that ends before the application starts needs no wake: the window's first frame finds the device.
        if (PrApp* app = static_cast<Window*>(context)->wakes.load())
            prAppWake(app);
    };
    options.readyContext = &w;
    w.device = prGpuDeviceCreate(PrGpuBackend_Default, &options);
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

// Makes the window's host texture show a decoded frame that has not been shown yet. The device imports the frame's
// image in place if it can, and otherwise copies its texels. No list draws the last frame by now, so the import goes
// into the same texture.
void showFrame(Window& w, const PrMediaFrame& f) {
    if (f.serial == w.frameSerial && w.shown != nullptr)
        return;
    if (prDrawHostTextureImport(w.device, &f.image, PrTextureAlpha_None, &w.shown) != PrStatus_Ok)
        return;
    w.frameSerial = f.serial;
    if (!w.reported) {
        bool inPlace = prGpuDeviceImportsInPlace(w.device, f.image.kind);
        print("video: {} {} each frame\n", prGpuDeviceInfo(w.device).backend, inPlace ? "imports" : "copies");
        w.reported = true;
    }
}

// Records the text with the video as its fill. The text is as wide as the window allows, and the video covers the
// text's box, cropped to the box's shape, so that the letters show the middle of the picture.
void recordLetters(Window& w, float width, float height) {
    PrDrawList* list = w.list;
    if (w.font == nullptr)
        return;
    // The text is shaped at a size of 100 to learn its width, and again at the size that fits.
    PrTextRun run{};
    run.font = w.font;
    run.text = kLetters;
    run.pixelsPerEm = 100;
    if (prTextShaperShape(w.shaper, &run, w.glyphs) != PrStatus_Ok)
        return;
    Array<PrFontGlyph> glyphs;
    glyphs.resize(prTextGlyphBufferGlyphCount(w.glyphs));
    float advance = prTextGlyphBufferPlace(w.glyphs, 100, PrVec2{0, 0}, glyphs.data(), nullptr).x;
    if (!(advance > 0))
        return;
    float size = 100 * (width - 96) / advance;
    run.pixelsPerEm = size;
    if (!(size > 0) || prTextShaperShape(w.shaper, &run, w.glyphs) != PrStatus_Ok)
        return;
    // The capitals stand on the baseline and reach about seven tenths of the size above it.
    float capHeight = size * 0.72f;
    PrVec2 origin{48, (height + capHeight) / 2};
    prTextGlyphBufferPlace(w.glyphs, size, origin, glyphs.data(), nullptr);
    PrRect box{origin.x, origin.y - capHeight, width - 48, origin.y};
    // The picture covers the box and keeps its shape.
    PrUVec2 extent = prDrawHostTextureExtent(w.shown);
    float scale = prF32Max((box.right - box.left) / float(extent.x), (box.bottom - box.top) / float(extent.y));
    PrVec2 middle{(box.left + box.right) / 2, (box.top + box.bottom) / 2};
    PrRect area{
        middle.x - float(extent.x) * scale / 2, middle.y - float(extent.y) * scale / 2,
        middle.x + float(extent.x) * scale / 2, middle.y + float(extent.y) * scale / 2,
    };
    PrDrawBrush brush = prDrawBrushImage(prDrawSourceHostTexture(list, w.shown), area, PrDrawSampling_Linear);
    PrFontGlyphRun drawn{};
    drawn.font = w.font;
    drawn.size = size;
    drawn.glyphs = glyphs.data();
    drawn.glyphCount = glyphs.count();
    prDrawListGlyphs(list, &drawn, PrVec2{}, {.brush = &brush});
}

// Records the frame: the video fitted to the window over its shadow, under rounded corners, and its progress beneath.
void record(Window& w, float width, float height, double seconds) {
    PrDrawList* list = w.list;
    prDrawListRect(list, PrRect{0, 0, width, height}, {.color = PrColor{0.09f, 0.08f, 0.1f, 1}});
    if (w.shown == nullptr)
        return;
    if (w.lettered) {
        recordLetters(w, width, height);
        return;
    }
    PrUVec2 extent = prDrawHostTextureExtent(w.shown);
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
    PrDrawSource source = prDrawSourceHostTexture(list, w.shown);
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
    if (!openDevice(w))
        return PrAppOutput{};
    prGpuSwapChainWait(w.swapChain);
    if (!w.paused)
        w.clock += f->delta;
    // The video loops.
    double seconds = w.clock;
    if (prF64IsFinite(w.info.duration) && w.info.duration > 0)
        seconds = prF64FloorMod(w.clock, w.info.duration);
    // The list lets go of the frame that it drew before the next frame is imported.
    prDrawListClear(w.list);
    PrMediaFrame decoded{};
    if (prMediaDecoderFrame(w.decoder, seconds, &decoded))
        showFrame(w, decoded);

    // The list is in logical pixels, and its transform takes them to the display's target pixels.
    float ratio = f->display.pixelRatio;
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
    else if (e->keyboard.key == 't' || e->keyboard.key == 'T')
        w.lettered = !w.lettered;
    else if (e->keyboard.key == PrAppKey_Escape)
        prAppStop(w.app);
}

// Opens the window. The window renders once the device is ready, and its close stops the application.
bool start(void* context, PrApp* app) {
    Window& w = *static_cast<Window*>(context);
    w.app = app;
    w.wakes.store(app);
    PrAppWindowDesc desc{
        .title = "Prax Video",
        .extent = {960, 720},
        .handler = {.context = &w, .open = open, .frame = frame, .event = event},
    };
    return prAppWindowCreate(app, &desc, &w.window) == PrStatus_Ok;
}

// A wake asks the window for a frame, which takes up the device that became ready.
void appEvent(void* context, PrApp*, const PrAppEvent* e) {
    Window& w = *static_cast<Window*>(context);
    if (e->kind == PrAppEventKind_Wake && w.window != nullptr)
        prAppWindowRequestFrame(w.window);
}

void shutdown(void* context, PrApp*) {
    Window& w = *static_cast<Window*>(context);
    closeDevice(w);
    prMediaDecoderDestroy(w.decoder);
    prMediaCodecDestroy(w.codec);
    prDrawListRelease(w.list);
    prTextGlyphBufferDestroy(w.glyphs);
    prTextShaperDestroy(w.shaper);
    prFontRelease(w.font);
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
    // The text that the video fills is set in the heaviest weight of the example's font, so that the letters show much
    // of the picture.
    w.shaper = prTextShaperCreate(PrTextShaperBackend_Default);
    w.glyphs = prTextGlyphBufferCreate();
    const PrFont* base = nullptr;
    if (BlobRef resource = BlobRef::adopt(prResourceOpen("font_bundle/NotoSans[wdth,wght].ttf"))) {
        if (prFontCreate(resource, 0, nullptr, &base) == PrStatus_Ok) {
            const PrFontVariation heavy{PR_FONT_TAG('w', 'g', 'h', 't'), 900};
            PrFontDerivation derivation = prFontDerivationDefault();
            derivation.variations = &heavy;
            derivation.variationCount = 1;
            w.font = prFontCreateDerived(base, &derivation);
            prFontRelease(base);
        }
    }

    PrApp* app = prAppCreate(PrAppBackend_Default, nullptr);
    prAppRun(app, {.context = &w, .start = start, .event = appEvent, .shutdown = shutdown});
    prAppDestroy(app);
    prTaskPoolDestroy(w.pool);
    return 0;
}
