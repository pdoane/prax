// Alchemy: a studio for generating images with a ComfyUI server, in Studio's window (pr/studio_window.h).
//
// main() is the composition root. Alchemy chooses the services that its state uses: the platform's network with HTTP
// and WebSockets over it, a task pool for decoding images, and the image and compression codecs. Each frame takes the
// server's events and the decoded images into the state before the window's rounds build the interface.

#include "pr/app.h"
#include "pr/array.h"
#include "pr/assert.h"
#include "pr/atomic.h"
#include "pr/callback.h"
#include "pr/count_of.h"
#include "pr/draw_cpu.h"
#include "pr/draw_list.h"
#include "pr/draw_renderer.h"
#include "pr/environment.h"
#include "pr/file.h"
#include "pr/float.h"
#include "pr/float_convert.h"
#include "pr/http.h"
#include "pr/min_max.h"
#include "pr/net.h"
#include "pr/print.h"
#include "pr/string.h"
#include "pr/string_ops.h"
#include "pr/studio_app.h"
#include "pr/tls.h"
#include "pr/websocket.h"

#include "apps/alchemy/commands.h"
#include "apps/alchemy/view.h"

namespace {

using namespace alchemy;

// Alchemy in a window: its state and interface, and the services that the state uses.
struct Alchemy {
    AppState state;
    Shell shell{&state};
    Commands* commands = nullptr;
    View view;

    PrNet* net = nullptr;
    PrTls* PR_NULLABLE tls = nullptr;
    PrHttp* http = nullptr;
    PrWebSocketNetwork* websockets = nullptr;

    // The application, which the network's loop and the task pool wake when they deliver results.
    Atomic<PrApp*> app;     // starts null, as the state is static
    double lastTime = 0;

    // The file that $ALCHEMY_CAPTURE names, which a frame drawn $ALCHEMY_CAPTURE_SECONDS after the start is written to
    // as a PNG, rendered on the CPU, before the application quits. It lets a script see the interface without a screen.
    // $ALCHEMY_CAPTURE_COMMANDS names commands, separated by commas, that run halfway to the frame, to bring up what
    // the capture should show.
    String capturePath;         // the file to capture the window into, or empty for none
    String captureCommands;     // the commands to run before the capture, or empty for none
    double captureSeconds = 5;
    double startTime = -1;
    double frameTime = 0;       // of the frame that is being drawn
};

Alchemy& alchemyOf(void* context) {
    return *static_cast<Alchemy*>(context);
}

// Renders `list` on the CPU into a PNG file at `path`, at `extent` target pixels on the `canvas` color.
bool capture(Alchemy& a, const PrDrawList* list, PrStringView path, PrUVec2 extent, PrColor canvas) {
    PrDrawRenderer* cpu = prDrawCpuCreate(nullptr);
    if (cpu == nullptr)
        return false;
    Array<uint8_t> pixels(size_t(extent.x) * extent.y * 4);
    for (uint32_t i = 0; i < pixels.count(); i += 4) {
        pixels[i] = uint8_t(canvas.r * 255.0f + 0.5f);
        pixels[i + 1] = uint8_t(canvas.g * 255.0f + 0.5f);
        pixels[i + 2] = uint8_t(canvas.b * 255.0f + 0.5f);
        pixels[i + 3] = 255;
    }
    bool written = false;
    PrPixels into{pixels.data(), extent.x * 4, extent};
    if (prDrawRendererRenderPixels(cpu, list, into, PrPixelFormat_Rgba8) == PrStatus_Ok) {
        PrPixelView view{pixels.data(), extent.x * 4, extent};
        size_t size = 0;
        prImageCodecEncode(a.state.codec, "image/png", view, nullptr, 0, &size);
        Array<uint8_t> png(size);
        if (prImageCodecEncode(a.state.codec, "image/png", view, png.data(), png.count(), &size) == PrStatus_Ok)
            written = prFileWrite(path, PrByteView{png.data(), size});
    }
    prDrawRendererDestroy(cpu);
    return written;
}

void wake(Alchemy& a) {
    if (PrApp* app = a.app.load())
        prAppWake(app);
}

bool open(void* context, PrStudioWindow* window) {
    Alchemy& a = alchemyOf(context);
    PrApp* app = prStudioWindowApp(window);
    a.app.store(app);
    a.shell.app = app;
    a.shell.window = prStudioWindowAppWindow(window);
    a.shell.ui = prStudioWindowUi(window);
    a.commands = commandsCreate(&a.shell);
    connectServer(a.state);
    return true;
}

// The state takes what arrived since the last frame before the build reads it.
void beginFrame(void* context, PrStudioWindow*, const PrAppFrame* f) {
    Alchemy& a = alchemyOf(context);
    double dt = a.lastTime != 0 ? f->time - a.lastTime : 0;
    a.lastTime = f->time;
    a.frameTime = f->time;
    pumpEvents(a.state, dt);
    imageCachePump(a.state.images);
    applyDimensions(a.state);
    noteVisit(a.state);
}

// Runs the capture's commands halfway to its moment, and at its moment writes the frame and quits. A capture waits for
// its moment, which no input announces.
void drawn(void* context, PrStudioWindow* window, const PrDrawList* list, PrUVec2 extent, PrColor canvas) {
    Alchemy& a = alchemyOf(context);
    if (a.startTime < 0)
        a.startTime = a.frameTime;
    double elapsed = a.frameTime - a.startTime;
    if (!a.captureCommands.isEmpty() && elapsed >= a.captureSeconds / 2) {
        String commands = move(a.captureCommands);
        PrStringView rest = commands;
        for (;;) {
            size_t end = prStringFindByte(rest, ',');
            runCommand(*a.commands, prStringSlice(rest, 0, end));
            if (end == rest.size)
                break;
            rest = prStringSlice(rest, end + 1, rest.size - end - 1);
        }
    }
    if (a.capturePath.isEmpty())
        return;
    if (elapsed >= a.captureSeconds && extent.x != 0) {
        if (!capture(a, list, a.capturePath, extent, canvas))
            printError("alchemy: could not write {}\n", a.capturePath);
        prAppStop(prStudioWindowApp(window));
        return;
    }
    prAppWindowRequestFrame(prStudioWindowAppWindow(window));
}

// The services go in the reverse of their making: the client lets go of the loop, the sources on the loop go on it, and
// the pool finishes its decodes and surveys before the state that they write to.
void closeServices(void* context, PrStudioWindow*) {
    Alchemy& a = alchemyOf(context);
    a.app.store(nullptr);
    if (a.shell.app != nullptr)
        prAppCancel(a.shell.app, &a.shell);
    comfyClientDestroy(a.state.client);
    auto destroyOnLoop = [&] {
        prWebSocketNetworkDestroy(a.websockets);
        prHttpDestroy(a.http);
    };
    prNetPostBlocking(a.net, PR_SCOPED_CALLBACK(destroyOnLoop));
    prTlsDestroy(a.tls);
    prNetDestroy(a.net);
    imageCacheDestroy(a.state.images);
    stateReset(a.state);
    prTaskPoolDestroy(a.state.pool);
    prImageCodecDestroy(a.state.codec);
    prCompressCodecDestroy(a.state.compress);
    viewReset(a.view);
    commandsDestroy(a.commands);
}

} // namespace

