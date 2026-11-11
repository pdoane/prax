// A viewer for what the engine draws: collections of documents, each shown in one pane or compared across two. A pane
// shows Prax, the engine, in a configuration of its renderers: a renderer, a text shaper and a font hinting. Prax
// renders a page for a media: on the screen, or printed, as the pages that a printed document holds. Each collection
// chooses the media that its pages open in. Otherwise a pane shows one of the document's references, by the document's
// type: the system's browser, another player such as lottie-web, Skottie or rlottie, an image such as a suite's
// reference, or a printed document. A printed document is a PDF that another engine printed, a PDF or SVG that Prax
// prints of the page when the document opens, or a PDF that the browser prints of the page for print media. The
// platform rasterizes a PDF where it can, and the system's browser shows it elsewhere. The engine shows an SVG as the
// document of a page (documents.h). The panes are alike, so two of Prax's configurations, or two players, compare as
// Prax and a reference do. A document's first reference is the one that its suite compares the engine with, which the
// second pane opens on.
//
// Each collection declares the viewport that its documents are written for, and each pane lays its document out in that
// viewport, in either view. A viewport smaller than its pane is centered in it, the pane shows as much of a larger one
// as fits, and the document scrolls through its own overflow in both axes. A document that has an extent of its own and
// no viewport, an image or a Lottie animation, is fitted to the viewport and centered. A printed page or a PDF shows
// its pages as the engine's paged view does: down the viewport with a gap between them, centered across it. The zoom is
// page zoom: the viewport is drawn at the zoom, and its target pixels per CSS pixel are the display's times the zoom.
// It starts at 100%, or at `--zoom <percent>`, and changes only by the zoom commands, and Fit width zooms the first
// pane's document so that what its viewport scrolls through is as wide as the pane. A document opens at its top left,
// and a change of media opens it there again. In comparison, the pane that was last scrolled leads, and the other
// pane's document, or the browser's by scrollTo, scrolls to the same offset.
//
// The controls beside the documents choose the collection, the document, each pane and the view. Keys: the arrows move
// through the tree of documents, opening and closing its directories. [ and ] turn the document. 1 through 9 choose a
// renderer's family for the first pane, returning to the configuration that it last showed. V turns through the
// family's configurations. T turns through the shapers, which reloads the document. P switches the first pane's page
// between the screen and print, which reloads it too. X swaps the panes. C switches between the single view and the
// comparison view. S, F and D show a comparison side by side, flipping between the two panes, or as their difference.
// Command = and Command - zoom in and out through the standard steps, and Command 0 shows the actual size. Every
// document renders again at each of the display's refreshes, so that its figures are steady. B renders back to back
// instead, as fast as the device allows, to measure; the figures say which. H presents in half floats, so that colors
// past SDR white and past sRGB's gamut show on a display that can show them; the panes then render in half floats, tone
// mapped to the display's headroom. Space plays and pauses the document's clock. Q or Escape quits. The pointer and the
// wheel over a document go to the document, and after a click on a page, so do keys other than Command chords, until a
// click elsewhere. A wheel that a page cannot take at its scroll limit stays with the page.
//
// `viewer --render <directory>` opens no window. It renders every document of the collection with every renderer and
// writes each render, in a viewport of the collection's size, as a PNG named for the document, its media if it is a
// page, and the renderer, as in 5-flyer-print-cpu.png, creating the directory if needed, and prints each of a
// document's printed references in its format, named for the document and the reference, as in 1-campaign-prax-pdf.pdf.
// The collection is the first unless `--collection` names another. `--reference <renderer>` also compares every other
// renderer's render with that renderer's render, as the SVG corpora are compared, and writes the difference beside it.
// `--document <which>` narrows either mode to the documents that it names, by number or by a word of the name, and the
// window opens at the first of them. `--pixel-ratio <pixels>` renders at that many target pixels per unit of the
// document.
//
// The window opens on `--collection <word>`, the collection whose name contains the word, with the first pane on
// `--renderer <name>`, or on fragment in its default configuration. The first pane, and the renders, shape text with
// `--shaper <name>`, or with the default shaper, and hint fonts as `--hinting <name>` says, or not at all. They render
// pages for `--media screen` or `--media print`, or for the collection's media. With `--reference <name>`, which names
// a renderer or one of the document's own references for the second pane, by its name or its file name (`prax-svg` for
// "Prax SVG"), the window opens in comparison, at `--zoom <percent>` of the natural size rather than fitted, shown as
// `--look side`, `flip` or `difference`. `--snapshot <path>` writes the window's first frame whose figures are settled
// as a PNG, including the controls, and quits. `--tour <passes>` turns through every document that many times, one per
// frame, and quits. `--rendering back-to-back` opens rendering back to back, as B does, and `--rendering paced` paced
// by the display. `--present hdr` opens presenting in half floats, as H does, and `--present sdr` in 8 bits.
// `--wpt-server <http port> <https port>` serves WPT's reftests from WPT's server (tools/suite/wpt/wpt_serve.py), which
// `./dev.sh viewer` starts, and the WPT collection is listed only with it. `--third-party-server <http port>` loads the
// other pages of third_party from an HTTP server that serves the directory at its root, which `./dev.sh viewer` starts
// too. With PR_VIEWER_PROFILE set, which prints each frame's phases, the tour measures what moving between documents
// costs.

#include "pr/app.h"
#include "pr/assert.h"
#include "pr/compress_builtin.h"
#include "pr/draw.h"
#include "pr/draw_display.h"
#include "pr/draw_fragment.h"
#include "pr/draw_list.h"
#include "pr/image_builtin.h"
#include "pr/vec.h"
#include "pr/webview.h"

#include "tools/harness/compare.h"
#include "tools/harness/draw_renderers.h"
#include "tools/viewer/documents.h"
#include "tools/viewer/panes.h"
#include "tools/viewer/source_html.h"
#include "tools/viewer/ui.h"
#include "tools/viewer/zoom.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace viewer;
using namespace pr;
using namespace harness;

namespace {

// The gap between the comparison's panes, in logical pixels.
constexpr float kGap = 8;

// The window's initial size, in logical pixels.
constexpr float kWindowWidth = 1400;
constexpr float kWindowHeight = 800;

// What the window shows where nothing else is: around the controls and the panes, and in each pane wherever its
// document draws nothing, as between a printed document's pages.
constexpr PrColor kBackdrop{0.12f, 0.12f, 0.13f, 1};

// Returns the swap chain that the viewer presents with, for SDR or for HDR.
PrGpuSwapChainDesc swapChainDesc(bool hdr) {
    return {
        .intent = PrGpuSwapChainIntent(hdr ? PrGpuSwapChainIntent_Hdr : PrGpuSwapChainIntent_Sdr),
        .flags = PrGpuSwapChainFlag_Threaded,
    };
}

// ---------------------------------------------------------------------------------------------------------------------
// Rendering without a window

// Renders the list into the slot and waits for the device, for rendering without a window.
Rendered renderSlotWaiting(PrGpuDevice* device, Slot& slot, const std::vector<RendererConfig>& configs,
        uint32_t config, const PrDrawList* list, PrColor background) {
    PrGpuCommands* commands = nullptr;
    if (!prGpuCommandsBegin(device, nullptr, &commands))
        return Rendered{.status = PrStatus_Lost};
    Rendered r = renderSlot(slot, configs, config, list, background, commands, PaneOutput{});
    r.gpuMs = prGpuCommandsSubmitAndWait(commands) * 1000;
    return r;
}

// Records the source into the list at `width` by `height` logical pixels and at `time`, at `pixelRatio` target pixels
// per logical pixel.
void record(PrDrawList* list, Loaded& loaded, float width, float height, float pixelRatio, double time) {
    prDrawListClear(list);
    prDrawListPushTransformAffine2(list, prAffine2Scale(PrVec2{pixelRatio, pixelRatio}));
    update(loaded, time);
    PrAppDisplay display = prAppDisplayDefault();
    display.pixelRatio = pixelRatio;
    draw(loaded, list, width, height, display, prAppUserPreferencesDefault());
    prDrawListPopTransform(list);
}

// Reads the slot's pixels, waiting for the device, for rendering without a window.
std::vector<uint8_t> readSlot(const Slot& slot) {
    uint32_t width = prGpuTextureExtent(slot.device, slot.texture).x;
    uint32_t height = prGpuTextureExtent(slot.device, slot.texture).y;
    std::vector<uint8_t> pixels(size_t(width) * height * 4);
    PrPixels texels{pixels.data(), size_t(width) * 4, {width, height}};
    prGpuTextureRead(slot.device, slot.texture, PrGpuAccessFlag_ShaderRead, PrIVec2{0, 0}, texels);
    return pixels;
}

// Writes straight RGBA pixels as a PNG.
bool writeRgbaPng(const std::string& path, const std::vector<uint8_t>& rgba, uint32_t width, uint32_t height) {
    const PrPixelView pixels{rgba.data(), size_t(width) * 4, {width, height}};
    PrCompressCodec* compressCodec = prCompressBuiltinCreate();
    PrImageCodec* imageCodec = prImageBuiltinCreate(compressCodec);
    size_t size = 0;
    prImageCodecEncode(imageCodec, "image/png", pixels, nullptr, 0, &size);
    std::vector<uint8_t> png(size);
    prImageCodecEncode(imageCodec, "image/png", pixels, png.data(), png.size(), &size);
    prImageCodecDestroy(imageCodec);
    prCompressCodecDestroy(compressCodec);
    FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr)
        return false;
    bool written = std::fwrite(png.data(), 1, png.size(), file) == png.size();
    return std::fclose(file) == 0 && written;
}

// Writes premultiplied BGRA pixels as a PNG of straight RGBA.
bool writePng(const std::string& path, const std::vector<uint8_t>& pixels, uint32_t width, uint32_t height) {
    std::vector<uint8_t> rgba = pixels;
    straightenBgra(rgba.data(), width, height, size_t(width) * 4);
    return writeRgbaPng(path, rgba, width, height);
}

int indexOf(const std::vector<RendererConfig>& configs, const char* name) {
    for (uint32_t b = 0; b < uint32_t(configs.size()); ++b)
        if (std::strcmp(configs[b].name, name) == 0)
            return int(b);
    return -1;
}

