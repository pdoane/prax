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
// --print, before the next page, renders the page and its references printed, as WPT's print reftests capture them
// (web-platform-tests docs, "Print Reftests"): each page of the printed document is rendered as its own image, of its
// page box's extent. A screenshot then writes the first page at its path and each later page n with `-page<n>` before
// the extension, and --write names the renders of a later page the same way, as in `<stem>-page2-ref.png`. --pages
// <ranges>, before the next page or reference, keeps only the pages of that document that the ranges name, numbered
// from 1, in the form of WPT's reftest-pages meta, as in `-2,4,6-`. The comparison of a printed page with a reference
// is a line `REFTEST <json>` that gives both documents' numbers of pages ("pageCounts"), whether the reference draws
// anything on any page ("blank"), and, if the numbers match, the figures of each pair of pages in order ("pages"). Two
// pages of different extents are compared over the extent that they share, from their top left.
//
// --pdf and --svg write the next page as a document of PDF or SVG (pr/draw_writer.h) once every page has settled: the
// page printed for print media, as the pages of the page boxes that its @page rules give. --snapshot, before the next
// page, writes it instead as it renders in the viewport, as one page of the viewport's size. A PDF is tagged with the
// document's structure (pr/html_structure.h), and its streams are compressed. An SVG of several pages is a file for
// each page (PageHost::documentFiles).
//
// --page-size WxH and --page-margin <px> set the page box that a printed document starts from before its @page rules
// apply, as a printer's paper and margins give it, in CSS pixels: a US letter page with no margins
// (prLayoutPageBoxDefault) unless they are given.
//
// --html also opens a page that tools/pages/pages.py captured, by its JSON file. The page is the HTML file beside the
// JSON, and loads at the URL that it was captured from. The resources that the capture saved are answered from their
// files, and those that it left out fail.
//
// --no-scripts loads the pages that follow without running their own scripts. --dark loads them for a user who prefers
// a dark color scheme.
//
// --shaper shapes the pages' text with the shaper of lib/text that it names, the default shaper otherwise, --hinting
// hints their fonts as the hinting that it names with the default hinter, none by default, and --media reads their
// media resources with the codec of lib/media that it names, the built-in codec otherwise, which decodes no frames.
// --audio plays their media elements' audio on the output of lib/audio that it names, and on none otherwise, and
// --audio-write writes what a memory output rendered to the WAVE file that it names when the run ends. Each sets the
// process's backend, so a served run (--serve) uses the backends that the process started with. A memory output in a
// process started with --virtual-time renders in step with the virtual clock (PageHost::paceAudio), so what it writes
// depends on nothing but the pages.
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
// --timings reports on stderr how long each phase of a run took: loading the pages until they settle, and, for each
// document written, its pagination, the paint of its pages, its structure, the writer, and the file's write.
//
// --verify checks each page's presentation against a full recomputation (prCssPresentationVerify) after every rendering
// update and once the pages settle, and writes each page's first failure to stderr as "<page>: presentation verify:
// ...". It checks the layout of each printed document's pages (prLayoutTreeVerify), which includes that no line crosses
// a page's edge, and writes a failure as "<page>: print verify: ...". A failure does not change the exit status, so a
// runner reads the pages' own results as it would without --verify.

#include "pr/assert.h"
#include "pr/audio_memory.h"
#include "pr/audio_mixer.h"
#include "pr/compress.h"
#include "pr/draw_cpu.h"
#include "pr/draw_renderer.h"
#include "pr/draw_structure.h"
#include "pr/draw_writer.h"
#include "pr/file.h"
#include "pr/float.h"
#include "pr/font_hinter.h"
#include "pr/image.h"
#include "pr/layout_page.h"
#include "pr/media.h"
#include "pr/string_ops.h"
#include "pr/text.h"
#include "pr/time.h"

#include "tools/harness/compare.h"
#include "tools/harness/harness.h"
#include "tools/harness/harness_png.h"
#include "tools/harness/harness_streams.h"
#include "tools/shell/device_render.h"
#include "tools/shell/page_host.h"