int main() {
    prCrashHandler();
    static Alchemy a;
    // The services of the state.
    a.net = prNetCreate(PrNetBackend_Default);
    a.tls = prTlsCreate(PrTlsBackend_Default, a.net);
    PrHttpDesc httpDesc{.net = a.net, .tls = a.tls};
    a.http = prHttpCreate(PrHttpBackend_Default, &httpDesc);
    PrWebSocketNetworkDesc socketDesc{.net = a.net, .tls = a.tls};
    a.websockets = prWebSocketNetworkCreate(PrWebSocketNetworkBackend_Default, &socketDesc);
    a.state.compress = prCompressCodecCreate(PrCompressCodecBackend_Default);
    a.state.codec = prImageCodecCreate(PrImageCodecBackend_Default, a.state.compress);
    a.state.pool = prTaskPoolCreate(PrTaskPoolBackend_Default, 4);
    a.state.images = imageCacheCreate(a.state.pool, a.state.codec, [] { wake(a); });
    a.state.client = comfyClientCreate({
        .net = a.net,
        .http = a.http,
        .websockets = a.websockets,
        .wake = [] { wake(a); },
    });
    String server = String::adopt(prEnvironmentVariable("ALCHEMY_SERVER"));
    if (!server.isEmpty())
        a.state.server = move(server);
    a.capturePath = String::adopt(prEnvironmentVariable("ALCHEMY_CAPTURE"));
    a.captureCommands = String::adopt(prEnvironmentVariable("ALCHEMY_CAPTURE_COMMANDS"));
    String seconds = String::adopt(prEnvironmentVariable("ALCHEMY_CAPTURE_SECONDS"));
    prF64ParsePrefix(seconds, &a.captureSeconds);
    galleryOpen(a.state.gallery, galleryDirectory());
    viewInit(a.view);

    // Alchemy uses only the system's fonts.
    PrFontSystem* system = prFontSystemCreate(PrFontSystemBackend_Default);
    PrFontSet* fonts = prFontSetCreate(nullptr);
    prFontSetSetSystem(fonts, system);
    PrStudioWindowDesc desc{
        .title = "Alchemy",
        .extent = {1500, 940},
        .decoration = PrAppWindowDecoration_Custom,
        .open = open,
        .beginFrame = beginFrame,
        .build = [](void* context, PrStudioWindow* window, PrLayoutNode root, PrVec2) {
            Alchemy& alchemy = alchemyOf(context);
            buildView(prStudioWindowKit(window), root, alchemy.view, *alchemy.commands);
        },
        .titleBar = [](void* context, PrStudioWindow* window, PrLayoutNode bar) {
            Alchemy& alchemy = alchemyOf(context);
            buildTitleBarRow(prStudioWindowKit(window), bar, alchemy.view, *alchemy.commands);
        },
        .drawn = drawn,
        .close = closeServices,
        .context = &a,
    };
    // Alchemy asks for Multiple windows, so that its menus, tooltips and dialogs show in windows of their own. A
    // capture asks for Single, so that the captured window shows what its commands open.
    PrAppOptions options = prAppOptionsDefault();
    options.windowModel = a.capturePath.isEmpty() ? PrAppWindowModel_Multiple : PrAppWindowModel_Single;
    PrStudioAppDesc app{.fonts = fonts, .options = &options, .window = &desc};
    PrStatus status = prStudioAppRun(&app);
    prFontSetDestroy(fonts);
    prFontSystemDestroy(system);
    if (status == PrStatus_NotSupported)
        printError("alchemy: no GPU device to render with\n");
    return status == PrStatus_Ok ? 0 : 1;
}