int renderAll(const char* directory, const Collection& documents, const char* which, const char* referenceName,
        TextBackends text, page_host::Media media, double time, float pixelRatio) {
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        std::fprintf(stderr, "cannot create %s: %s\n", directory, error.message().c_str());
        return 1;
    }
    PrGpuDevice* device = nullptr;
    if (prGpuDeviceCreate(PrGpuBackend_Default, nullptr, &device) != PrStatus_Ok) {
        std::fprintf(stderr, "no GPU device\n");
        return 1;
    }
    std::vector<RendererConfig> configs = createConfigs(device);
    int reference = referenceName != nullptr ? indexOf(configs, referenceName) : -1;
    if (referenceName != nullptr && reference < 0) {
        std::fprintf(stderr, "no renderer named %s\n", referenceName);
        destroyConfigs(configs);
        prGpuDeviceDestroy(device);
        return 2;
    }
    Slot slot;
    Slot referenceSlot;
    PrDrawList* list = prDrawListCreate();
    int failures = 0;
    for (uint32_t d = 0; d < uint32_t(documents.documents().size()); ++d) {
        if (!documentNamed(documents, d, which))
            continue;
        const Document& document = documents.documents()[d];
        // A document renders in a viewport of the collection's size.
        PrVec2 size{documents.width, documents.height};
        uint32_t width = uint32_t(std::lround(size.x * pixelRatio));
        uint32_t height = uint32_t(std::lround(size.y * pixelRatio));
        // Each renderer configuration renders a load of its own, presented where its renderer renders.
        auto recordFor = [&](uint32_t config) {
            PrGpuDevice* presenter = prDrawRendererDevice(configs[config].renderer);
            std::shared_ptr<Loaded> loaded = load(document.source, text, media, presenter);
            record(list, *loaded, size.x, size.y, pixelRatio, time);
            return loaded;
        };
        std::string stem = std::string(directory) + "/" + std::to_string(d + 1) + "-" + fileNameOf(document.name) + "-";
        // The renders of a document that the engine prints say which media they are for.
        std::string renders = stem;
        if (prints(document.source))
            renders += media == page_host::Media_Print ? "print-" : "screen-";
        std::vector<uint8_t> expected;
        if (reference >= 0) {
            std::shared_ptr<Loaded> loaded = recordFor(uint32_t(reference));
            if (sizeSlot(device, referenceSlot, width, height) == PrStatus_Ok &&
                    renderSlotWaiting(device, referenceSlot, configs, uint32_t(reference), list, kBackdrop)
                    .status == PrStatus_Ok) {
                expected = readSlot(referenceSlot);
                straightenBgra(expected.data(), width, height, size_t(width) * 4);
            }
        }
        for (uint32_t b = 0; b < uint32_t(configs.size()); ++b) {
            std::shared_ptr<Loaded> loaded = recordFor(b);
            Rendered r{};
            r.status = sizeSlot(device, slot, width, height);
            if (r.status == PrStatus_Ok)
                r = renderSlotWaiting(device, slot, configs, b, list, kBackdrop);
            std::vector<uint8_t> pixels;
            if (r.status == PrStatus_Ok)
                pixels = readSlot(slot);
            std::string path = renders + configs[b].name + ".png";
            bool ok = r.status == PrStatus_Ok && writePng(path, pixels, width, height);
            std::printf("%s %s: %u commands, %.2f ms", ok ? "wrote" : "FAILED", path.c_str(), r.commands, r.ms);
            if (ok && !expected.empty() && expected.size() == pixels.size() && int(b) != reference) {
                std::vector<uint8_t> diff(pixels.size());
                straightenBgra(pixels.data(), width, height, size_t(width) * 4);
                ImageDifference c = compareImages(expected.data(), pixels.data(), width, height,
                    size_t(width) * 4, ImageComparison{}, diff.data());
                std::string diffPath = renders + configs[b].name + "-diff-" + referenceName + ".png";
                ok = writeRgbaPng(diffPath, diff, width, height);
                std::printf(", against %s changed %.3f%% (content %.3f%%), mean error %.3f",
                    referenceName, c.changedRatio * 100, c.contentChangedRatio * 100, c.mae);
            }
            std::printf("\n");
            failures += ok ? 0 : 1;
        }
        // Each printed reference is printed beside the renders, named for the document and the reference, with its
        // format's extension.
        for (const Reference& listed : document.references) {
            if (listed.printed.path.empty())
                continue;
            Reference printed = listed;
            PrStringView extension = prDrawWriterBackendName(listed.printed.format);
            printed.printed.path = stem + fileNameOf(listed.name) + "." + std::string(extension.data, extension.size);
            bool ok = print(printed);
            std::printf("%s %s\n", ok ? "wrote" : "FAILED", printed.printed.path.string().c_str());
            failures += ok ? 0 : 1;
        }
    }
    prDrawListRelease(list);
    releaseSlot(slot);
    releaseSlot(referenceSlot);
    destroyConfigs(configs);
    prGpuDeviceDestroy(device);
    return failures == 0 ? 0 : 1;
}

// ---------------------------------------------------------------------------------------------------------------------
// The app

// Compares renders on its own thread, so that no frame waits for it. Each side's pixels arrive as the device reads them
// back, on the device's thread, tagged with what was rendered. A comparison runs once both sides hold what the viewer
// last asked for. Its result, which a later frame takes, wakes the app.
class Comparer {
public:
    struct Result {
        std::string shown[2];           // what each side's pixels show
        uint32_t width = 0;
        uint32_t height = 0;
        ImageDifference difference{};
        std::vector<uint8_t> diff;      // opaque BGRA
    };

    explicit Comparer(PrApp* app) : app_(app), thread_([this] { run(); }) {}

    ~Comparer() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        wake_.notify_one();
        thread_.join();
    }

    // Requests the comparison of the renders shown as `rendered` and `reference`.
    void want(const std::string& rendered, const std::string& reference) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (wanted_[0] == rendered && wanted_[1] == reference)
                return;
            wanted_[0] = rendered;
            wanted_[1] = reference;
            compared_ = false;
        }
        wake_.notify_one();
    }

    // Delivers side 0's pixels (the render) or side 1's pixels (the reference) as premultiplied BGRA, from any thread.
    void deliver(uint32_t side, const std::string& shown, PrPixelView texels) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            Side& s = sides_[side];
            s.shown = shown;
            s.width = texels.extent.x;
            s.height = texels.extent.y;
            s.pixels.resize(size_t(texels.extent.x) * texels.extent.y * 4);
            for (uint32_t y = 0; y < texels.extent.y; ++y)
                std::copy_n(static_cast<const uint8_t*>(texels.data) + y * texels.stride,
                    size_t(texels.extent.x) * 4, s.pixels.data() + size_t(y) * texels.extent.x * 4);
            compared_ = false;
        }
        wake_.notify_one();
    }

    // Moves the latest result into `out`. Returns true if there was a new one.
    bool take(Result& out) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!ready_)
            return false;
        std::swap(out, result_);
        ready_ = false;
        return true;
    }

private:
    struct Side {
        std::string shown;
        uint32_t width = 0;
        uint32_t height = 0;
        std::vector<uint8_t> pixels;
    };

    // Returns true if both sides hold what was requested and have not been compared.
    bool due() const {
        return !compared_ && !wanted_[0].empty() && sides_[0].shown == wanted_[0] &&
            sides_[1].shown == wanted_[1] && sides_[0].width == sides_[1].width && sides_[0].height == sides_[1].height;
    }

    void run() {
        Side rendered;
        Side reference;
        Result result;
        for (;;) {
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait(lock, [this] { return due() || stopping_; });
                if (stopping_)
                    return;
                rendered = sides_[0];
                reference = sides_[1];
                compared_ = true;
            }
            uint32_t width = rendered.width;
            uint32_t height = rendered.height;
            size_t stride = size_t(width) * 4;
            straightenBgra(reference.pixels.data(), width, height, stride);
            straightenBgra(rendered.pixels.data(), width, height, stride);
            result.shown[0] = rendered.shown;
            result.shown[1] = reference.shown;
            result.width = width;
            result.height = height;
            result.diff.resize(rendered.pixels.size());
            result.difference = compareImages(reference.pixels.data(), rendered.pixels.data(),
                width, height, stride, ImageComparison{}, result.diff.data());
            // The difference is opaque, so its straight RGBA, once swapped, is the BGRA that the texture takes.
            for (size_t i = 0; i < result.diff.size(); i += 4)
                std::swap(result.diff[i], result.diff[i + 2]);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                std::swap(result_, result);
                ready_ = true;
            }
            prAppWake(app_);
        }
    }

    PrApp* app_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::string wanted_[2];
    Side sides_[2];
    Result result_;
    bool compared_ = false;
    bool ready_ = false;
    bool stopping_ = false;
    std::thread thread_;
};

// How far a print of a printed reference has come.
enum {
    PrintState_Pending,
    PrintState_Printed,
    PrintState_Failed,
};
typedef uint8_t PrintState;

// A print of a printed reference, which the printer runs. Its state is written on the printer's thread.
struct PrintJob {
    Reference reference;
    std::atomic<PrintState> state{PrintState_Pending};
};

// Prints the printed references on its own thread, in the order that they were asked for, so that no frame waits for a
// print. A job that no one holds any more is skipped. Each print that ends wakes the app.
class Printer {
public:
    explicit Printer(PrApp* app) : app_(app), thread_([this] { run(); }) {}

    // Waits for the print that is running.
    ~Printer() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        wake_.notify_one();
        thread_.join();
    }

    // Returns a job that prints the reference, queued after the jobs asked for before it.
    std::shared_ptr<PrintJob> ask(const Reference& reference) {
        std::shared_ptr<PrintJob> job = std::make_shared<PrintJob>();
        job->reference = reference;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push_back(job);
        }
        wake_.notify_one();
        return job;
    }

private:
    void run() {
        for (;;) {
            std::shared_ptr<PrintJob> job;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait(lock, [this] { return !queue_.empty() || stopping_; });
                if (stopping_)
                    return;
                job = queue_.front().lock();
                queue_.erase(queue_.begin());
            }
            if (job == nullptr)
                continue;
            job->state = print(job->reference) ? PrintState_Printed : PrintState_Failed;
            prAppWake(app_);
        }
    }

    PrApp* app_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::vector<std::weak_ptr<PrintJob>> queue_;
    bool stopping_ = false;
    std::thread thread_;
};

// A pane's own load of a document, and the text renderers, the renderer configuration and the media that it was loaded
// for.
struct PaneLoad {
    TextBackends text;
    uint32_t config = 0;
    page_host::Media media = page_host::Media_Screen;
    std::shared_ptr<Loaded> loaded;
};