#include <algorithm>
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
using page_host::Media;
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

// Renders `list`, in target pixels, into `out`, which covers `extent` CSS pixels at `pixelRatio`, rounded up. Returns
// false, saying why on stderr with the page's name `file`, if the renderer cannot render.
bool renderList(const PrDrawList* list, PrVec2 extent, float pixelRatio, PrDrawRendererBackend backend,
        Renderers& renderers, const char* file, Render* out) {
    out->width = uint32_t(prF64Ceil(double(extent.x) * double(pixelRatio)));
    out->height = uint32_t(prF64Ceil(double(extent.y) * double(pixelRatio)));
    out->rgba.assign(size_t(out->width) * out->height * 4, 0);
    PrPixels into{out->rgba.data(), size_t(out->width) * 4, {out->width, out->height}};
    if (backend != PrDrawRendererBackend_Cpu) {
        bool ok = renderOnDevice(renderers, backend, list, into.extent, out->rgba.data());
        if (!ok)
            fprintf(stderr, "%s: the %s renderer could not render\n", file, prDrawRendererBackendName(backend).data);
        return ok;
    }
    return prDrawRendererRenderPixels(renderers.cpu, list, into, PrPixelFormat_Rgba8) == PrStatus_Ok;
}

// Returns true if the page has finished, as a capture requires. Says on stderr if it has not.
bool captureReady(Page* page) {
    if (PageHost::finished(page))
        return true;
    fprintf(stderr, "%s: reftest-wait was not removed\n", PageHost::pageName(page).c_str());
    return false;
}

// Renders the page in its current state, over its canvas, as WPT's reftests capture it. Returns false, saying why on
// stderr, if the page has not finished or has no rendering.
bool renderPage(PageHost& host, Page* page, PrVec2 viewport, float pixelRatio, PrDrawRendererBackend backend,
        Renderers& renderers, Render* out) {
    if (!captureReady(page))
        return false;
    const std::string name = PageHost::pageName(page);
    PrDrawList* list = prDrawListCreate();
    // The page paints in CSS pixels, which the transform maps to target pixels.
    prDrawListPushTransformAffine2(list, prAffine2Scale(PrVec2{pixelRatio, pixelRatio}));
    PrAppDisplay display = prAppDisplayDefault();
    display.pixelRatio = pixelRatio;
    bool ok = host.paint(page, page_host::Media_Screen, viewport, display, host.userPreferences(), list);
    prDrawListPopTransform(list);
    if (!ok)
        fprintf(stderr, "%s: no rendering to capture\n", name.c_str());
    ok = ok && renderList(list, viewport, pixelRatio, backend, renderers, name.c_str(), out);
    prDrawListRelease(list);
    return ok;
}

// A range of the pages of a printed document, numbered from 1, both ends included. An end of 0 is open: the range
// starts at the first page or runs to the last.
struct PageRange {
    uint32_t first;
    uint32_t last;
};

// Parses `text` as the content of WPT's reftest-pages meta: ranges separated by commas, each a page or two pages,
// either of which may be absent, around a hyphen. Returns false, saying why on stderr, if it does not parse.
bool parsePageRanges(const std::string& text, std::vector<PageRange>* out) {
    // Reads a page's number at `at`, or 0 for none, and moves past it and the spaces around it.
    auto number = [&text](size_t& at, uint32_t* page) {
        while (at < text.size() && text[at] == ' ')
            ++at;
        uint64_t value = 0;
        size_t start = at;
        while (at < text.size() && text[at] >= '0' && text[at] <= '9' && value <= UINT32_MAX)
            value = value * 10 + uint64_t(text[at++] - '0');
        while (at < text.size() && text[at] == ' ')
            ++at;
        *page = uint32_t(value);
        return at == start || (value != 0 && value <= UINT32_MAX);
    };
    size_t at = 0;
    for (;;) {
        PageRange range{};
        bool ok = number(at, &range.first);
        if (ok && at < text.size() && text[at] == '-') {
            ++at;
            ok = number(at, &range.last);
        } else {
            ok = ok && range.first != 0;
            range.last = range.first;
        }
        if (!ok || (at < text.size() && text[at] != ',')) {
            fprintf(stderr, "--pages takes page ranges, as -2,4,6-: %s\n", text.c_str());
            return false;
        }
        out->push_back(range);
        if (at == text.size())
            return true;
        ++at;
    }
}

