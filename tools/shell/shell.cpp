// Command-line host for the engine's JavaScript APIs.
//
// Evaluates scripts, or loads HTML pages in a page host (tools/shell/page_host.h). --html loads a page by its URL, or
// by its path, which loads at its file: URL. --font installs a font for the pages, under the family that its name table
// gives.
//
// --network fetches over sockets, as a browser does, rather than answering each fetch on the shell's thread in a fixed
// order. --resolve <host> <address> resolves a host, or each subdomain of `*.<name>`, to an address, as a hosts file
// does, and --ca <pem> trusts a certificate authority besides the system's. --cache <directory> keeps the HTTP cache on
// disk in the directory, where it outlives the run, as a browser's does. Each implies --network. --virtual-time runs
// the pages on a virtual clock that advances only when they wait (PageHost::useVirtualTime).
//
// --screenshot writes the next page's rendering as a PNG once every page has settled, as WPT's reftests capture it: in
// the viewport, on white, with lib/draw's cpu renderer, or the renderer that --renderer names, on the platform's device
// for a renderer that renders on one. --reference, which may repeat, names a page that the next page is compared with,
// as a reftest compares them. Once every page has settled, each page is rendered as a screenshot is, and a line
// `REFTEST <json>` on stdout gives, for each reference in order, the figures of tools/suite/reftest.cpp's answer: the
// largest channel difference, the number of pixels that differ, and whether the reference draws anything. --write
// <stem>, before the next page, writes what its screenshot and comparisons render, as tools/suite/reftest.py names a
// case's: the page at `<stem>.png`, each reference at `<stem>-ref.png` and its difference at `<stem>-diff.png`,
// numbered from the second reference on, as in `<stem>-ref2.png`. --viewport WxH sets the viewport that screenshots and
// --verify lay the pages out in. It is 800x600 by default, the size that a window starts with and that WPT's reftests
// are captured in. The pages of suites/prax are laid out for 600x800. --pixel-ratio sets the target pixels per CSS
// pixel that pages render at and that screenshots and comparisons are taken at, 1 by default. A screenshot covers the
// viewport in target pixels, rounded up. A page whose root element still has the class `reftest-wait` once the pages
// settle is rendered and sent TestRendered, the event of WPT's reftest protocol that says the page has rendered, and
// the pages settle again. When a screenshot or comparison is taken, a page whose root still has the class has not
// finished, which fails the run. --capture-wait <script> runs a script in each page that is captured once it has
// loaded, which calls stopRun() once the page is ready (PageHost::setCaptureWait), as WPT's test-wait.js waits for a
// reftest's screenshot.
//
// --html also opens a page that tools/pages/pages.py captured, by its JSON file. The page is the HTML file beside the
// JSON, and loads at the URL that it was captured from. The resources that the capture saved are answered from their
// files.
//
// --no-scripts loads the pages that follow without running their own scripts. --dark loads them for a user who prefers
// a dark color scheme.
//
// --shaper shapes the pages' text with the shaper of lib/text that it names, the default shaper otherwise, --hinting
// hints their fonts as the hinting that it names with the default hinter, none by default, and --media reads their
// media resources with the codec of lib/media that it names, the built-in codec otherwise, which decodes no frames.
// Each sets the process's backend, so a served run (--serve) uses the backends that the process started with.
//
// --restyle-threads N restyles on N threads, including the event loop's own thread, as
// PrDomAgentOptions::restyleThreads describes: 0, the default, means one thread per core, and 1 means the event loop's
// thread alone. It must come before any page.
//
// --timeout stops waiting for the pages once that many milliseconds of the system's time have passed. The run then
// exits with status 124, as a run stopped by its deadline does, without answering what is asked of the pages. What the
// pages wrote is kept, which a run killed at its deadline may lose.
//
// --serve serves runs read from stdin in one process, which saves a process start for each run (serve).
//
// --verify checks each page's presentation against a full recomputation (prCssPresentationVerify) after every rendering
// update and once the pages settle, and writes each page's first failure to stderr as "<page>: render verify: ...". A
// failure does not change the exit status, so a runner reads the pages' own results as it would without --verify.