// The system's browser printing the document's page to a PDF, for the browser's reference in print media. The browser
// loads the page, waits for its fonts, and prints on the paper that the engine's prints start from
// (prLayoutPageBoxDefault). Its callbacks run on the application's thread, so no frame waits for the print. The print
// ends with its browser, which calls nothing more once destroyed.
struct BrowserPrint {
    PrWebView* PR_NULLABLE browser = nullptr;
    std::filesystem::path path;                     // where the PDF is written
    PrintState state = PrintState_Pending;
    std::shared_ptr<Loaded> loaded;                 // the PDF as a source, once printed, if the platform draws PDFs

    BrowserPrint() = default;
    BrowserPrint(const BrowserPrint&) = delete;
    BrowserPrint& operator=(const BrowserPrint&) = delete;
    ~BrowserPrint() { prWebViewDestroy(browser); }
};

// A document, loaded with the references that the engine draws, and loaded again by each pane, so that each pane's view
// is independent of the other's.
struct Opened {
    std::vector<Reference> references;              // the document's references (referencesOf)

    // Each reference's source, or null for a browser's page. A printed PDF's source is loaded once it is printed, if
    // the platform draws PDFs.
    std::vector<std::shared_ptr<Loaded>> loaded;

    std::vector<std::shared_ptr<PrintJob>> prints;  // each printed reference's print, or null for another reference
    std::shared_ptr<BrowserPrint> browserPrint;     // the browser's print of the page, once a pane asks for it
    PaneLoad panes[2];                              // by pane: the pane's own load
};

// The system's browser of a pane that shows a page, created when the pane first shows one. It is shown live over the
// pane and plays on its own. The fields hold the page it has, whether a command is pending, what it was last told, and
// how fast it reports drawing. It was told either to play from the clock's setting or to stand at a time.
struct PaneBrowser {
    PrWebView* PR_NULLABLE browser = nullptr;
    std::string url;
    bool loaded = false;
    bool busy = false;
    bool playing = false;
    uint64_t playedFrom = 0;            // the clock setting that it plays from
    double time = -1;                   // the time that it stands at, negative for none
    std::chrono::steady_clock::time_point asked;
    Rendered figures;

    PrVec2 scrolled{-1, -1};            // the scroll offset that it was last told, negative for none

    // Incremented on each load. A reply tagged with an older value is dropped. The next load is sent without waiting
    // for replies from earlier pages.
    uint64_t generation = 0;
};

struct Viewer {
    PrGpuDevice* device = nullptr;
    PrGpuSwapChain* swapChain = nullptr;
    bool swapChainHdr = false;                  // true if the swap chain is for HDR

    // The display that the panes render for, and the user's preferences, at the last frame. Without HDR, the display is
    // SDR in sRGB's gamut, which is all that an 8-bit swap chain shows.
    PrAppDisplay display = prAppDisplayDefault();
    PrAppUserPreferences preferences = prAppUserPreferencesDefault();
    std::vector<RendererConfig> configs;

    // A renderer is used by one thread at a time. The viewer thread and the panes hold these locks while they use a
    // renderer.
    std::vector<std::unique_ptr<std::mutex>> configLocks;

    // The number of frames that the device has finished. A pane waits on this before it writes a texture that one of
    // those frames drew.
    uint64_t frames = 0;
    std::atomic<uint64_t> framesDone{0};
    std::mutex framesMutex;
    std::condition_variable framesDoneChanged;

    Ui* ui = nullptr;
    PrGpuTexture frame{};           // rests in the accesses of shader reads

    // What maps the frame, which holds lib/draw's colors, to the display in a swap chain of HDR, made on first use.
    PrDrawDisplay* PR_NULLABLE displayShader = nullptr;

    std::unique_ptr<Pane> panes[2];     // as Choices::panes
    PaneBrowser browsers[2];            // the browser of each pane
    PrGpuTexture difference{};
    PrApp* PR_NULLABLE app = nullptr;
    PrAppWindow* PR_NULLABLE window = nullptr;
    std::unique_ptr<Comparer> comparer;
    Comparer::Result result;
    std::unique_ptr<Printer> printer;
    Choices choices;
    Figures figures;
    PrRect content{};           // where documents go, in logical pixels
    float zoomed = 1;           // the zoom that the panes last showed, which a change of zoom scrolls from

    // The engine's render of a page takes input as the browser's live page does: a press on it gives it the keyboard,
    // and a press elsewhere takes the keyboard back. A press that started on it keeps the pointer until its release.
    // `pagePane` is the pane whose page has the keyboard and, while pressed, the pointer.
    bool pageKeys = false;
    bool pagePressed = false;
    uint32_t pagePane = 0;
    float pointerX = 0;
    float pointerY = 0;
    uint32_t touring = 0;       // frames left to turn through, two for each document

    // The command line's names, resolved to renderers or references once they exist.
    const char* configName = nullptr;
    const char* referenceName = nullptr;
    const char* snapshot = nullptr;
    const char* collectionName = nullptr;

    // The pane whose document the wheel or a zoom last scrolled, and when, which the other pane follows.
    uint32_t scrolledPane = 0;
    std::chrono::steady_clock::time_point scrolledAt;

    // The document shown, loaded from the collection's listing, with its references and the collection and document it
    // was loaded for. Another document replaces it.
    std::shared_ptr<Opened> open;
    uint32_t openCollection = 0;
    uint32_t openDocument = 0;

    SharedClock clock;
    uint64_t clockSets = 0;             // the number of times the clock was set, each a start to play from

    // The document the clock runs for. A change restarts the clock from zero.
    uint32_t timedCollection = 0;
    uint32_t timedDocument = 0;

    bool profile = std::getenv("PR_VIEWER_PROFILE") != nullptr;
};

// Returns a pane's rectangle in logical pixels. Side by side, the first pane is on the left and the second on the
// right. Otherwise, the pane is the whole content area.
PrRect paneRect(const Viewer& v, uint32_t index) {
    PrRect c = v.content;
    if (v.choices.view != View_Compare || v.choices.look != Look_SideBySide)
        return c;
    float w = (prRectWidth(c) - kGap) / 2;
    float left = c.left + (index == 1 ? w + kGap : 0);
    return PrRect{left, c.top, left + w, c.bottom};
}

// Returns true if the view shows the pane's own render: alone, side by side, or flipped to it. A difference shows
// neither.
bool paneShown(const Viewer& v, uint32_t index) {
    if (v.choices.view == View_Single)
        return index == 0;
    if (v.choices.look == Look_SideBySide)
        return true;
    if (v.choices.look == Look_Flip)
        return index == (v.choices.showingSecond ? 1u : 0u);
    return false;
}

// Draws the texture where the slot's render goes, clipped to the rectangle. The texture's texels are the window's
// target pixels.
void showTexture(const Viewer& v, const Slot& slot, PrGpuTexture texture, PrRect rect) {
    if (texture.id != 0)
        uiImage(v.ui, texture, rect, slot.at, true);
}

// Returns true if the pane shows Prax, either by its choice or because the document has no such reference.
bool showsPrax(const PaneChoice& pane, const Opened& document) {
    return pane.renderer == Renderer_Prax || pane.reference >= document.references.size();
}

// Returns what a pane shows, except the time: the document, what draws it, and the view of it that the source renders.
std::string paneStem(const Viewer& v, const PaneChoice& pane, const Opened& document, const PaneSource& source) {
    char stem[320];
    const PaneOutput& output = source.output;
    bool prax = showsPrax(pane, document);
    std::snprintf(stem, sizeof(stem), "%u %u %s %u %u %u %u %ux%u %gx%g %g %g %g %u %g %u %u",
        v.choices.collection, v.choices.document, prax ? "prax" : "reference", prax ? pane.config : pane.reference,
        uint32_t(pane.text.shaper), uint32_t(pane.text.hinting), uint32_t(pane.media), source.width, source.height,
        double(source.docWidth), double(source.docHeight), double(source.pixelRatio), double(source.scroll.x),
        double(source.scroll.y),
        uint32_t(output.format), double(output.display.headroom), uint32_t(output.display.gamut),
        uint32_t(output.preferences.colorScheme));
    return stem;
}

// ---------------------------------------------------------------------------------------------------------------------
// The browser

// Returns the number after `"key":` in the JSON, or `fallback` if there is none.
double jsonNumberOf(PrStringView json, const char* key, double fallback) {
    std::string text(json.data != nullptr ? json.data : "", json.size);
    std::string quoted = std::string("\"") + key + "\":";
    size_t at = text.find(quoted);
    if (at == std::string::npos)
        return fallback;
    return std::strtod(text.c_str() + at + quoted.size(), nullptr);
}

// A command to a pane's browser, whose answer is for the page it was sent to.
struct BrowserCommand {
    Viewer* v;
    uint32_t pane;
    uint64_t generation;
};

// Returns the browser that the command's answer is for, or null if the command's page is no longer the one loaded.
PaneBrowser* PR_NULLABLE answered(void* context) {
    BrowserCommand* command = static_cast<BrowserCommand*>(context);
    Viewer* v = command->v;
    PaneBrowser& b = v->browsers[command->pane];
    bool current = command->generation == b.generation;
    delete command;
    if (!current)
        return nullptr;
    b.busy = false;
    prAppWindowRequestFrame(v->window);
    return &b;
}

void browserLoaded(void* context, PrStatus status) {
    PaneBrowser* b = answered(context);
    if (b == nullptr)
        return;
    b->loaded = status == PrStatus_Ok;
    if (!b->loaded)
        b->url.clear();
    b->scrolled = PrVec2{-1, -1};
    b->playing = false;
    b->time = -1;
}

void browserPlays(void* context, PrStatus, PrStringView) {
    answered(context);
}

void browserStats(void* context, PrStatus status, PrStringView json) {
    PaneBrowser* b = answered(context);
    if (b == nullptr)
        return;
    if (status == PrStatus_Ok && b->playing) {
        b->figures.fps = jsonNumberOf(json, "fps", 0);
        b->figures.frame = jsonNumberOf(json, "frame", -1);
    }
}

// The page standing at the time, captured into the pane for the comparer and the difference.
struct BrowserCapture {
    Viewer* v;
    uint32_t index;
    uint64_t generation;
    PaneSource source;
    uint64_t version;
    double time;
    double frame;                       // the frame that the page was set to, or negative for none
    std::chrono::steady_clock::time_point begin;
};