// Returns true if the page numbered `number` of `count` is in one of `ranges`, or if there are no ranges.
bool pageSelected(const std::vector<PageRange>& ranges, uint32_t number, uint32_t count) {
    if (ranges.empty())
        return true;
    for (const PageRange& range : ranges) {
        uint32_t first = range.first != 0 ? range.first : 1;
        uint32_t last = range.last != 0 ? range.last : count;
        if (first <= number && number <= last)
            return true;
    }
    return false;
}

// Prints the page in its current state from the page box `initial`, and renders each of its pages that `ranges` select,
// as WPT's print reftests capture them, appending one render for each. Returns false, saying why on stderr, if the page
// has not finished or has no rendering.
bool renderPrinted(PageHost& host, Page* page, const PrLayoutPageBox& initial, const std::vector<PageRange>& ranges,
        float pixelRatio, PrDrawRendererBackend backend, Renderers& renderers, std::vector<Render>* out) {
    if (!captureReady(page))
        return false;
    const std::string name = PageHost::pageName(page);
    std::vector<PageHost::PrintedPage> pages;
    // The pages lay out for the pixel ratio that they render at, as a screenshot does.
    PrAppDisplay display = prAppDisplayDefault();
    display.pixelRatio = pixelRatio;
    bool ok = host.print(page, initial, display, host.userPreferences(), pages);
    if (!ok)
        fprintf(stderr, "%s: no rendering to capture\n", name.c_str());
    for (size_t i = 0; ok && i < pages.size(); ++i) {
        if (!pageSelected(ranges, uint32_t(i + 1), uint32_t(pages.size())))
            continue;
        PrDrawList* list = prDrawListCreate();
        prDrawListPushTransformAffine2(list, prAffine2Scale(PrVec2{pixelRatio, pixelRatio}));
        prDrawListEmbed(list, pages[i].list, PrDVec2{0, 0});
        prDrawListPopTransform(list);
        out->emplace_back();
        ok = renderList(list, pages[i].extent, pixelRatio, backend, renderers, name.c_str(), &out->back());
        prDrawListRelease(list);
    }
    for (const PageHost::PrintedPage& printed : pages)
        prDrawListRelease(printed.list);
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
// with, the stem of the renders to write (--write), and the documents to write.
struct Request {
    Page* page;
    std::string screenshot;         // empty for none
    std::vector<Page*> references;
    std::string write;              // empty for none
    std::string pdf;                // empty for none
    std::string svg;                // empty for none
    Media media;
    bool timings;                   // true if the run reports how long each phase of the document's writing took

    // Whether the page and its references are rendered printed (--print), and the pages of each that are rendered then,
    // or no ranges for all of them.
    bool print;
    std::vector<PageRange> pages;
    std::vector<std::vector<PageRange>> referencePages;     // by reference
};

// Writes the page as a document in `media` by the writer `backend`, to `path` (PageHost::writeDocument). A printed
// document starts from the page box `pageBox`. Reports how long each phase took if asked. Returns false, saying why on
// stderr, if it cannot.
bool writeDocument(PageHost& host, Page* page, PrVec2 viewport, const PrLayoutPageBox& pageBox, Media media,
        PrDrawWriterBackend backend, const std::string& path, bool timings) {
    PageHost::DocumentTimings phases{};
    if (!host.writeDocument(page, media, pageBox, viewport, backend, path, &phases))
        return false;
    if (timings) {
        fprintf(stderr, "%s: %u pages, %zu bytes. paginate %.0f ms, paint %.0f ms, structure %.0f ms, other %.0f ms, "
            "write %.0f ms, file %.0f ms\n", prDrawWriterBackendName(backend).data, phases.pages, phases.bytes,
            phases.print.paginate, phases.print.paint, phases.print.structure, phases.other, phases.write, phases.file);
    }
    return true;
}

// Returns the path of the render of the page numbered `number` of a printed document, from 1: `<stem><suffix>` for the
// first page, and `<stem>-page<number><suffix>` for a later one.
std::string pageFile(const std::string& stem, size_t number, const std::string& suffix) {
    return number == 1 ? stem + suffix : stem + "-page" + std::to_string(number) + suffix;
}

// Returns the part of `render` from its top left that is `width` by `height` pixels, which must lie inside it.
Render cropped(const Render& render, uint32_t width, uint32_t height) {
    PR_ASSERT(width <= render.width && height <= render.height);
    Render out{width, height, std::vector<uint8_t>(size_t(width) * height * 4)};
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* row = render.rgba.data() + size_t(y) * render.width * 4;
        std::copy(row, row + size_t(width) * 4, out.rgba.data() + size_t(y) * width * 4);
    }
    return out;
}