#include "pr/assert.h"
#include "pr/draw_cpu.h"
#include "pr/draw_renderer.h"
#include "pr/float.h"
#include "pr/font_hinter.h"
#include "pr/media.h"
#include "pr/text.h"

#include "tools/harness/compare.h"
#include "tools/harness/harness.h"
#include "tools/harness/harness_png.h"
#include "tools/harness/harness_streams.h"
#include "tools/shell/device_render.h"
#include "tools/shell/page_host.h"

#include <filesystem>
#include <memory>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <utility>
#include <vector>

using namespace pr;
using namespace harness;
using namespace shell;
using page_host::Page;
using page_host::PageHost;

namespace {

// The viewport that a window starts with (css_media.cpp), which WPT's reftests are captured in.
constexpr PrVec2 kDefaultViewport = {800, 600};

// The renderers that captures are drawn with. A renderer on the device, and the device, are made on first use, and kept
// for the process, which a served shell renders many pages in.
struct Renderers {
    PrDrawRenderer* cpu = nullptr;
    DeviceRender* device = nullptr;
    PrDrawRendererBackend deviceBackend = PrDrawRendererBackend_Default;   // the backend of `device`, once it was tried

    void reset() {
        deviceRenderDestroy(device);
        prDrawRendererDestroy(cpu);
    }
};

// Renders `list` into `pixels`, `extent` of RGBA, with the renderer of `backend` on the device. Returns false if there
// is no device that it renders on.
bool renderOnDevice(Renderers& r, PrDrawRendererBackend backend, const PrDrawList* list, PrUVec2 extent, uint8_t* pixels) {
    if (r.deviceBackend != backend) {
        deviceRenderDestroy(r.device);
        r.device = deviceRenderCreate(backend);
        r.deviceBackend = backend;
    }
    // A page draws its canvas opaque, so the device's premultiplied pixels are the same straight.
    return r.device != nullptr && deviceRenderPixels(r.device, list, extent, pixels);
}

// A page's rendering in the viewport, on white, as straight RGBA.
struct Render {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgba;
};

// Renders the page in its current state, over its canvas, as WPT's reftests capture it. Returns false, saying why on
// stderr, if the page has not finished or has no rendering.
bool renderPage(PageHost& host, Page* page, PrVec2 viewport, float pixelRatio, PrDrawRendererBackend backend,
        Renderers& renderers, Render* out) {
    const std::string pathText = PageHost::pageName(page);
    const char* file = pathText.c_str();
    if (!PageHost::finished(page)) {
        fprintf(stderr, "%s: reftest-wait was not removed\n", file);
        return false;
    }
    PrDrawList* list = prDrawListCreate();
    // The page paints in CSS pixels, which the transform maps to target pixels.
    prDrawListPushTransformAffine2(list, prAffine2Scale(PrVec2{pixelRatio, pixelRatio}));
    PrAppDisplay display = prAppDisplayDefault();
    display.pixelRatio = pixelRatio;
    bool ok = host.paint(page, viewport, display, host.userPreferences(), list);
    prDrawListPopTransform(list);
    if (!ok)
        fprintf(stderr, "%s: no rendering to capture\n", file);
    out->width = uint32_t(ceilF64(double(viewport.x) * double(pixelRatio)));
    out->height = uint32_t(ceilF64(double(viewport.y) * double(pixelRatio)));
    out->rgba.assign(size_t(out->width) * out->height * 4, 0);
    PrPixels into{out->rgba.data(), size_t(out->width) * 4, {out->width, out->height}};
    if (ok && prDrawRendererBackendIsDevice(backend)) {
        ok = renderOnDevice(renderers, backend, list, into.extent, out->rgba.data());
        if (!ok)
            fprintf(stderr, "%s: the %s renderer could not render\n", file, prDrawRendererBackendName(backend).data);
    } else {
        ok = ok && prDrawRendererRenderPixels(renderers.cpu, list, into, PrPixelFormat_Rgba8) == PrStatus_Ok;
    }
    prDrawListRelease(list);
    return ok;
}

// Writes the render as a PNG to `path`. Returns false, saying why on stderr, if it cannot.
bool writePng(const Render& render, Page* page, const std::string& path) {
    bool ok = harnessWritePng(path.c_str(), PrPixelView{
        render.rgba.data(), size_t(render.width) * 4,
        {render.width, render.height},
    });
    if (!ok)
        fprintf(stderr, "%s: cannot write the screenshot %s\n", PageHost::pageName(page).c_str(), path.c_str());
    return ok;
}

// What is asked of a page loaded with --html once the pages settle: a screenshot, the references that it is compared
// with, and the stem of the renders to write (--write).
struct Request {
    Page* page;
    std::string screenshot;         // empty for none
    std::vector<Page*> references;
    std::string write;              // empty for none
};

// Renders the request's page and writes its screenshot, and compares it with each of its references. Returns false,
// saying why on stderr, if a page cannot be rendered or the screenshot written.
bool answer(PageHost& host, const Request& request, PrVec2 viewport, float pixelRatio, PrDrawRendererBackend backend,
        Renderers& renderers) {
    if (request.screenshot.empty() && request.references.empty() && request.write.empty())
        return true;
    Render render;
    if (!renderPage(host, request.page, viewport, pixelRatio, backend, renderers, &render))
        return false;
    if (!request.screenshot.empty() && !writePng(render, request.page, request.screenshot))
        return false;
    if (!request.write.empty() && !writePng(render, request.page, request.write + ".png"))
        return false;
    size_t stride = size_t(render.width) * 4;
    for (size_t r = 0; r < request.references.size(); ++r) {
        Page* reference = request.references[r];
        Render expected;
        if (!renderPage(host, reference, viewport, pixelRatio, backend, renderers, &expected))
            return false;
        if (!request.write.empty()) {
            std::string number = r == 0 ? "" : std::to_string(r + 1);
            Render diff{render.width, render.height, std::vector<uint8_t>(render.rgba.size())};
            compareImages(expected.rgba.data(), render.rgba.data(), render.width, render.height, stride,
                ImageComparison{}, diff.rgba.data());
            if (!writePng(expected, reference, request.write + "-ref" + number + ".png") ||
                    !writePng(diff, request.page, request.write + "-diff" + number + ".png"))
                return false;
        }
        PixelDifference d = countDifferences(expected.rgba.data(), render.rgba.data(), render.width, render.height, stride);
        bool blank = !imageDrawsAnything(expected.rgba.data(), expected.width, expected.height, stride);
        printf("REFTEST {\"maxDifference\":%u,\"differingPixels\":%llu,\"blank\":%s}\n", d.maxDifference,
            static_cast<unsigned long long>(d.differingPixels), blank ? "true" : "false");
    }
    return true;
}

// What the arguments of one run ask for: the settings that apply to the whole process, and the pages, with what is
// asked of each once they settle.
struct Run {
    bool ok = true;
    bool verify = false;
    bool serve = false;
    bool network = false;           // the host fetches over sockets
    double timeout = -1;            // in milliseconds of the system's time, or negative for none
    PrVec2 viewport = kDefaultViewport;
    float pixelRatio = 1;                                       // target pixels per CSS pixel of screenshots and
                                                                // comparisons
    PrDrawRendererBackend renderer = PrDrawRendererBackend_Cpu; // what renders screenshots and comparisons
    std::string screenshot;
    std::vector<std::string> references;
    std::string write;
    std::vector<Request> requests;
};

// Has the host fetch over sockets, once. Returns false, saying why on stderr, if it cannot.
bool network(PageHost& host, Run& run) {
    if (run.network)
        return true;
    run.network = host.useNetwork();
    return run.network;
}

// Applies the arguments of a run to the host, loading its pages and running its scripts in order.
void parse(PageHost& host, const std::vector<std::string>& args, Run& run) {
    for (size_t i = 0; run.ok && i < args.size(); ++i) {
        const std::string& option = args[i];
        bool takesTwo = option == "--resolve";
        bool takesOne = option == "--prelude" || option == "--font" || option == "--screenshot" ||
            option == "--write" || option == "--reference" || option == "--html" ||
            option == "--viewport" || option == "--pixel-ratio" || option == "--renderer" ||
            option == "--timeout" || option == "--restyle-threads" || option == "--shaper" || option == "--media" ||
            option == "--hinting" || option == "--ca" || option == "--capture-wait" || option == "--cache";
        if ((takesOne && i + 1 >= args.size()) || (takesTwo && i + 2 >= args.size())) {
            fprintf(stderr, "%s needs %s\n", option.c_str(), takesTwo ? "two arguments" : "an argument");
            run.ok = false;
        } else if (option == "--network") {
            run.ok = network(host, run);
        } else if (takesTwo) {
            run.ok = network(host, run);
            host.addHostRule(args[i + 1], args[i + 2]);
            i += 2;
        } else if (option == "--virtual-time") {
            host.useVirtualTime();
        } else if (option == "--no-scripts") {
            host.disableScripts();
        } else if (option == "--dark") {
            host.setUserPreferences(PrAppUserPreferences{.colorScheme = PrAppColorScheme_Dark});
        } else if (option == "--verify") {
            host.verifyRendering();
            run.verify = true;
        } else if (option == "--serve") {
            run.serve = true;
        } else if (!takesOne) {
            run.ok = host.runFile(option.c_str());
        } else {
            const std::string& value = args[++i];
            if (option == "--prelude") {
                host.addPrelude(value);
            } else if (option == "--font") {
                run.ok = host.addFont(value);
            } else if (option == "--cache") {
                run.ok = host.useDiskCache(value);
                run.network = run.ok;
            } else if (option == "--ca") {
                run.ok = network(host, run) && host.addTrustAnchor(value);
            } else if (option == "--capture-wait") {
                run.ok = host.setCaptureWait(value);
            } else if (option == "--screenshot") {
                run.screenshot = value;
            } else if (option == "--write") {
                run.write = value;
            } else if (option == "--reference") {
                run.references.push_back(value);
            } else if (option == "--shaper" || option == "--hinting" || option == "--media") {
                // main reads these options, before the host is created.
            } else if (option == "--restyle-threads") {
                char* end = nullptr;
                unsigned long threads = strtoul(value.c_str(), &end, 10);
                run.ok = end != value.c_str() && *end == '\0';
                if (!run.ok)
                    fprintf(stderr, "--restyle-threads takes a count of threads\n");
                else
                    host.setRestyleThreads(uint32_t(threads));
            } else if (option == "--timeout") {
                char* end = nullptr;
                run.timeout = strtod(value.c_str(), &end);
                run.ok = end != value.c_str() && *end == '\0' && run.timeout >= 0;
                if (!run.ok)
                    fprintf(stderr, "--timeout takes milliseconds\n");
            } else if (option == "--viewport") {
                unsigned width = 0;
                unsigned height = 0;
                run.ok = sscanf(value.c_str(), "%ux%u", &width, &height) == 2 && width != 0 && height != 0;
                if (!run.ok)
                    fprintf(stderr, "--viewport takes WxH, as 600x800\n");
                run.viewport = PrVec2{float(width), float(height)};
                host.setViewport(run.viewport);
            } else if (option == "--renderer") {
                run.renderer = prDrawRendererBackendFromName(value);
                run.ok = run.renderer != PrDrawRendererBackend_Default && prDrawRendererBackendIsBuilt(run.renderer);
                if (!run.ok)
                    fprintf(stderr, "--renderer names no renderer of lib/draw that the build has: %s\n", value.c_str());
            } else if (option == "--pixel-ratio") {
                char* end = nullptr;
                run.pixelRatio = strtof(value.c_str(), &end);
                run.ok = end != value.c_str() && *end == '\0' && run.pixelRatio > 0 && run.pixelRatio <= 8;
                if (!run.ok)
                    fprintf(stderr, "--pixel-ratio takes target pixels per CSS pixel, as 1.5\n");
            } else {
                // A capture loads at the URL that it was captured from.
                std::string page = value;
                if (std::filesystem::path(page).extension() == ".json")
                    run.ok = host.addCapture(value, &page);
                // A page that is captured, or compared with references, waits until it is ready, as a reftest does.
                PageHost::Capture capture = PageHost::Capture::None;
                if (!run.screenshot.empty() || !run.references.empty() || !run.write.empty())
                    capture = PageHost::Capture::Wait;
                Request request{run.ok ? host.load(page, capture) : nullptr, run.screenshot, {}, run.write};
                run.ok = request.page != nullptr && PageHost::pageOk(request.page);
                for (size_t r = 0; run.ok && r < run.references.size(); ++r) {
                    Page* reference = host.load(run.references[r], PageHost::Capture::Wait);
                    run.ok = reference != nullptr && PageHost::pageOk(reference);
                    request.references.push_back(reference);
                }
                if (run.ok)
                    run.requests.push_back(std::move(request));
                run.screenshot.clear();
                run.references.clear();
                run.write.clear();
            }
        }
    }
}

// The status that a run exits with if it passes its timeout.
constexpr int kTimedOut = 124;

// Settles the run's pages and answers what is asked of them. Returns the status that the run exits with: 0, or 1 if a
// page fails or cannot be answered, or kTimedOut.
int finish(PageHost& host, Run& run, Renderers& renderers) {
    if (run.ok) {
        PageHost::Settled settled = host.settle(run.timeout);
        // A page that still waits once it has rendered hears so, and settles again.
        if (settled == PageHost::Settled::Idle && host.signalRendered(run.viewport))
            settled = host.settle(run.timeout);
        if (settled == PageHost::Settled::TimedOut) {
            fprintf(stderr, "the pages did not settle in %g ms\n", run.timeout);
            // What the pages reached is still answered, because a page that never settles has no other record of it.
            host.stopWaiting();
            for (const Request& request : run.requests)
                answer(host, request, run.viewport, run.pixelRatio, run.renderer, renderers);
            return kTimedOut;
        }
        run.ok = settled == PageHost::Settled::Idle;
        // A page's load fails once its failure arrives.
        for (const Request& request : run.requests) {
            run.ok = run.ok && PageHost::pageOk(request.page);
            for (const Page* reference : request.references)
                run.ok = run.ok && PageHost::pageOk(reference);
        }
    }
    if (run.verify)
        host.verify(run.viewport);
    for (const Request& request : run.requests) {
        if (run.ok)
            run.ok = answer(host, request, run.viewport, run.pixelRatio, run.renderer, renderers);
    }
    return run.ok ? 0 : 1;
}

// Serves runs read from stdin until stdin ends, each in a host reset to the state that a new process would start in. A
// request is a case in the form that the harnesses read (tools/harness/harness.h), whose bytes are the run's arguments,
// one per line: the pages, and what is asked of them. The settings from the process's own arguments apply to every run.
// The answer to each run, on stdout, is a line `<status> <stdout bytes> <stderr bytes>`, followed by what the run wrote
// to each stream. The status is what the process would have exited with.
int serve(PageHost& host, const Run& settings, Renderers& renderers) {
    HarnessCapture* capture = harnessCaptureOpen();
    if (capture == nullptr)
        return 1;
    HarnessCase request = {};
    while (harnessReadCase(&request)) {
        Run run;
        run.verify = settings.verify;
        run.viewport = settings.viewport;
        run.pixelRatio = settings.pixelRatio;
        run.renderer = settings.renderer;
        run.timeout = settings.timeout;
        std::vector<std::string> args;
        PrStringView input = textView(&request.input);
        for (size_t start = 0, i = 0; i <= input.size; ++i) {
            if (i == input.size || input.data[i] == '\n') {
                if (i > start)
                    args.emplace_back(input.data + start, i - start);
                start = i + 1;
            }
        }
        harnessCaptureBegin(capture);
        host.reset();
        host.setViewport(run.viewport);
        parse(host, args, run);
        int status = run.ok ? finish(host, run, renderers) : 1;
        std::string written;
        std::string failed;
        harnessCaptureEnd(capture, &written, &failed);
        harnessCaptureAnswer(capture, status, written, failed);
    }
    harnessCaseReset(&request);
    harnessCaptureClose(capture);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    prCrashHandler();
    harnessStreamsBinary();
    if (argc < 2) {
        fprintf(stderr, "usage: pr_shell [--network] [--resolve <host> <address>]... [--ca <pem>]... [--cache <directory>] "
            "[--prelude <script>] [--font <file>] [--capture-wait <script>] [--virtual-time] [--verify] [--no-scripts] [--dark] "
            "[--viewport WxH] [--pixel-ratio <target pixels per px>] [--renderer <name>] "
            "[--timeout <ms>] [--restyle-threads <n>] [--shaper <name>] [--hinting none|light|full] [--media <name>] [--serve] "
            "<script | [--screenshot <png>] [--reference <page>]... [--write <stem>] "
            "--html <url | page.html | capture.json>>...\n");
        return 2;
    }
    std::vector<std::string> args(argv + 1, argv + argc);
    PrTextShaperBackend chosen = PrTextShaperBackend_Default;
    PrFontHinting hinting = PrFontHinting_None;
    PrMediaCodecBackend media = PrMediaCodecBackend_Builtin;
    for (size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == "--shaper") {
            chosen = prTextShaperBackendFromName(args[i + 1]);
            if (!prTextShaperBackendIsBuilt(chosen)) {
                fprintf(stderr, "--shaper names no shaper that the build has: %s\n", args[i + 1].c_str());
                return 2;
            }
        } else if (args[i] == "--media") {
            media = prMediaCodecBackendFromName(args[i + 1]);
            if (!prMediaCodecBackendIsBuilt(media)) {
                fprintf(stderr, "--media names no media codec that the build has: %s\n", args[i + 1].c_str());
                return 2;
            }
        } else if (args[i] == "--hinting") {
            hinting = prFontHintingFromName(args[i + 1]);
            if (hinting == PrFontHinting_Count) {
                fprintf(stderr, "--hinting takes none, light or full\n");
                return 2;
            }
        }
    }
    std::unique_ptr<PrTextShaper, decltype(&prTextShaperDestroy)> shaper(prTextShaperCreate(chosen),
        prTextShaperDestroy);
    std::unique_ptr<PrMediaCodec, decltype(&prMediaCodecDestroy)> mediaCodec(prMediaCodecCreate(media),
        prMediaCodecDestroy);
    PageHost host(shaper.get(), mediaCodec.get());
    // The hinter lives as long as the process, because the fonts that name it are kept.
    host.setHinting(hinting, hinting != PrFontHinting_None ? prFontHinterCreate(PrFontHinterBackend_Default) : nullptr);
    Run run;
    parse(host, args, run);
    Renderers renderers;
    // Glyphs are drawn at the phases that the fragment backend's atlas holds them at, so that runs that a page splits
    // differently from its reference draw alike, as reftests expect.
    PrDrawCpuOptions cpuOptions = prDrawCpuOptionsDefault();
    cpuOptions.glyphPhases = true;
    renderers.cpu = prDrawCpuCreate(&cpuOptions);
    int status = !run.ok ? 1 : run.serve ? serve(host, run, renderers) : finish(host, run, renderers);
    renderers.reset();
    return status;
}