void browserCaptured(void* context, PrStatus status, PrPixelView pixels) {
    BrowserCapture* c = static_cast<BrowserCapture*>(context);
    Viewer& v = *c->v;
    PaneBrowser& b = v.browsers[c->index];
    if (c->generation != b.generation) {
        delete c;
        return;
    }
    b.busy = false;
    Pane& pane = *v.panes[c->index];
    if (pane.source().stem == c->source.stem && pane.source().url == c->source.url) {
        uint32_t index = pane.begin();
        Slot& slot = pane.slot(index);
        Rendered r{};
        r.captured = true;
        r.status = status;
        r.time = c->time;
        r.frame = c->frame;
        std::string shown = shownAt(c->source.stem, c->time);
        if (status == PrStatus_Ok) {
            if (sizeSlot(v.device, slot, pixels.extent.x, pixels.extent.y) == PrStatus_Ok) {
                prGpuTextureWrite(slot.device, slot.texture, PrIVec2{0, 0}, pixels);
                if (c->source.compare != nullptr)
                    c->source.compare(shown, pixels);
            }
            r.width = pixels.extent.x;
            r.height = pixels.extent.y;
        }
        r.ms = millisSince(c->begin);
        slot.rendered = r;
        slot.shown = shown;
        slot.at = c->source.at;
        pane.publish(index, c->version);
    }
    delete c;
    prAppWindowRequestFrame(v.window);
}

// Handles the answer once the page stands at the time. If the answer is a number, it is the frame that the page's
// player was set to.
void browserStood(void* context, PrStatus status, PrStringView json) {
    BrowserCapture* c = static_cast<BrowserCapture*>(context);
    if (json.size > 0) {
        std::string text(json.data, json.size);
        char* end = nullptr;
        double frame = std::strtod(text.c_str(), &end);
        if (end != text.c_str())
            c->frame = frame;
    }
    if (status != PrStatus_Ok) {
        browserCaptured(context, status, PrPixelView{});
        return;
    }
    const PaneSource& source = c->source;
    // The capture holds the part of the viewport that the pane's texture holds.
    PrRect region{0, 0, float(source.width) / source.pixelRatio, float(source.height) / source.pixelRatio};
    prWebViewCapture(c->v->browsers[c->index].browser, region, PrUVec2{source.width, source.height}, browserCaptured, c);
}

// Tells the browser of pane `index`, which shows a page, the next thing that the pane needs, if nothing is pending.
// That is loading the page. While the clock runs, it is playing from the clock's setting, then reporting how fast it
// draws once a second. While the clock stands, it is standing at its time and capturing.
void browserNext(Viewer& v, uint32_t index, bool running) {
    Pane& pane = *v.panes[index];
    PaneBrowser& b = v.browsers[index];
    const PaneSource& source = pane.source();
    if (b.browser == nullptr)
        return;
    auto viewport = [](float size) { return uint32_t(std::lround(size)); };
    // Another page loads immediately, regardless of what the last page is doing, and the last page's answers no longer
    // count.
    if (b.url != source.url) {
        prWebViewResize(b.browser, PrUVec2{viewport(source.docWidth), viewport(source.docHeight)});
        b.url = source.url;
        b.loaded = false;
        b.busy = true;
        ++b.generation;
        prWebViewLoad(b.browser, source.url, browserLoaded, new BrowserCommand{&v, index, b.generation});
        return;
    }
    if (b.busy)
        return;
    prWebViewResize(b.browser, PrUVec2{viewport(source.docWidth), viewport(source.docHeight)});
    if (!b.loaded)
        return;
    char script[2048];
    if (b.scrolled.x != source.scroll.x || b.scrolled.y != source.scroll.y) {
        b.scrolled = source.scroll;
        std::snprintf(script, sizeof(script), "window.scrollTo(%g, %g)", double(source.scroll.x),
            double(source.scroll.y));
        b.busy = true;
        prWebViewEvaluate(b.browser, script, browserPlays, new BrowserCommand{&v, index, b.generation});
        return;
    }
    if (running) {
        if (!b.playing || b.playedFrom != v.clockSets) {
            b.playing = true;
            b.playedFrom = v.clockSets;
            b.time = -1;
            b.figures = Rendered{};
            b.figures.captured = true;
            b.figures.time = -1;
            b.asked = std::chrono::steady_clock::now();
            std::snprintf(script, sizeof(script), "window.prPlay && window.prPlay(%.6f)", v.clock.now());
            b.busy = true;
            prWebViewEvaluate(b.browser, script, browserPlays, new BrowserCommand{&v, index, b.generation});
        } else if (millisSince(b.asked) >= 1000) {
            b.asked = std::chrono::steady_clock::now();
            b.busy = true;
            prWebViewEvaluate(b.browser, "window.prStats ? window.prStats() : null", browserStats,
                new BrowserCommand{&v, index, b.generation});
        }
        return;
    }
    double time = v.clock.now();
    if (!b.playing && b.time == time && pane.holds(shownAt(source.stem, time)))
        return;
    b.playing = false;
    b.time = time;
    BrowserCapture* c = new BrowserCapture{
        &v,   index, b.generation, source, pane.version(),
        time, -1,    std::chrono::steady_clock::now(),
    };
    b.busy = true;
    std::snprintf(script, sizeof(script), "(function(t) { if (window.prSetTime) return window.prSetTime(t);"
        " var root = document.documentElement;"
        " if (root && root.pauseAnimations) { root.pauseAnimations(); root.setCurrentTime(t); }"
        " if (document.getAnimations)"
        " for (var a of document.getAnimations()) { a.pause(); a.currentTime = t * 1000; }})(%.6f)", time);
    prWebViewEvaluate(b.browser, script, browserStood, c);
}

// Shows the page of pane `index` live where the view shows the pane, placed where its texture would be. Otherwise,
// takes the page out of the window.
void browserShow(Viewer& v, uint32_t index) {
    PaneBrowser& b = v.browsers[index];
    if (b.browser == nullptr)
        return;
    const PaneSource& source = v.panes[index]->source();
    if (source.url.empty() || !paneShown(v, index)) {
        prWebViewHide(b.browser);
        return;
    }
    float zoom = v.choices.zoom;
    PrRect viewport{
        source.at.left,
        source.at.top,
        source.at.left + source.docWidth * zoom,
        source.at.top + source.docHeight * zoom,
    };
    PrSurface host{};
    if (prAppWindowSurface(v.window, &host) == PrStatus_Ok)
        prWebViewShow(b.browser, &host, source.at, viewport);
}

// ---------------------------------------------------------------------------------------------------------------------
// The panes

// Returns the document that the choices show, reopened if they have turned to another document since it was last
// opened.
Opened& shownDocument(Viewer& v) {
    if (v.open == nullptr || v.openCollection != v.choices.collection || v.openDocument != v.choices.document) {
        v.open.reset();
        v.openCollection = v.choices.collection;
        v.openDocument = v.choices.document;
        const Document& listed = allCollections()[v.openCollection].documents()[v.openDocument];
        v.open = std::make_shared<Opened>();
        v.open->references = referencesOf(listed);
        v.choices.scroll = PrVec2{0, 0};
        // Each opening prints the printed references again, if their pages have changed or this process has not printed
        // them, so that the PDF shows the engine and the page as they are.
        for (const Reference& reference : v.open->references) {
            bool printed = !reference.printed.path.empty();
            bool drawn = reference.url.empty() && !printed;
            std::shared_ptr<Loaded> loaded;
            if (drawn)
                loaded = load(reference.source, TextBackends{}, page_host::Media_Screen, nullptr);
            v.open->loaded.push_back(std::move(loaded));
            v.open->prints.push_back(printed ? v.printer->ask(reference) : nullptr);
        }
        // Finds a reference named on the command line that belongs to the document rather than to a renderer.
        for (uint32_t r = 0; v.referenceName != nullptr && r < v.open->references.size(); ++r) {
            const std::string& name = v.open->references[r].name;
            if (name == v.referenceName || fileNameOf(name) == fileNameOf(v.referenceName)) {
                v.choices.panes[1].renderer = Renderer_Reference;
                v.choices.panes[1].reference = r;
                v.choices.view = View_Compare;
            }
        }
    }
    return *v.open;
}

// Returns the document as pane `index` loaded it. Loads it again if the pane's text renderers, renderer configuration
// or media changed since its last load. The load is presented on the device that the configuration's renderer renders
// on.
std::shared_ptr<Loaded> paneDocument(Viewer& v, Opened& document, uint32_t index) {
    const PaneChoice& choice = v.choices.panes[index];
    PaneLoad& pane = document.panes[index];
    const Document& listed = allCollections()[v.openCollection].documents()[v.openDocument];
    page_host::Media media = paneMedia(choice, listed.source);
    if (pane.loaded == nullptr || pane.text != choice.text || pane.config != choice.config || pane.media != media) {
        PrGpuDevice* device = prDrawRendererDevice(v.configs[choice.config].renderer);
        pane = PaneLoad{choice.text, choice.config, media, load(listed.source, choice.text, media, device)};
        // A load opens at the top of the document, and the other pane follows it there.
        v.choices.scroll = PrVec2{0, 0};
    }
    return pane.loaded;
}

// Returns true if the pane shows the browser's reference printed: its choice is the browser showing the document's
// page, in print media.
bool printsBrowser(const Viewer& v, const Opened& document, const PaneChoice& choice) {
    const Document& listed = allCollections()[v.openCollection].documents()[v.openDocument];
    return !showsPrax(choice, document) && document.references[choice.reference].browsed &&
        paneMedia(choice, listed.source) == page_host::Media_Print;
}

void browserPrintDone(void* context, PrStatus status) {
    BrowserPrint* print = static_cast<BrowserPrint*>(context);
    print->state = status == PrStatus_Ok ? PrintState_Printed : PrintState_Failed;
    if (status != PrStatus_Ok)
        std::fprintf(stderr, "viewer: the browser did not print %s\n", print->path.string().c_str());
}

void browserPrintReady(void* context, PrStatus, PrStringView) {
    BrowserPrint* print = static_cast<BrowserPrint*>(context);
    PrLayoutVec paper = prLayoutPageBoxDefault().extent;
    if (prWebViewPrint(print->browser, print->path.string(), prLayoutVecToPx(paper), browserPrintDone, print) !=
            PrStatus_Ok)
        print->state = PrintState_Failed;
}