// Renders the request's page printed, writes its screenshot, and compares each of its pages with the pages of each
// reference in order. Returns false, saying why on stderr, if a page cannot be rendered or a file written.
bool answerPrinted(PageHost& host, const Request& request, const PrLayoutPageBox& pageBox, float pixelRatio,
        PrDrawRendererBackend backend, Renderers& renderers) {
    std::vector<Render> rendered;
    if (!renderPrinted(host, request.page, pageBox, request.pages, pixelRatio, backend, renderers, &rendered))
        return false;
    std::string screenshot = request.screenshot;
    if (std::filesystem::path(screenshot).extension() == ".png")
        screenshot.resize(screenshot.size() - 4);
    for (size_t i = 0; i < rendered.size(); ++i) {
        if (!request.screenshot.empty() && !writePng(rendered[i], request.page, pageFile(screenshot, i + 1, ".png")))
            return false;
        if (!request.write.empty() && !writePng(rendered[i], request.page, pageFile(request.write, i + 1, ".png")))
            return false;
    }
    for (size_t r = 0; r < request.references.size(); ++r) {
        Page* reference = request.references[r];
        std::vector<Render> expected;
        if (!renderPrinted(host, reference, pageBox, request.referencePages[r], pixelRatio, backend, renderers,
                &expected))
            return false;
        std::string number = r == 0 ? "" : std::to_string(r + 1);
        bool blank = true;
        std::string figures;
        for (size_t i = 0; i < expected.size(); ++i) {
            const Render& want = expected[i];
            blank = blank && !imageDrawsAnything(want.rgba.data(), want.width, want.height, size_t(want.width) * 4);
            if (!request.write.empty() && !writePng(want, reference, pageFile(request.write, i + 1, "-ref" + number + ".png")))
                return false;
            if (expected.size() != rendered.size())
                continue;
            // wptrunner measures two images of different extents over the extent that they share.
            uint32_t width = rendered[i].width < want.width ? rendered[i].width : want.width;
            uint32_t height = rendered[i].height < want.height ? rendered[i].height : want.height;
            Render got = cropped(rendered[i], width, height);
            Render wanted = cropped(want, width, height);
            size_t stride = size_t(width) * 4;
            if (!request.write.empty()) {
                Render diff{width, height, std::vector<uint8_t>(got.rgba.size())};
                compareImages(wanted.rgba.data(), got.rgba.data(), width, height, stride, ImageComparison{},
                    diff.rgba.data());
                if (!writePng(diff, request.page, pageFile(request.write, i + 1, "-diff" + number + ".png")))
                    return false;
            }
            PixelDifference d = countDifferences(wanted.rgba.data(), got.rgba.data(), width, height, stride);
            figures += (i == 0 ? "" : ",") + std::string("{\"maxDifference\":") + std::to_string(d.maxDifference) +
                ",\"differingPixels\":" + std::to_string(d.differingPixels) + "}";
        }
        printf("REFTEST {\"pageCounts\":[%zu,%zu],\"blank\":%s,\"pages\":[%s]}\n", rendered.size(), expected.size(),
            blank ? "true" : "false", figures.c_str());
    }
    return true;
}