void browserPrintLoaded(void* context, PrStatus status) {
    BrowserPrint* print = static_cast<BrowserPrint*>(context);
    if (status != PrStatus_Ok) {
        print->state = PrintState_Failed;
        return;
    }
    prWebViewEvaluate(print->browser, "document.fonts.ready.then(() => 0)", browserPrintReady, print);
}

// Starts the browser's print of the document's page at `url`, unless it has started, and returns it.
BrowserPrint& browserPrint(Viewer& v, Opened& document, const std::string& url) {
    if (document.browserPrint == nullptr) {
        document.browserPrint = std::make_shared<BrowserPrint>();
        BrowserPrint& print = *document.browserPrint;
        const Document& listed = allCollections()[v.openCollection].documents()[v.openDocument];
        print.path = viewerCache() / "browser" / (fileNameOf(listed.name) + ".pdf");
        std::error_code error;
        std::filesystem::create_directories(print.path.parent_path(), error);
        PrLayoutVec paper = prLayoutPageBoxDefault().extent;
        PrVec2 px = prLayoutVecToPx(paper);
        PrWebViewDesc desc{.viewport = PrUVec2{uint32_t(std::lround(px.x)), uint32_t(std::lround(px.y))}};
        if (prWebViewCreate(PrWebViewBackend_Default, &desc, &print.browser) != PrStatus_Ok) {
            print.state = PrintState_Failed;
        } else {
            prWebViewLoad(print.browser, url, browserPrintLoaded, &print);
        }
    }
    BrowserPrint& print = *document.browserPrint;
    if (print.state == PrintState_Printed && print.loaded == nullptr && pdfDrawn())
        print.loaded = loadPrinted(print.path, PrDrawWriterBackend_Pdf);
    return print;
}

// Returns the source that the engine draws for the reference that the pane chooses, or null if a browser shows it or
// nothing shows yet. A printed PDF is drawn once its print ends, if the platform draws PDFs, and so is the browser's
// print of the page in print media.
std::shared_ptr<Loaded> shownReference(Viewer& v, Opened& document, const PaneChoice& choice) {
    if (printsBrowser(v, document, choice))
        return browserPrint(v, document, document.references[choice.reference].url).loaded;
    const Reference& reference = document.references[choice.reference];
    std::shared_ptr<Loaded>& loaded = document.loaded[choice.reference];
    const std::shared_ptr<PrintJob>& job = document.prints[choice.reference];
    bool pdf = reference.printed.format == PrDrawWriterBackend_Pdf;
    if (loaded == nullptr && job != nullptr && job->state == PrintState_Printed && (!pdf || pdfDrawn()))
        loaded = loadPrinted(reference.printed.path, reference.printed.format);
    return loaded;
}

// Returns the URL that the system's browser shows for the reference that the pane chooses, or empty while it has none:
// a printed document shows nothing until its print ends.
std::string referenceUrl(Viewer& v, Opened& document, const PaneChoice& choice) {
    const Reference& reference = document.references[choice.reference];
    if (printsBrowser(v, document, choice)) {
        const BrowserPrint& print = browserPrint(v, document, reference.url);
        return print.state == PrintState_Printed ? fileUrl(print.path) : std::string();
    }
    const std::shared_ptr<PrintJob>& job = document.prints[choice.reference];
    if (job == nullptr)
        return reference.url;
    return job->state == PrintState_Printed ? fileUrl(reference.printed.path) : std::string();
}

// Gives pane `index` what its choice says that it shows: the document rendered by Prax, or one of its references, at
// the view's zoom, in target pixels of a display of `pixelRatio`, laid out in the viewport that its collection
// declares. The viewport is centered in the pane, and the pane shows as much of it as fits. In comparison, the pane's
// renders go to the comparer while the clock stands.
void showPane(Viewer& v, uint32_t index, float pixelRatio) {
    Opened& document = shownDocument(v);
    const PaneChoice& choice = v.choices.panes[index];
    bool prax = showsPrax(choice, document);
    std::shared_ptr<Loaded> engine = paneDocument(v, document, prax ? index : 0);
    std::shared_ptr<Loaded> shown = prax ? engine : shownReference(v, document, choice);
    float zoom = v.choices.zoom;
    PrRect rect = paneRect(v, index);
    const Collection& collection = allCollections()[v.choices.collection];
    PrVec2 visible{
        std::min(collection.width, prRectWidth(rect) / zoom),
        std::min(collection.height, prRectHeight(rect) / zoom),
    };
    PaneSource source;
    // A viewport smaller than the pane is centered in it. A larger one fills the pane and scrolls.
    float left = rect.left + (prRectWidth(rect) - visible.x * zoom) / 2;
    float top = rect.top + (prRectHeight(rect) - visible.y * zoom) / 2;
    source.at = PrRect{left, top, left + visible.x * zoom, top + visible.y * zoom};
    source.docWidth = collection.width;
    source.docHeight = collection.height;
    source.pixelRatio = zoom * pixelRatio;
    source.width = uint32_t(std::lround(visible.x * source.pixelRatio));
    source.height = uint32_t(std::lround(visible.y * source.pixelRatio));
    if (source.width == 0 || source.height == 0) {
        v.panes[index]->show(PaneSource{});
        return;
    }
    PrPixelFormat format = v.choices.hdr ? PrPixelFormat_RgbaF16 : PrPixelFormat_Bgra8;
    source.output = PaneOutput{format, v.display, v.preferences};
    source.background = kBackdrop;
    source.backToBack = v.choices.backToBack;
    // A reference that the engine draws draws with the cpu renderer, which is the first.
    source.config = prax ? choice.config : 0;
    source.loaded = shown;
    if (shown == nullptr)
        source.url = referenceUrl(v, document, choice);
    source.scroll = v.choices.scroll;
    source.stem = paneStem(v, choice, document, source);
    if (v.choices.view == View_Compare && !v.clock.running()) {
        Comparer* comparer = v.comparer.get();
        source.compare = [comparer, index](const std::string& shown, PrPixelView texels) {
            comparer->deliver(index, shown, texels);
        };
    }
    v.panes[index]->show(std::move(source));
}

// Asks the comparer for the difference of the shown panes, and takes its result if it is of those panes, keeping the
// difference as a texture to show. The controls show it from the next frame.
void updateDifference(Viewer& v, const std::string& first, const std::string& second) {
    v.comparer->want(first, second);
    bool current = v.figures.compared && v.result.shown[0] == first && v.result.shown[1] == second;
    v.figures.comparing = !current;
    if (!v.comparer->take(v.result))
        return;
    if (v.result.shown[0] != first || v.result.shown[1] != second)
        return;
    v.figures.difference = v.result.difference;
    v.figures.compared = true;
    v.figures.comparing = false;
    if (sizeTexture(v.device, v.difference, v.result.width, v.result.height) == PrStatus_Ok) {
        PrPixels texels{v.result.diff.data(), size_t(v.result.width) * 4, {v.result.width, v.result.height}};
        prGpuTextureWrite(v.device, v.difference, PrIVec2{0, 0}, prPixelsView(texels));
    }
}

// Makes the device that the viewer presents with, its swap chain over the window, and what renders on the device.
// Returns false if there is no device to present with.
bool openViewer(Viewer& v) {
    PrApp* app = v.app;
    PrAppWindow* window = v.window;
    PrSurface surface{};
    PrGpuSwapChainDesc desc = swapChainDesc(false);
    if (prGpuDeviceCreate(PrGpuBackend_Default, nullptr, &v.device) != PrStatus_Ok ||
            prAppWindowSurface(window, &surface) != PrStatus_Ok ||
            prGpuSwapChainCreate(v.device, &surface, &desc, &v.swapChain) != PrStatus_Ok) {
        std::fprintf(stderr, "viewer: no GPU device to present with\n");
        return false;
    }
    PrGpuDeviceInfo info = prGpuDeviceInfo(v.device);
    PrStringView api = info.backend;
    std::printf("viewer: %.*s on %.*s\n", int(api.size), api.data, int(info.adapter.size), info.adapter.data);
    v.configs = createConfigs(v.device);
    for (PaneChoice& pane : v.choices.panes) {
        pane.shown.assign(familyCount(v.configs), 0);
        for (uint32_t b = uint32_t(v.configs.size()); b-- > 0;)
            pane.shown[v.configs[b].family] = b;
        pane.config = defaultConfig(v.configs);
    }
    v.comparer = std::make_unique<Comparer>(app);
    v.printer = std::make_unique<Printer>(app);
    if (v.configName != nullptr && indexOf(v.configs, v.configName) >= 0)
        v.choices.panes[0].config = uint32_t(indexOf(v.configs, v.configName));
    // The second pane shows the document's first reference, unless the command line names a renderer for it. It sets
    // its text and its media as the first does.
    v.choices.panes[1].renderer = Renderer_Reference;
    v.choices.panes[1].text = v.choices.panes[0].text;
    v.choices.panes[1].media = v.choices.panes[0].media;
    if (v.referenceName != nullptr && indexOf(v.configs, v.referenceName) >= 0) {
        v.choices.panes[1].renderer = Renderer_Prax;
        v.choices.panes[1].config = uint32_t(indexOf(v.configs, v.referenceName));
        v.choices.view = View_Compare;
    }
    for (size_t b = 0; b < v.configs.size(); ++b)
        v.configLocks.push_back(std::make_unique<std::mutex>());
    PaneContext shared{
        app,      v.device,      &v.configs,    &v.configLocks,
        &v.clock, &v.framesDone, &v.framesMutex, &v.framesDoneChanged,
    };
    for (std::unique_ptr<Pane>& pane : v.panes)
        pane = std::make_unique<Pane>(shared, true);
    v.ui = uiCreate(v.device, window);
    return true;
}

bool open(void*, PrAppWindow*, const PrAppFrame*) {
    return true;
}

void frameDone(void* PR_NULLABLE context, double) {
    Viewer* v = static_cast<Viewer*>(context);
    std::lock_guard<std::mutex> lock(v->framesMutex);
    v->framesDone.fetch_add(1);
    v->framesDoneChanged.notify_all();
}

// The viewer thread's frame: the controls, and each pane's latest render, taken as it is, because the panes render
// independently. The display paces the frames, and taking a pane's render lets the pane start its next.
bool inside(PrRect r, float x, float y) {
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

// Scrolls the other pane's document to the offset of the document that was last scrolled, so that the panes show the
// same part of the document. A browser's document follows by its source (browserNext). A page reports its scroll once
// its rendering update takes the scroll, so the panes follow for a while after the last scroll.
void followScroll(Viewer& v) {
    if (millisSince(v.scrolledAt) > 2000)
        return;
    const PaneSource& leader = v.panes[v.scrolledPane]->source();
    if (leader.loaded == nullptr)
        return;
    PrVec2 offset = scroll(*leader.loaded);
    if (offset.x == v.choices.scroll.x && offset.y == v.choices.scroll.y)
        return;
    v.choices.scroll = offset;
    const PaneSource& other = v.panes[1 - v.scrolledPane]->source();
    if (other.loaded != nullptr) {
        scrollTo(*other.loaded, offset);
        v.panes[1 - v.scrolledPane]->touch();
    }
}

// Applies a change of the zoom since the panes last showed it. Fitting the width zooms the first pane's document so
// that what its viewport scrolls through is as wide as the pane. Each pane's document then scrolls to keep the point at
// the pane's center fixed.
void applyZoom(Viewer& v) {
    const PaneSource& first = v.panes[0]->source();
    if (v.choices.fitWidth && first.loaded != nullptr) {
        float width = contentWidth(*first.loaded);
        if (width > 0)
            v.choices.zoom = prRectWidth(paneRect(v, 0)) / width;
    }
    v.choices.fitWidth = false;
    if (v.choices.zoom == v.zoomed)
        return;
    PrRect shown = paneRect(v, 0);
    PrVec2 offset{
        anchoredScroll(v.choices.scroll.x, prRectWidth(shown), v.zoomed, v.choices.zoom),
        anchoredScroll(v.choices.scroll.y, prRectHeight(shown), v.zoomed, v.choices.zoom),
    };
    v.zoomed = v.choices.zoom;
    v.choices.scroll = offset;
    for (std::unique_ptr<Pane>& pane : v.panes) {
        if (pane->source().loaded != nullptr) {
            scrollTo(*pane->source().loaded, offset);
            pane->touch();
        }
    }
}

// Returns the panel's output with that of the page that holds the keyboard, where its pane shows it.
PrAppOutput pageOutput(Viewer& v, const PrAppOutput& panel, double time) {
    uint32_t index = v.pagePane;
    if (!v.pageKeys)
        return panel;
    const PaneSource& source = v.panes[index]->source();
    if (source.loaded == nullptr || !paneShown(v, index))
        return panel;
    PrAppOutput page = prAppOutputPlaced(output(*source.loaded, v.window, time), PrVec2{source.at.left, source.at.top},
        v.choices.zoom);
    // A caret that blinks changes the page's render.
    if (page.wantsFrame || page.frameTime != 0)
        v.panes[index]->touch();
    return prAppOutputMerge(panel, page, inside(source.at, v.pointerX, v.pointerY));
}

PrAppOutput frame(void* context, PrAppWindow* window, const PrAppFrame* frame) {
    Viewer& v = *static_cast<Viewer*>(context);
    PrApp* app = v.app;
    if (v.swapChain == nullptr || !(frame->extent.x > 0 && frame->extent.y > 0))
        return {};
    float pixelRatio = frame->display.pixelRatio;
    uint32_t width = uint32_t(std::lround(frame->extent.x * pixelRatio));
    uint32_t height = uint32_t(std::lround(frame->extent.y * pixelRatio));
    // A switch to or from HDR replaces the swap chain once each pane holds a render in the chosen format. The panes
    // render for the controls' choice (showPane), and the swap chain changes on the frame that first shows those
    // renders, so that no frame shows a render through a swap chain of the other format, which would clip its colors.
    PrPixelFormat chosen = v.choices.hdr ? PrPixelFormat_RgbaF16 : PrPixelFormat_Bgra8;
    bool ready = v.panes[0]->rendersIn(chosen) && v.panes[1]->rendersIn(chosen);
    if (v.choices.hdr != v.swapChainHdr && ready) {
        PrSurface surface{};
        PrGpuSwapChain* swapChain = nullptr;
        PrGpuSwapChainDesc desc = swapChainDesc(v.choices.hdr);
        if (prAppWindowSurface(window, &surface) == PrStatus_Ok &&
                prGpuSwapChainCreate(v.device, &surface, &desc, &swapChain) == PrStatus_Ok) {
            prGpuSwapChainDestroy(v.swapChain);
            v.swapChain = swapChain;
            v.swapChainHdr = v.choices.hdr;
        } else {
            v.choices.hdr = v.swapChainHdr;
        }
    }
    // The headroom is kept in quarters, so that the panes render again only when it changes visibly.
    v.display = frame->display;
    if (v.choices.hdr) {
        v.display.headroom = std::max(1.0f, std::round(frame->display.headroom * 4) / 4);
    } else {
        v.display.headroom = 1;
        v.display.gamut = PrColorSpace_Srgb;
    }
    v.preferences = frame->preferences;
    PrPixelFormat format = v.swapChainHdr ? PrPixelFormat_RgbaF16 : PrPixelFormat_Bgra8;
    if (sizeTexture(v.device, v.frame, width, height, format) != PrStatus_Ok)
        return {};
    std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
    // The frame shows the panes' latest renders, which are taken before the controls are laid out, so that the figures
    // that the controls show are those of the renders shown. Panes that the running clock drives each show their own
    // time, so they are not compared.
    ++v.frames;
    const Slot* slots[2] = {&v.panes[0]->take(v.frames, format), &v.panes[1]->take(v.frames, format)};
    bool compared = v.choices.view == View_Compare;
    bool played = v.clock.running();
    v.figures.playing = compared && played;
    if (compared && !played) {
        updateDifference(v, slots[0]->shown, slots[1]->shown);
    } else {
        v.figures.compared = false;
        v.figures.comparing = false;
    }
    for (uint32_t i = 0; i < 2; ++i) {
        bool browsed = (i == 0 || compared) && !v.panes[i]->source().url.empty();
        v.figures.panes[i] = browsed && played ? v.browsers[i].figures : slots[i]->rendered;
    }
    v.content = uiLayout(v.ui, frame, v.pageKeys, v.configs, shownDocument(v).references, v.figures, v.choices);
    double layoutMs = millisSince(begin);

    // The clock starts from zero on each document that the controls or keys turn to, and runs while it plays. When
    // stepped, it stands.
    bool running = v.choices.playing;
    if (v.timedCollection != v.choices.collection || v.timedDocument != v.choices.document) {
        v.timedCollection = v.choices.collection;
        v.timedDocument = v.choices.document;
        v.choices.time = 0;
        v.clock.set(0, running);
        ++v.clockSets;
    } else if (running != v.clock.running() || (!running && v.choices.time != v.clock.now())) {
        v.clock.set(v.choices.time, running);
        ++v.clockSets;
    }
    if (running)
        v.choices.time = v.clock.now();
    shownDocument(v);

    // What each pane shows, which the pane renders on its own. A pane's browser fills the pane of a page, and plays
    // live over the pane where the view shows it.
    bool compare = v.choices.view == View_Compare;
    uint32_t paneCount = compare ? 2 : 1;
    followScroll(v);
    applyZoom(v);
    for (uint32_t i = 0; i < paneCount; ++i)
        showPane(v, i, pixelRatio);
    if (!compare)
        v.panes[1]->show(PaneSource{});
    bool live[2] = {};
    bool liveShown[2] = {};
    for (uint32_t i = 0; i < 2; ++i) {
        const PaneSource& page = v.panes[i]->source();
        live[i] = i < paneCount && !page.url.empty();
        if (live[i] && v.browsers[i].browser == nullptr) {
            PrWebViewDesc desc{
                .viewport = PrUVec2{uint32_t(std::lround(page.docWidth)), uint32_t(std::lround(page.docHeight))},
            };
            prWebViewCreate(PrWebViewBackend_Default, &desc, &v.browsers[i].browser);
        }
        if (live[i])
            browserNext(v, i, running);
        // A browser that shows no page forgets the last, so that a page shown again loads again, as a printed PDF must
        // once it is printed again.
        if (!live[i])
            v.browsers[i].url.clear();
        browserShow(v, i);
        // Under the page, its capture while the clock stands. The capture shows the page as it stands, and is what a
        // snapshot of the frame shows. Nothing is under the page while it plays.
        liveShown[i] = live[i] && running && paneShown(v, i);
    }

    PrGpuCommands* commands = nullptr;
    if (!prGpuCommandsBegin(v.device, nullptr, &commands)) {
        // The viewer shares its device with every pane's thread, and stops instead of making it again.
        std::fprintf(stderr, "viewer: the GPU device was lost\n");
        prAppStop(app);
        return {};
    }
    if (compare && v.choices.look == Look_Difference) {
        if (v.figures.compared)
            showTexture(v, *slots[0], v.difference, v.content);
    } else {
        for (uint32_t i = 0; i < paneCount; ++i)
            if (paneShown(v, i) && !liveShown[i])
                showTexture(v, *slots[i], slots[i]->texture, paneRect(v, i));
    }
    // One pass clears the frame to the backdrop, then draws the panes' renders and the controls.
    uiPrepare(v.ui, commands, PrUVec2{width, height}, format);
    PrGpuSubresources all{};
    prGpuCommandsTextureBarrier(commands, v.frame, all, PrGpuAccessFlag_ShaderRead, PrGpuAccessFlag_ColorAttachment);
    PrGpuPassDesc controls = prGpuPassDescColor(v.frame, PrGpuLoad_Clear, PrColor{kBackdrop.r, kBackdrop.g, kBackdrop.b, 1});
    PrGpuPass* pass = prGpuPassBegin(commands, &controls);
    uiEncode(v.ui, pass);
    prGpuPassEnd(pass);
    prGpuCommandsTextureBarrier(commands, v.frame, all, PrGpuAccessFlag_ColorAttachment, PrGpuAccessFlag_ShaderRead);
    prGpuCommandsOnDone(commands, frameDone, &v);

    // The drawable waits on the display, which paces the frames.
    PrGpuSwapChainFrame swapFrame{};
    PrStatus acquired = prGpuSwapChainAcquire(v.swapChain, PrUVec2{width, height}, &swapFrame);
    if (acquired == PrStatus_Ok) {
        PrGpuTexture drawable = swapFrame.color;
        PrGpuSwapChainInfo info = prGpuSwapChainInfo(v.swapChain);
        prGpuCommandsTextureBarrier(commands, drawable, all, PrGpuAccessFlag_Discard, PrGpuAccessFlag_ColorAttachment);
        // A swap chain of 8-bit sRGB takes the frame as it is. Any other maps it to the display.
        bool mapped = v.swapChainHdr && (info.format != PrGpuFormat_Bgra8 || info.space != PrColorSpace_Srgb);
        if (mapped && v.displayShader == nullptr)
            prDrawDisplayCreate(v.device, &v.displayShader);
        if (mapped && v.displayShader != nullptr) {
            PrGpuPassDesc shown = prGpuPassDescColor(drawable, PrGpuLoad_Clear, PrColor{0, 0, 0, 1});
            PrGpuPass* mapPass = prGpuPassBegin(commands, &shown);
            PrDrawDisplayDesc mapping{.target = info.space, .headroom = v.display.headroom, .gamut = v.display.gamut};
            prDrawDisplayDraw(v.displayShader, mapPass, v.frame, &mapping);
            prGpuPassEnd(mapPass);
            prGpuCommandsTextureBarrier(commands, drawable, all, PrGpuAccessFlag_ColorAttachment,
                PrGpuAccessFlag_Present);
        } else {
            // The window shows the swap chain's texture past the frame while it resizes, which the backdrop fills.
            PrGpuPassDesc backdrop = prGpuPassDescColor(drawable, PrGpuLoad_Clear, kBackdrop);
            prGpuPassEnd(prGpuPassBegin(commands, &backdrop));
            prGpuCommandsTextureBarrier(commands, drawable, all, PrGpuAccessFlag_ColorAttachment,
                PrGpuAccessFlag_CopyWrite);
            prGpuCommandsTextureBarrier(commands, v.frame, all, PrGpuAccessFlag_ShaderRead, PrGpuAccessFlag_CopyRead);
            PrGpuTextureRegion region = prGpuTextureRegion2d(PrUVec2{0, 0}, PrUVec2{width, height});
            prGpuCommandsCopyTexture(commands, v.frame, &region, drawable, &region);
            prGpuCommandsTextureBarrier(commands, v.frame, all, PrGpuAccessFlag_CopyRead, PrGpuAccessFlag_ShaderRead);
            prGpuCommandsTextureBarrier(commands, drawable, all, PrGpuAccessFlag_CopyWrite, PrGpuAccessFlag_Present);
        }
        prGpuSwapChainPresent(v.swapChain, commands);
    }
    // A pane that waits for its reference's print shows nothing yet, which is not its settled render.
    bool printing = false;
    for (uint32_t i = 0; i < paneCount; ++i) {
        const PaneChoice& choice = v.choices.panes[i];
        if (showsPrax(choice, *v.open))
            continue;
        const std::shared_ptr<PrintJob>& job = v.open->prints[choice.reference];
        printing = printing || (job != nullptr && job->state == PrintState_Pending);
    }
    bool settled = !running && !printing && v.panes[0]->settled() && (!compare || v.panes[1]->settled()) &&
        !v.browsers[0].busy && !v.browsers[1].busy;
    if (settled && !v.figures.comparing && v.snapshot != nullptr) {
        prGpuCommandsSubmitAndWait(commands);
        std::vector<uint8_t> pixels(size_t(width) * height * prPixelFormatSize(format));
        PrPixels texels{pixels.data(), size_t(width) * prPixelFormatSize(format), {width, height}};
        prGpuTextureRead(v.device, v.frame, PrGpuAccessFlag_ShaderRead, PrIVec2{0, 0}, texels);
        if (format == PrPixelFormat_RgbaF16)
            pixels = narrowed(prPixelsView(texels));
        if (!writePng(v.snapshot, pixels, width, height))
            std::fprintf(stderr, "viewer: cannot write %s\n", v.snapshot);
        prAppStop(app);
        return {};
    }
    prGpuCommandsSubmit(commands);
    if (v.profile)
        std::fprintf(stderr, "frame %llu: layout %.2f ms, whole %.2f ms; first %.1f FPS %s, %.2f ms, GPU %.3f ms, at %.3f s, "
            "frame %.2f; second %.1f FPS, %.2f ms, GPU %.3f ms, at frame %.2f\n",
            static_cast<unsigned long long>(v.frames), layoutMs, millisSince(begin), v.figures.panes[0].fps,
            v.figures.panes[0].backToBack ? "back to back" : "paced", v.figures.panes[0].ms,
            v.figures.panes[0].gpuMs, v.figures.panes[0].time, v.figures.panes[0].frame,
            v.figures.panes[1].fps, v.figures.panes[1].ms, v.figures.panes[1].gpuMs, v.figures.panes[1].frame);
    // A tour moves on once the panes show the document. While the clock runs, they never stand at its time, so any
    // render of the document will do.
    bool toured = settled;
    if (running)
        toured = (live[0] || v.panes[0]->current()) && (!compare || live[1] || v.panes[1]->current()) &&
            !v.browsers[0].busy && !v.browsers[1].busy;
    if (v.touring != 0 && toured) {
        if (--v.touring == 0) {
            prAppStop(app);
            return {};
        }
        uint32_t count = uint32_t(allCollections()[v.choices.collection].documents().size());
        v.choices.document = (v.choices.document + 1) % count;
    }
    // Every refresh shows the panes' latest renders and takes them, which paces the panes' continuous rendering.
    PrAppOutput panel = uiOutput(v.ui);
    panel.wantsFrame = true;
    return pageOutput(v, panel, frame->time);
}

void keyDown(Viewer& v, PrApp* app, PrAppKey key, PrAppModifierFlags modifiers) {
    Choices& c = v.choices;
    PaneChoice& first = c.panes[0];
    uint32_t count = uint32_t(allCollections()[c.collection].documents().size());
    // Command =, Command + and Command - zoom in and out, and Command 0 shows the actual size.
    if ((modifiers & PrAppModifierFlag_Super) != 0) {
        if (key == '=' || key == '+')
            c.zoom = zoomIn(c.zoom);
        else if (key == '-')
            c.zoom = zoomOut(c.zoom);
        else if (key == '0')
            c.zoom = 1;
        return;
    }
    if (key == 'Q' || key == PrAppKey_Escape) {
        prAppStop(app);
    } else if (key == ']') {
        c.document = (c.document + 1) % count;
        c.documentChanged = true;
    } else if (key == '[') {
        c.document = (c.document + count - 1) % count;
        c.documentChanged = true;
    } else if (key >= '1' && key <= '9' && uint32_t(key - '1') < first.shown.size()) {
        first.renderer = Renderer_Prax;
        chooseFamily(first, v.configs, uint32_t(key - '1'));
    } else if (key == 'V') {
        first.renderer = Renderer_Prax;
        first.config = nextInFamily(v.configs, first.config);
    } else if (key == 'T') {
        // The next shaper that the build has, Default among them.
        do
            first.text.shaper = PrTextShaperBackend((first.text.shaper + 1) % PrTextShaperBackend_Count);
        while (!prTextShaperBackendIsBuilt(first.text.shaper));
    } else if (key == 'P') {
        first.renderer = Renderer_Prax;
        first.media = first.media == page_host::Media_Print ? page_host::Media_Screen : page_host::Media_Print;
    } else if (key == 'X') {
        std::swap(c.panes[0], c.panes[1]);
    } else if (key == 'C') {
        c.view = c.view == View_Single ? View_Compare : View_Single;
    } else if (key == 'S') {
        c.view = View_Compare;
        c.look = Look_SideBySide;
    } else if (key == 'F') {
        if (c.view == View_Compare && c.look == Look_Flip)
            c.showingSecond = !c.showingSecond;
        c.view = View_Compare;
        c.look = Look_Flip;
    } else if (key == 'D') {
        c.view = View_Compare;
        c.look = Look_Difference;
    } else if (key == 'B') {
        c.backToBack = !c.backToBack;
    } else if (key == 'H') {
        c.hdr = !c.hdr;
    } else if (key == ' ') {
        c.playing = !c.playing;
    } else if (key == ',' || key == '.') {
        c.playing = false;
        c.time = key == '.' ? c.time + kFrame : std::fmax(c.time - kFrame, 0.0);
    } else if (key == PrAppKey_Home) {
        c.playing = false;
        c.time = 0;
    }
}

// Returns the engine's render of a page in pane `index` if it takes input and the view shows it, and sets `out` to
// where `window`, in the window's logical pixels, is in the document's units. Returns null for a point that is not on
// the document, unless the page holds the pointer.
Loaded* PR_NULLABLE pageAt(Viewer& v, uint32_t index, PrVec2 window, PrVec2* out) {
    const PaneSource& source = v.panes[index]->source();
    if (source.loaded == nullptr || source.docWidth <= 0 || source.docHeight <= 0 || !paneShown(v, index))
        return nullptr;
    float zoom = v.choices.zoom;
    *out = PrVec2{(window.x - source.at.left) / zoom, (window.y - source.at.top) / zoom};
    bool onPage = inside(source.at, window.x, window.y);
    return onPage || (v.pagePressed && v.pagePane == index) ? source.loaded.get() : nullptr;
}

// Sends the event to the engine's render of the page under it, as the browser's live page takes it. Returns true if the
// page took it. A Command chord stays the viewer's.
bool toPage(Viewer& v, const PrAppEvent& event) {
    switch (event.kind) {
        case PrAppEventKind_PointerMove:
        case PrAppEventKind_PointerDown:
        case PrAppEventKind_PointerUp:
        case PrAppEventKind_Scroll: {
            PrVec2 window =
                event.kind == PrAppEventKind_Scroll ? PrVec2{v.pointerX, v.pointerY} : event.pointer.position;
            PrAppEvent local = event;
            PrVec2 position{};
            Loaded* page = nullptr;
            uint32_t index = 0;
            for (uint32_t i = 0; i < 2 && page == nullptr; ++i) {
                index = v.pagePressed ? v.pagePane : i;
                page = pageAt(v, index, window, &position);
            }
            prAppEventSetPosition(&local, position);
            if (event.kind == PrAppEventKind_PointerDown) {
                v.pageKeys = page != nullptr;
                v.pagePane = index;
            }
            if (page == nullptr)
                return false;
            if (event.kind == PrAppEventKind_PointerDown)
                v.pagePressed = true;
            else if (event.kind == PrAppEventKind_PointerUp)
                v.pagePressed = false;
            input(*page, local);
            v.panes[index]->touch();
            if (event.kind == PrAppEventKind_Scroll) {
                v.scrolledPane = index;
                v.scrolledAt = std::chrono::steady_clock::now();
            }
            return true;
        }
        case PrAppEventKind_KeyDown:
        case PrAppEventKind_KeyUp:
        case PrAppEventKind_Text:
        case PrAppEventKind_Paste:
        case PrAppEventKind_CompositionUpdate:
        case PrAppEventKind_CompositionEnd: {
            const PaneSource& source = v.panes[v.pagePane]->source();
            if (!v.pageKeys || (event.modifiers & PrAppModifierFlag_Super) != 0 || source.loaded == nullptr)
                return false;
            input(*source.loaded, event);
            v.panes[v.pagePane]->touch();
            // A key may scroll the page, which the other pane follows.
            v.scrolledPane = v.pagePane;
            v.scrolledAt = std::chrono::steady_clock::now();
            return true;
        }
        default:
            return false;
    }
}

void event(void* context, PrAppWindow* window, const PrAppEvent* event) {
    Viewer& v = *static_cast<Viewer*>(context);
    if (event->kind == PrAppEventKind_CloseRequested) {
        prAppStop(v.app);
        return;
    }
    if (v.ui == nullptr)
        return;
    prAppWindowRequestFrame(window);
    // Each input has one owner. The keys go to a page that holds the keyboard and to nothing else, apart from Command
    // chords, which stay the viewer's.
    bool key = event->kind == PrAppEventKind_KeyDown || event->kind == PrAppEventKind_KeyUp ||
        event->kind == PrAppEventKind_Text || event->kind == PrAppEventKind_CompositionUpdate ||
        event->kind == PrAppEventKind_CompositionEnd;
    if (key && toPage(v, *event))
        return;
    bool taken = uiEvent(v.ui, *event);
    if (taken && event->kind == PrAppEventKind_PointerDown)
        v.pageKeys = false;
    if (event->kind == PrAppEventKind_PointerDown || event->kind == PrAppEventKind_PointerMove) {
        v.pointerX = event->pointer.position.x;
        v.pointerY = event->pointer.position.y;
    }
    // The pointer and the wheel over a document go to the document.
    if (!taken && !key && toPage(v, *event))
        return;
    if (event->kind == PrAppEventKind_KeyDown && !taken)
        keyDown(v, v.app, event->keyboard.key, event->modifiers);
}

// Opens the viewer's window and its device. The window's close stops the application.
bool start(void* context, PrApp* app) {
    Viewer& v = *static_cast<Viewer*>(context);
    v.app = app;
    PrAppWindowDesc desc{
        .title = "Prax draw viewer",
        .extent = {uint32_t(kWindowWidth), uint32_t(kWindowHeight)},
        .handler = {.context = &v, .open = open, .frame = frame, .event = event},
    };
    return prAppWindowCreate(app, &desc, &v.window) == PrStatus_Ok && openViewer(v);
}

void appEvent(void*, PrApp*, const PrAppEvent*) {}

void shutdown(void* context, PrApp*) {
    Viewer& v = *static_cast<Viewer*>(context);
    if (v.device == nullptr)
        return;
    // The panes stop first, then the device finishes its work. Browser captures still in flight return nothing more.
    for (std::unique_ptr<Pane>& pane : v.panes)
        pane->stop();
    prGpuDeviceWaitIdle(v.device);
    for (std::unique_ptr<Pane>& pane : v.panes)
        pane.reset();
    for (PaneBrowser& b : v.browsers) {
        prWebViewDestroy(b.browser);
        b.browser = nullptr;
    }
    v.comparer.reset();
    v.printer.reset();
    prDrawDisplayDestroy(v.displayShader);
    prGpuTextureDestroy(v.device, v.difference);
    prGpuTextureDestroy(v.device, v.frame);
    uiDestroy(v.ui);
    // The documents are released before the device, because a page's canvases keep their bitmaps on it.
    v.open.reset();
    destroyConfigs(v.configs);
    prGpuSwapChainDestroy(v.swapChain);
    prGpuDeviceDestroy(v.device);
}

} // namespace