// Writes the request's documents, renders its page and writes its screenshot, and compares it with each of its
// references. Returns false, saying why on stderr, if a page cannot be rendered or a file written.
bool answer(PageHost& host, const Request& request, PrVec2 viewport, const PrLayoutPageBox& pageBox, float pixelRatio,
        PrDrawRendererBackend backend, Renderers& renderers) {
    if (!request.pdf.empty() &&
            !writeDocument(host, request.page, viewport, pageBox, request.media, PrDrawWriterBackend_Pdf, request.pdf,
                request.timings))
        return false;
    if (!request.svg.empty() &&
            !writeDocument(host, request.page, viewport, pageBox, request.media, PrDrawWriterBackend_Svg, request.svg,
                request.timings))
        return false;
    if (request.screenshot.empty() && request.references.empty() && request.write.empty())
        return true;
    if (request.print)
        return answerPrinted(host, request, pageBox, pixelRatio, backend, renderers);
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
    PrLayoutPageBox pageBox = prLayoutPageBoxDefault();         // what a printed document starts from
    std::string screenshot;
    std::vector<std::string> references;
    std::string write;
    std::string pdf;
    std::string svg;
    Media media = page_host::Media_Print;
    bool print = false;             // true if the next page is rendered printed (--print)

    // The pages that --pages selects of the next document named, and of each reference named since the last page, or no
    // ranges for all of them.
    std::vector<PageRange> pages;
    std::vector<std::vector<PageRange>> referencePages;     // by reference

    bool timings = false;           // true if the run reports how long each phase took (--timings)
    double started = 0;             // when the run began, in milliseconds of the monotonic clock
    std::vector<Request> requests;
};

// Has the host fetch over sockets, once. Returns false, saying why on stderr, if it cannot.
bool network(PageHost& host, Run& run) {
    if (run.network)
        return true;
    run.network = host.useNetwork();
    return run.network;
}

// The format that a run plays audio in.
constexpr uint32_t kAudioSampleRate = 48000;
constexpr uint32_t kAudioChannels = 2;