int main(int argc, char** argv) {
    prCrashHandler();
    const char* directory = nullptr;
    const char* which = nullptr;
    float pixelRatio = 1;
    bool mediaChosen = false;
    Viewer viewer;
    auto usage = [](FILE* to) {
        std::fprintf(to, "usage: viewer [--document <which>] [--render <directory>] [--pixel-ratio <pixels>]"
            " [--renderer <name>] [--shaper <name>] [--hinting <name>] [--media screen|print] [--reference <name>]"
            " [--look side|flip|difference] [--snapshot <path>] [--tour <passes>] [--collection <word>] [--zoom <percent>]"
            " [--time <seconds>] [--rendering paced|back-to-back] [--present sdr|hdr] [--wpt-server <http port> <https port>]"
            " [--third-party-server <http port>]\n");
    };
    for (int i = 1; i < argc; i += 2) {
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            usage(stdout);
            return 0;
        }
        if (std::strcmp(argv[i], "--wpt-server") == 0 && i + 2 < argc) {
            serveWptFrom(uint16_t(std::atoi(argv[i + 1])), uint16_t(std::atoi(argv[i + 2])));
            ++i;
            continue;
        }
        if (std::strcmp(argv[i], "--third-party-server") == 0 && i + 1 < argc) {
            serveThirdPartyFrom(uint16_t(std::atoi(argv[i + 1])));
            continue;
        }
        if (i + 1 == argc) {
            std::fprintf(stderr, "%s takes a value\n", argv[i]);
            usage(stderr);
            return 2;
        }
        const char* value = argv[i + 1];
        if (std::strcmp(argv[i], "--render") == 0) {
            directory = value;
        } else if (std::strcmp(argv[i], "--pixel-ratio") == 0) {
            pixelRatio = std::strtof(value, nullptr);
        } else if (std::strcmp(argv[i], "--document") == 0) {
            which = value;
        } else if (std::strcmp(argv[i], "--renderer") == 0) {
            viewer.configName = value;
        } else if (std::strcmp(argv[i], "--shaper") == 0 &&
                prTextShaperBackendIsBuilt(prTextShaperBackendFromName(value))) {
            viewer.choices.panes[0].text.shaper = prTextShaperBackendFromName(value);
        } else if (std::strcmp(argv[i], "--hinting") == 0 && prFontHintingFromName(value) != PrFontHinting_Count) {
            viewer.choices.panes[0].text.hinting = prFontHintingFromName(value);
        } else if (std::strcmp(argv[i], "--media") == 0 && std::strcmp(value, "screen") == 0) {
            viewer.choices.panes[0].media = page_host::Media_Screen;
            mediaChosen = true;
        } else if (std::strcmp(argv[i], "--media") == 0 && std::strcmp(value, "print") == 0) {
            viewer.choices.panes[0].media = page_host::Media_Print;
            mediaChosen = true;
        } else if (std::strcmp(argv[i], "--reference") == 0) {
            viewer.referenceName = value;
        } else if (std::strcmp(argv[i], "--look") == 0 && std::strcmp(value, "side") == 0) {
            viewer.choices.look = Look_SideBySide;
        } else if (std::strcmp(argv[i], "--look") == 0 && std::strcmp(value, "flip") == 0) {
            viewer.choices.look = Look_Flip;
        } else if (std::strcmp(argv[i], "--look") == 0 && std::strcmp(value, "difference") == 0) {
            viewer.choices.look = Look_Difference;
        } else if (std::strcmp(argv[i], "--snapshot") == 0) {
            viewer.snapshot = value;
            viewer.choices.playing = false;
        } else if (std::strcmp(argv[i], "--zoom") == 0 && std::strtof(value, nullptr) > 0) {
            viewer.choices.zoom = std::strtof(value, nullptr) / 100;
            viewer.zoomed = viewer.choices.zoom;
        } else if (std::strcmp(argv[i], "--collection") == 0) {
            viewer.collectionName = value;
        } else if (std::strcmp(argv[i], "--time") == 0) {
            viewer.choices.time = std::strtod(value, nullptr);
            viewer.choices.playing = false;
        } else if (std::strcmp(argv[i], "--rendering") == 0 && std::strcmp(value, "paced") == 0) {
            viewer.choices.backToBack = false;
        } else if (std::strcmp(argv[i], "--rendering") == 0 && std::strcmp(value, "back-to-back") == 0) {
            viewer.choices.backToBack = true;
        } else if (std::strcmp(argv[i], "--present") == 0 && std::strcmp(value, "sdr") == 0) {
            viewer.choices.hdr = false;
        } else if (std::strcmp(argv[i], "--present") == 0 && std::strcmp(value, "hdr") == 0) {
            viewer.choices.hdr = true;
        } else if (std::strcmp(argv[i], "--tour") == 0) {
            viewer.touring = uint32_t(std::strtoul(value, nullptr, 10));
        } else {
            usage(stderr);
            return 2;
        }
    }
    const std::vector<Collection>& collections = allCollections();
    for (uint32_t c = 0; viewer.collectionName != nullptr && c < collections.size(); ++c) {
        if (fileNameOf(collections[c].name).find(fileNameOf(viewer.collectionName)) != std::string::npos) {
            viewer.choices.collection = c;
            break;
        }
    }
    // The engine renders the collection's pages for its media, unless the command line chooses another.
    if (!mediaChosen)
        viewer.choices.panes[0].media = collections[viewer.choices.collection].media;
    if (directory != nullptr)
        return renderAll(directory, collections[viewer.choices.collection], which, viewer.referenceName,
            viewer.choices.panes[0].text, viewer.choices.panes[0].media, viewer.choices.time,
            pixelRatio > 0 ? pixelRatio : 1);
    // The list opens scrolled to the chosen document.
    viewer.choices.documentChanged = true;
    const Collection& opened = collections[viewer.choices.collection];
    // A tour's passes cover the collection that it opens.
    viewer.touring *= uint32_t(opened.documents().size());
    for (uint32_t d = 0; d < uint32_t(opened.documents().size()); ++d) {
        if (documentNamed(opened, d, which)) {
            viewer.choices.document = d;
            break;
        }
    }
    // The clock starts where the command line puts it, on the document that the viewer opens.
    viewer.timedCollection = viewer.choices.collection;
    viewer.timedDocument = viewer.choices.document;
    PrApp* app = prAppCreate(PrAppBackend_Default, nullptr);
    prAppRun(app, {.context = &viewer, .start = start, .event = appEvent, .shutdown = shutdown});
    prAppDestroy(app);
    return 0;
}