// Writes what a stopped memory output rendered to `path`, as a WAVE file of 32-bit floats. Returns false if the file is
// not written.
bool writeAudio(const PrAudioOutput* output, const std::string& path) {
    uint64_t frames = prAudioMemoryFrameCount(output);
    PrAudioFormat format = prAudioOutputFormat(output);
    uint32_t dataSize = uint32_t(frames * format.channels * sizeof(float));
    std::string file;
    auto text = [&file](const char* value) { file.append(value, 4); };
    auto word = [&file](uint32_t value, int bytes) {
        for (int i = 0; i < bytes; ++i)
            file.push_back(char((value >> (8 * i)) & 0xff));
    };
    text("RIFF");
    word(36 + dataSize, 4);
    text("WAVE");
    text("fmt ");
    word(16, 4);
    word(3, 2); // WAVE_FORMAT_IEEE_FLOAT
    word(format.channels, 2);
    word(format.sampleRate, 4);
    word(format.sampleRate * format.channels * uint32_t(sizeof(float)), 4);
    word(format.channels * uint32_t(sizeof(float)), 2);
    word(32, 2);
    text("data");
    word(dataSize, 4);
    if (dataSize != 0)
        file.append(reinterpret_cast<const char*>(prAudioMemorySamples(output)), dataSize);
    return prFileWrite(path, prStringBytes(file));
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
            option == "--audio" || option == "--audio-write" || option == "--hinting" || option == "--ca" ||
            option == "--capture-wait" || option == "--cache" || option == "--pdf" || option == "--svg" ||
            option == "--pages" || option == "--page-size" || option == "--page-margin";
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
        } else if (option == "--snapshot") {
            run.media = page_host::Media_Screen;
        } else if (option == "--timings") {
            run.timings = true;
        } else if (option == "--print") {
            run.print = true;
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
            } else if (option == "--pdf") {
                run.pdf = value;
            } else if (option == "--svg") {
                run.svg = value;
            } else if (option == "--reference") {
                run.references.push_back(value);
                run.referencePages.push_back(std::move(run.pages));
                run.pages.clear();
            } else if (option == "--pages") {
                run.pages.clear();
                run.ok = parsePageRanges(value, &run.pages);
            } else if (option == "--page-size") {
                float width = 0;
                float height = 0;
                run.ok = sscanf(value.c_str(), "%fx%f", &width, &height) == 2 && width > 0 && height > 0;
                if (!run.ok)
                    fprintf(stderr, "--page-size takes WxH in CSS pixels, as 480x288\n");
                run.pageBox.extent = PrLayoutVec{prLayoutUnitFromPx(width), prLayoutUnitFromPx(height)};
            } else if (option == "--page-margin") {
                char* end = nullptr;
                float margin = strtof(value.c_str(), &end);
                run.ok = end != value.c_str() && *end == '\0' && margin >= 0;
                if (!run.ok)
                    fprintf(stderr, "--page-margin takes CSS pixels, as 48\n");
                for (PrLayoutUnit& side : run.pageBox.margin)
                    side = prLayoutUnitFromPx(margin);
            } else if (option == "--shaper" || option == "--hinting" || option == "--media" || option == "--audio" ||
                    option == "--audio-write") {
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
                page_host::Capture capture = page_host::Capture_None;
                if (!run.screenshot.empty() || !run.references.empty() || !run.write.empty() || !run.pdf.empty() ||
                        !run.svg.empty() || run.print)
                    capture = page_host::Capture_Wait;
                Request request{
                    .page = run.ok ? host.load(page, capture) : nullptr,
                    .screenshot = run.screenshot,
                    .write = run.write,
                    .pdf = run.pdf,
                    .svg = run.svg,
                    .media = run.media,
                    .timings = run.timings,
                    .print = run.print,
                    .pages = run.pages,
                    .referencePages = run.referencePages,
                };
                run.ok = request.page != nullptr && PageHost::pageOk(request.page);
                for (size_t r = 0; run.ok && r < run.references.size(); ++r) {
                    Page* reference = host.load(run.references[r], page_host::Capture_Wait);
                    run.ok = reference != nullptr && PageHost::pageOk(reference);
                    request.references.push_back(reference);
                }
                if (run.ok)
                    run.requests.push_back(std::move(request));
                run.screenshot.clear();
                run.references.clear();
                run.write.clear();
                run.pdf.clear();
                run.svg.clear();
                run.media = page_host::Media_Print;
                run.print = false;
                run.pages.clear();
                run.referencePages.clear();
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
        page_host::Settled settled = host.settle(run.timeout);
        // A page that still waits once it has rendered hears so, and settles again.
        if (settled == page_host::Settled_Idle && host.signalRendered(run.viewport))
            settled = host.settle(run.timeout);
        if (settled == page_host::Settled_TimedOut) {
            fprintf(stderr, "the pages did not settle in %g ms\n", run.timeout);
            // What the pages reached is still answered, because a page that never settles has no other record of it.
            host.stopWaiting();
            for (const Request& request : run.requests)
                answer(host, request, run.viewport, run.pageBox, run.pixelRatio, run.renderer, renderers);
            return kTimedOut;
        }
        run.ok = settled == page_host::Settled_Idle;
        if (run.timings)
            fprintf(stderr, "load: %.0f ms\n", prTimeMonotonicMilliseconds() - run.started);
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
            run.ok = answer(host, request, run.viewport, run.pageBox, run.pixelRatio, run.renderer, renderers);
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
        run.pageBox = settings.pageBox;
        run.pixelRatio = settings.pixelRatio;
        run.renderer = settings.renderer;
        run.timeout = settings.timeout;
        std::vector<std::string> args;
        PrStringView input = prTextBufferView(&request.input);
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
            "[--viewport WxH] [--pixel-ratio <target pixels per px>] [--renderer <name>] [--page-size WxH] "
            "[--page-margin <px>] [--timeout <ms>] [--restyle-threads <n>] [--shaper <name>] [--hinting none|light|full] "
            "[--media <name>] [--serve] [--timings] <script | [--print] [--screenshot <png>] [[--pages <ranges>] "
            "--reference <page>]... [--write <stem>] [--pdf <pdf>] [--svg <svg>] [--pages <ranges>] "
            "--html <url | page.html | capture.json>>...\n");
        return 2;
    }
    std::vector<std::string> args(argv + 1, argv + argc);
    PrTextShaperBackend chosen = PrTextShaperBackend_Default;
    PrFontHinting hinting = PrFontHinting_None;
    PrMediaCodecBackend media = PrMediaCodecBackend_Builtin;
    PrAudioOutputBackend audio = PrAudioOutputBackend_Count;
    std::string audioPath;
    bool virtualTime = std::find(args.begin(), args.end(), "--virtual-time") != args.end();
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
        } else if (args[i] == "--audio") {
            audio = prAudioOutputBackendFromName(args[i + 1]);
            if (!prAudioOutputBackendIsBuilt(audio)) {
                fprintf(stderr, "--audio names no audio output that the build has: %s\n", args[i + 1].c_str());
                return 2;
            }
        } else if (args[i] == "--audio-write") {
            audioPath = args[i + 1];
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
    PrAudioOutput* audioOutput = nullptr;
    PrAudioFormat audioFormat{kAudioSampleRate, kAudioChannels};
    bool pacedAudio = virtualTime && audio == PrAudioOutputBackend_Memory;
    if (pacedAudio)
        audioOutput = prAudioMemoryCreate(audioFormat, PrAudioMemoryPace_Host);
    else if (audio != PrAudioOutputBackend_Count)
        audioOutput = prAudioOutputCreate(audio, audioFormat);
    std::unique_ptr<PrAudioOutput, decltype(&prAudioOutputDestroy)> audioOutputOwner(audioOutput, prAudioOutputDestroy);
    PrAudioMixer* mixer = audioOutput != nullptr ? prAudioMixerCreate(audioOutput) : nullptr;
    std::unique_ptr<PrAudioMixer, decltype(&prAudioMixerDestroy)> mixerOwner(mixer, prAudioMixerDestroy);
    // The pages' canvases are textures on the device that the cpu renderer draws on, which outlives the pages.
    PrGpuDevice* memory = nullptr;
    prGpuDeviceCreate(PrGpuBackend_Memory, nullptr, &memory);
    std::unique_ptr<PrGpuDevice, decltype(&prGpuDeviceDestroy)> memoryOwner(memory, prGpuDeviceDestroy);
    PageHost host(shaper.get(), mediaCodec.get(), mixer);
    host.setDevice(memory);
    if (pacedAudio)
        host.paceAudio(audioOutput);
    // The hinter lives as long as the process, because the fonts that name it are kept.
    host.setHinting(hinting, hinting != PrFontHinting_None ? prFontHinterCreate(PrFontHinterBackend_Default) : nullptr);
    Run run;
    run.started = prTimeMonotonicMilliseconds();
    parse(host, args, run);
    Renderers renderers;
    // Glyphs are drawn at the phases that the fragment backend's atlas holds them at, so that runs that a page splits
    // differently from its reference draw alike, as reftests expect.
    PrDrawCpuOptions cpuOptions = prDrawCpuOptionsDefault();
    cpuOptions.glyphPhases = true;
    cpuOptions.device = memory;
    renderers.cpu = prDrawCpuCreate(&cpuOptions);
    int status = !run.ok ? 1 : run.serve ? serve(host, run, renderers) : finish(host, run, renderers);
    renderers.reset();
    if (!audioPath.empty() && audio == PrAudioOutputBackend_Memory && audioOutput != nullptr) {
        // The output stops here because the pages that play on it live until the host goes.
        prAudioOutputStop(audioOutput);
        if (!writeAudio(audioOutput, audioPath)) {
            fprintf(stderr, "cannot write %s\n", audioPath.c_str());
            status = status != 0 ? status : 1;
        }
    }
    return status;
}
