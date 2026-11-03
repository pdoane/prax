// A viewer for what the engine draws: collections of documents, each shown in one pane or compared across two. A pane
// shows Prax, the engine, in a configuration of its renderers: a renderer, a text shaper and a font hinting. Or it
// shows one of the document's references, by the document's type: the system's browser, another player such as
// lottie-web, Skottie or rlottie, or an image such as a suite's reference. The panes are alike, so two of Prax's
// configurations, or two players, compare as Prax and a reference do. A document's first reference is the one that its
// suite compares the engine with, which the second pane opens on.
//
// The controls beside the documents choose the collection, the document, each pane and the view. Keys: the arrows move
// through the tree of documents, opening and closing its directories. [ and ] turn the document. 1 through 9 choose a
// renderer's family for the first pane, returning to the configuration that it last showed. V turns through the
// family's configurations. T turns through the shapers, which reloads the document. X swaps the panes. C switches
// between the single view and the comparison view. S, F and D show a comparison side by side, flipping between the two
// panes, or as their difference. = shows the renders at their natural size, and 0 fits them to their panes. Every
// document renders again at each of the display's refreshes, so that its figures are steady. B renders back to back
// instead, as fast as the device allows, to measure; the figures say which. H presents in half floats, so that colors
// past SDR white and past sRGB's gamut show on a display that can show them; the panes then render in half floats, tone
// mapped to the display's headroom. Space plays and pauses the document's clock. Q or Escape quits. In comparison,
// scrolling zooms and dragging pans both renders together, and the renders open at their natural size. Prax's render of
// a page takes input as the browser's live page does: the pointer and the wheel over it go to the page, and after a
// click on it, so do keys other than Command chords, until a click elsewhere. Scrolling and dragging elsewhere zoom and
// pan.
//
// `viewer --render <directory>` opens no window. It renders every document of the collection with every renderer and
// writes each render as a PNG named for the document and the renderer, creating the directory if needed. The collection
// is the features unless `--collection` names another. `--reference <renderer>` also compares every other renderer's
// render with that renderer's render, as the SVG corpora are compared, and writes the difference beside it. `--document
// <which>` narrows either mode to the documents that it names, by number or by a word of the name, and the window opens
// at the first of them. `--pixel-ratio <pixels>` renders at that many target pixels per unit of the document.
//
// The window opens on `--collection <word>`, the collection whose name contains the word, with the first pane on
// `--renderer <name>`, or on fragment in its default configuration. The first pane, and the renders, shape text with
// `--shaper <name>`, or with the default shaper, and hint fonts as `--hinting <name>` says, or not at all. With
// `--reference <name>`, which names a renderer or one of the document's own references for the second pane, the window
// opens in comparison, at `--zoom <percent>` of the natural size rather than fitted, shown as `--look side`, `flip` or
// `difference`. `--snapshot <path>` writes the window's first frame whose figures are settled as a PNG, including the
// controls, and quits. `--tour <passes>` turns through every document that many times, one per frame, and quits.
// `--rendering back-to-back` opens rendering back to back, as B does, and `--rendering paced` paced by the display.
// `--present hdr` opens presenting in half floats, as H does, and `--present sdr` in 8 bits. `--wpt-server <http port>
// <https port>` serves WPT's reftests from WPT's server (tools/suite/wpt/wpt_serve.py), which `./dev.sh viewer` starts,
// and the WPT collection is listed only with it. With PR_VIEWER_PROFILE set, which prints each frame's phases, the tour
// measures what moving between documents costs.

#include "pr/app.h"
#include "pr/assert.h"
#include "pr/compress_builtin.h"
#include "pr/draw.h"
#include "pr/draw_display.h"
#include "pr/draw_fragment.h"
#include "pr/draw_list.h"
#include "pr/image_builtin.h"
#include "pr/vec.h"

#include "tools/harness/compare.h"
#include "tools/viewer/documents.h"
#include "tools/viewer/drawing.h"
#include "tools/viewer/features.h"
#include "tools/viewer/panes.h"
#include "tools/viewer/renderers.h"
#include "tools/viewer/source_html.h"
#include "tools/viewer/ui.h"

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

// The window's initial size: the controls, and a feature drawing at the size it lays out for.
constexpr float kWindowWidth = 1400;
constexpr float kWindowHeight = 800;

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
    draw(loaded, list, width, height, display, PrAppUserPreferences{});
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

void releaseResources() {
    releaseDrawings();
}

int indexOf(const std::vector<RendererConfig>& configs, const char* name) {
    for (uint32_t b = 0; b < uint32_t(configs.size()); ++b)
        if (std::strcmp(configs[b].name, name) == 0)
            return int(b);
    return -1;
}

int renderAll(const char* directory, const Collection& documents, const char* which, const char* referenceName,
        TextBackends text, double time, float pixelRatio) {
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
    uint32_t width = uint32_t(std::lround(documents.width * pixelRatio));
    uint32_t height = uint32_t(std::lround(documents.height * pixelRatio));
    Slot slot;
    Slot referenceSlot;
    PrDrawList* list = prDrawListCreate();
    int failures = 0;
    for (uint32_t d = 0; d < uint32_t(documents.documents().size()); ++d) {
        if (!documentNamed(documents, d, which))
            continue;
        const Document& document = documents.documents()[d];
        // Each renderer configuration renders a load of its own, presented where its renderer renders.
        auto recordFor = [&](uint32_t config) {
            PrGpuDevice* presenter = prDrawRendererIsOnDevice(configs[config].renderer) ? device : nullptr;
            std::shared_ptr<Loaded> loaded = load(document.source, text, presenter);
            record(list, *loaded, documents.width, documents.height, pixelRatio, time);
            return loaded;
        };
        std::string stem = std::string(directory) + "/" + std::to_string(d + 1) + "-" + fileNameOf(document.name) + "-";
        std::vector<uint8_t> expected;
        if (reference >= 0) {
            std::shared_ptr<Loaded> loaded = recordFor(uint32_t(reference));
            if (sizeSlot(device, referenceSlot, width, height) == PrStatus_Ok &&
                    renderSlotWaiting(device, referenceSlot, configs, uint32_t(reference), list, background(*loaded))
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
                r = renderSlotWaiting(device, slot, configs, b, list, background(*loaded));
            std::vector<uint8_t> pixels;
            if (r.status == PrStatus_Ok)
                pixels = readSlot(slot);
            std::string path = stem + configs[b].name + ".png";
            bool ok = r.status == PrStatus_Ok && writePng(path, pixels, width, height);
            std::printf("%s %s: %u commands, %.2f ms", ok ? "wrote" : "FAILED", path.c_str(), r.commands, r.ms);
            if (ok && !expected.empty() && int(b) != reference) {
                std::vector<uint8_t> diff(pixels.size());
                straightenBgra(pixels.data(), width, height, size_t(width) * 4);
                ImageDifference c = compareImages(expected.data(), pixels.data(), width, height,
                    size_t(width) * 4, ImageComparison{}, diff.data());
                std::string diffPath = stem + configs[b].name + "-diff-" + referenceName + ".png";
                ok = writeRgbaPng(diffPath, diff, width, height);
                std::printf(", against %s changed %.3f%% (content %.3f%%), mean error %.3f",
                    referenceName, c.changedRatio * 100, c.contentChangedRatio * 100, c.mae);
            }
            std::printf("\n");
            failures += ok ? 0 : 1;
        }
    }
    prDrawListRelease(list);
    releaseSlot(slot);
    releaseSlot(referenceSlot);
    destroyConfigs(configs);
    prGpuDeviceDestroy(device);
    releaseResources();
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

// A pane's own load of a document, and the text renderers and the renderer configuration that it was loaded for.
struct PaneLoad {
    TextBackends text;
    uint32_t config = 0;
    std::shared_ptr<Loaded> loaded;
};

// A document, loaded with the references that the engine draws, and loaded again by each pane, so that each pane's view
// is independent of the other's.
struct Opened {
    std::vector<Reference> references;              // the document's references (referencesOf)
    std::vector<std::shared_ptr<Loaded>> loaded;    // each reference's source, or null for a browser's page
    PaneLoad panes[2];                              // by pane
};

// The system's browser of a pane that shows a page, created when the pane first shows one. It is shown live over the
// pane and plays on its own. The fields hold the page it has, whether a command is pending, what it was last told, and
// how fast it reports drawing. It was told either to play from the clock's setting or to stand at a time.
struct PaneBrowser {
    PrAppBrowser* PR_NULLABLE browser = nullptr;
    std::string url;
    bool loaded = false;
    bool busy = false;
    bool playing = false;
    uint64_t playedFrom = 0;            // the clock setting that it plays from
    double time = -1;                   // the time that it stands at, negative for none
    std::chrono::steady_clock::time_point asked;
    Rendered figures;

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
    PrAppUserPreferences preferences{};
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
    std::unique_ptr<Comparer> comparer;
    Comparer::Result result;
    Choices choices;
    Figures figures;
    PrRect content{};           // where documents go, in logical pixels
    bool dragging = false;

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

// Returns where the view shows the pane: its rectangle side by side, and otherwise the whole content area.
PrRect paneShownRect(const Viewer& v, uint32_t index) {
    return v.choices.view == View_Compare && v.choices.look == Look_SideBySide ? paneRect(v, index) : v.content;
}

// Returns the comparison's zoom: logical pixels per unit of the document, so that 1 shows the document at its natural
// size regardless of the display's pixel ratio or the density that it was rendered at. If the view is fitted, the zoom
// is the one that fits the pane's render in the rectangle. Otherwise, it is the chosen zoom.
float viewZoom(const Viewer& v, const Slot& slot, PrRect rect) {
    if (!v.choices.fit || slot.texture.id == 0)
        return v.choices.zoom;
    float w = prRectWidth(rect);
    float h = prRectHeight(rect);
    PrUVec2 extent = prGpuTextureExtent(slot.device, slot.texture);
    return std::fmin(w * slot.pixelRatio / float(extent.x), h * slot.pixelRatio / float(extent.y));
}

// Returns where the view places a document of `width` by `height` units at `zoom` in the rectangle: centered and
// panned.
PrRect placed(const Viewer& v, PrRect rect, float width, float height, float zoom) {
    float w = width * zoom;
    float h = height * zoom;
    float cx = (rect.left + rect.right) / 2 + v.choices.panX;
    float cy = (rect.top + rect.bottom) / 2 + v.choices.panY;
    return PrRect{cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2};
}

// Returns the view's zoom for a document of `size` units in the rectangle: the zoom that fits it if the view is fitted,
// and the chosen zoom otherwise.
float zoomFor(const Viewer& v, PrRect rect, PrVec2 size) {
    if (!v.choices.fit || size.x <= 0 || size.y <= 0)
        return v.choices.zoom;
    return std::fmin(prRectWidth(rect) / size.x, prRectHeight(rect) / size.y);
}

// Draws the texture in the rectangle. A render of the view's region goes where it was rendered for, one texel per
// target pixel. A whole document is placed the way the view places it: centered, zoomed and panned, and sampled by
// nearest texel if one texel covers a target pixel or more.
void showTexture(const Viewer& v, const Slot& slot, PrGpuTexture texture, PrRect rect, float pixelRatio) {
    if (texture.id == 0)
        return;
    if (slot.viewed) {
        uiImage(v.ui, texture, rect, slot.at, true);
        return;
    }
    float zoom = viewZoom(v, slot, rect);
    PrUVec2 extent = prGpuTextureExtent(v.device, texture);
    PrRect at = placed(v, rect, float(extent.x) / slot.pixelRatio, float(extent.y) / slot.pixelRatio, zoom);
    uiImage(v.ui, texture, rect, at, zoom * pixelRatio / slot.pixelRatio >= 1);
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
    std::snprintf(stem, sizeof(stem), "%u %u %s %u %u %u %ux%u %d %gx%g %g %g,%g %u %g %u %u", v.choices.collection,
        v.choices.document, prax ? "prax" : "reference", prax ? pane.config : pane.reference,
        uint32_t(pane.text.shaper), uint32_t(pane.text.hinting), source.width, source.height,
        int(v.choices.view == View_Single), double(source.docWidth), double(source.docHeight),
        double(source.pixelRatio), double(source.origin.x), double(source.origin.y),
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
    prAppRequestFrame(v->app);
    return &b;
}

void browserLoaded(void* context, PrStatus status) {
    PaneBrowser* b = answered(context);
    if (b == nullptr)
        return;
    b->loaded = status == PrStatus_Ok;
    if (!b->loaded)
        b->url.clear();
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
        slot.pixelRatio = c->source.pixelRatio;
        slot.viewed = c->source.viewed;
        slot.at = c->source.at;
        pane.publish(index, c->version);
    }
    delete c;
    prAppRequestFrame(v.app);
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
    PrRect region = source.viewed ? source.region : PrRect{0, 0, source.docWidth, source.docHeight};
    prAppBrowserCapture(c->v->browsers[c->index].browser, region, PrUVec2{source.width, source.height}, browserCaptured, c);
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
        prAppBrowserResize(b.browser, PrUVec2{viewport(source.docWidth), viewport(source.docHeight)});
        b.url = source.url;
        b.loaded = false;
        b.busy = true;
        ++b.generation;
        prAppBrowserLoad(b.browser, source.url, browserLoaded, new BrowserCommand{&v, index, b.generation});
        return;
    }
    if (b.busy)
        return;
    prAppBrowserResize(b.browser, PrUVec2{viewport(source.docWidth), viewport(source.docHeight)});
    if (!b.loaded)
        return;
    char script[640];
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
            prAppBrowserEvaluate(b.browser, script, browserPlays, new BrowserCommand{&v, index, b.generation});
        } else if (millisSince(b.asked) >= 1000) {
            b.asked = std::chrono::steady_clock::now();
            b.busy = true;
            prAppBrowserEvaluate(b.browser, "window.prStats ? window.prStats() : null", browserStats,
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
    prAppBrowserEvaluate(b.browser, script, browserStood, c);
}

// Shows the page of pane `index` live where the view shows the pane, placed where its texture would be. Otherwise,
// takes the page out of the window.
void browserShow(Viewer& v, uint32_t index) {
    PaneBrowser& b = v.browsers[index];
    if (b.browser == nullptr)
        return;
    const PaneSource& source = v.panes[index]->source();
    if (source.url.empty() || !paneShown(v, index)) {
        prAppBrowserHide(b.browser);
        return;
    }
    PrRect rect = paneShownRect(v, index);
    float zoom = v.choices.zoom;
    if (v.choices.fit && source.docWidth > 0 && source.docHeight > 0)
        zoom = std::fmin(prRectWidth(rect) / source.docWidth, prRectHeight(rect) / source.docHeight);
    prAppBrowserShow(b.browser, rect, placed(v, rect, source.docWidth, source.docHeight, zoom));
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
        for (const Reference& reference : v.open->references)
            v.open->loaded.push_back(reference.url.empty() ? load(reference.source, TextBackends{}, nullptr) : nullptr);
        // Finds a reference named on the command line that belongs to the document rather than to a renderer.
        for (uint32_t r = 0; v.referenceName != nullptr && r < v.open->references.size(); ++r) {
            if (v.open->references[r].name == v.referenceName) {
                v.choices.panes[1].renderer = Renderer_Reference;
                v.choices.panes[1].reference = r;
                v.choices.view = View_Compare;
            }
        }
    }
    return *v.open;
}

// Returns the document as pane `index` loaded it. Loads it again if the pane's text renderers or renderer configuration
// changed since its last load. The load is presented on the viewer's device if the configuration's renderer renders
// there, and in memory otherwise.
std::shared_ptr<Loaded> paneDocument(Viewer& v, Opened& document, uint32_t index) {
    const PaneChoice& choice = v.choices.panes[index];
    PaneLoad& pane = document.panes[index];
    if (pane.loaded == nullptr || pane.text != choice.text || pane.config != choice.config) {
        const Document& listed = allCollections()[v.openCollection].documents()[v.openDocument];
        PrGpuDevice* device = prDrawRendererIsOnDevice(v.configs[choice.config].renderer) ? v.device : nullptr;
        pane = PaneLoad{choice.text, choice.config, load(listed.source, choice.text, device)};
    }
    return pane.loaded;
}

// Returns the pixel ratio that a comparison measures at, if it has one: the ratio of a pane that shows a raster. For a
// reference that is an image, the ratio is the image's own pixels per unit: the collection's pixel ratio, or else the
// image's pixels over the document's size. For a player of the document's own pixels, the ratio is the display's pixel
// ratio. Returns zero if every pane draws from vectors.
float rasterPixelRatio(const Viewer& v, const Collection& collection, const Opened& document, PrVec2 size,
        float pixelRatio) {
    for (const PaneChoice& pane : v.choices.panes) {
        if (showsPrax(pane, document))
            continue;
        const Reference& reference = document.references[pane.reference];
        const std::shared_ptr<Loaded>& loaded = document.loaded[pane.reference];
        if (reference.source.type == SourceType_Image || reference.source.type == SourceType_Snapshots) {
            if (collection.pixelRatio > 0)
                return collection.pixelRatio;
            PrVec2 pixels = loaded != nullptr ? naturalSize(*loaded) : PrVec2{0, 0};
            return pixels.x > 0 ? pixels.x / size.x : 1;
        }
        if (loaded != nullptr && pixelsOnly(*loaded))
            return pixelRatio;
    }
    return 0;
}

// Sets the source to render the view's region of a document of its size in the pane's rectangle `rect`, at the view's
// zoom, on a display of `pixelRatio` target pixels per logical pixel. The region is the part that the pane shows, in
// whole target pixels, so that the texture is never larger than the pane.
void viewRegion(const Viewer& v, PaneSource& source, PrRect rect, float pixelRatio) {
    PrVec2 size{source.docWidth, source.docHeight};
    float zoom = zoomFor(v, rect, size);
    PrRect at = placed(v, rect, size.x, size.y, zoom);
    float ratio = zoom * pixelRatio;
    // The pane and the document in target pixels, and the whole pixels of the pane that the document covers.
    float left = std::fmax(std::floor(rect.left * pixelRatio), std::floor(at.left * pixelRatio));
    float top = std::fmax(std::floor(rect.top * pixelRatio), std::floor(at.top * pixelRatio));
    float right = std::fmin(std::ceil(rect.right * pixelRatio), std::ceil(at.right * pixelRatio));
    float bottom = std::fmin(std::ceil(rect.bottom * pixelRatio), std::ceil(at.bottom * pixelRatio));
    source.viewed = true;
    source.pixelRatio = ratio;
    source.width = right > left ? uint32_t(right - left) : 0;
    source.height = bottom > top ? uint32_t(bottom - top) : 0;
    source.origin = PrVec2{at.left * pixelRatio - left, at.top * pixelRatio - top};
    source.at = PrRect{left / pixelRatio, top / pixelRatio, right / pixelRatio, bottom / pixelRatio};
    source.region = PrRect{
        (left - at.left * pixelRatio) / ratio, (top - at.top * pixelRatio) / ratio,
        (right - at.left * pixelRatio) / ratio, (bottom - at.top * pixelRatio) / ratio,
    };
}

// Gives pane `index` what its choice says that it shows: the document rendered by Prax, or one of its references. In
// the single view, the document lays out for the pane. In comparison, it lays out at its natural size. Each pane
// renders the view's region at the view's zoom (viewRegion), except in a comparison with a raster, which renders the
// whole document at the raster's pixels per unit. In comparison, the pane's renders go to the comparer while the clock
// stands.
void showPane(Viewer& v, uint32_t index, float pixelRatio) {
    const Collection& collection = allCollections()[v.choices.collection];
    const Document& listed = collection.documents()[v.choices.document];
    Opened& document = shownDocument(v);
    const PaneChoice& choice = v.choices.panes[index];
    bool prax = showsPrax(choice, document);
    // A reference's pane reads the document's size and background from the first pane's load.
    std::shared_ptr<Loaded> engine = paneDocument(v, document, prax ? index : 0);
    bool single = v.choices.view == View_Single;
    PrRect r = paneRect(v, index);
    PaneSource source;
    source.docWidth = prRectWidth(r);
    source.docHeight = prRectHeight(r);
    float raster = 0;
    if (!single) {
        // The listing's size, or else the source's own, or else the collection's.
        PrVec2 natural = naturalSize(*engine);
        float width = listed.width > 0 ? listed.width : natural.x;
        float height = listed.height > 0 ? listed.height : natural.y;
        source.docWidth = width > 0 ? width : collection.width;
        source.docHeight = height > 0 ? height : collection.height;
        raster = rasterPixelRatio(v, collection, document, PrVec2{source.docWidth, source.docHeight}, pixelRatio);
    }
    if (raster > 0) {
        source.pixelRatio = raster;
        source.width = uint32_t(std::lround(source.docWidth * raster));
        source.height = uint32_t(std::lround(source.docHeight * raster));
    } else {
        viewRegion(v, source, r, pixelRatio);
    }
    if (source.width == 0 || source.height == 0) {
        v.panes[index]->show(PaneSource{});
        return;
    }
    PrPixelFormat format = v.choices.hdr ? PrPixelFormat_RgbaF16 : PrPixelFormat_Bgra8;
    source.output = PaneOutput{format, v.display, v.preferences};
    source.stem = paneStem(v, choice, document, source);
    source.background = background(*engine);
    source.backToBack = v.choices.backToBack;
    // A reference that the engine draws draws with the cpu renderer, which is the first.
    source.config = prax ? choice.config : 0;
    if (prax) {
        source.loaded = engine;
    } else {
        source.loaded = document.loaded[choice.reference];
        source.url = document.references[choice.reference].url;
    }
    if (!single && !v.clock.running()) {
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

void start(void* context, PrApp* app, const PrAppFrame*) {
    Viewer& v = *static_cast<Viewer*>(context);
    PrAppSurface surface{};
    PrGpuSwapChainDesc desc = swapChainDesc(false);
    if (prGpuDeviceCreate(PrGpuBackend_Default, nullptr, &v.device) != PrStatus_Ok ||
            prAppSurface(app, &surface) != PrStatus_Ok ||
            prGpuSwapChainCreate(v.device, &surface, &desc, &v.swapChain) != PrStatus_Ok) {
        std::fprintf(stderr, "viewer: no GPU device to present with\n");
        prAppStop(app);
        return;
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
    if (v.configName != nullptr && indexOf(v.configs, v.configName) >= 0)
        v.choices.panes[0].config = uint32_t(indexOf(v.configs, v.configName));
    // The second pane shows the document's first reference, unless the command line names a renderer for it. It sets
    // its text as the first does.
    v.choices.panes[1].renderer = Renderer_Reference;
    v.choices.panes[1].text = v.choices.panes[0].text;
    if (v.referenceName != nullptr && indexOf(v.configs, v.referenceName) >= 0) {
        v.choices.panes[1].renderer = Renderer_Prax;
        v.choices.panes[1].config = uint32_t(indexOf(v.configs, v.referenceName));
        v.choices.view = View_Compare;
    }
    for (size_t b = 0; b < v.configs.size(); ++b)
        v.configLocks.push_back(std::make_unique<std::mutex>());
    v.app = app;
    PaneContext shared{
        app,      v.device,      &v.configs,    &v.configLocks,
        &v.clock, &v.framesDone, &v.framesMutex, &v.framesDoneChanged,
    };
    for (std::unique_ptr<Pane>& pane : v.panes)
        pane = std::make_unique<Pane>(shared, true);
    v.ui = uiCreate(v.device, app);
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

// Returns the panel's output with that of the page that holds the keyboard, where its pane shows it.
PrAppOutput pageOutput(Viewer& v, const PrAppOutput& panel, double time) {
    uint32_t index = v.pagePane;
    if (!v.pageKeys)
        return panel;
    const PaneSource& source = v.panes[index]->source();
    if (source.loaded == nullptr || !takesInput(*source.loaded) || !paneShown(v, index))
        return panel;
    PrRect rect = paneShownRect(v, index);
    float zoom = zoomFor(v, rect, PrVec2{source.docWidth, source.docHeight});
    PrRect at = placed(v, rect, source.docWidth, source.docHeight, zoom);
    PrAppOutput page = prAppOutputPlaced(output(*source.loaded, v.app, time), PrVec2{at.left, at.top}, zoom);
    // A caret that blinks changes the page's render.
    if (page.wantsFrame)
        v.panes[index]->touch();
    return prAppOutputMerge(panel, page, inside(rect, v.pointerX, v.pointerY));
}

PrAppOutput frame(void* context, PrApp* app, const PrAppFrame* frame) {
    Viewer& v = *static_cast<Viewer*>(context);
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
        PrAppSurface surface{};
        PrGpuSwapChain* swapChain = nullptr;
        PrGpuSwapChainDesc desc = swapChainDesc(v.choices.hdr);
        if (prAppSurface(app, &surface) == PrStatus_Ok &&
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
            PrAppBrowserDesc desc{
                .viewport = PrUVec2{uint32_t(std::lround(page.docWidth)), uint32_t(std::lround(page.docHeight))},
            };
            prAppBrowserCreate(app, &desc, &v.browsers[i].browser);
        }
        if (live[i])
            browserNext(v, i, running);
        browserShow(v, i);
        // Under the page, its capture while the clock stands. The capture shows the page as it stands, and is what a
        // snapshot of the frame shows. Nothing is under the page while it plays.
        liveShown[i] = live[i] && running && paneShown(v, i);
    }

    ++v.frames;
    const Slot* slots[2] = {&v.panes[0]->take(v.frames, format), &v.panes[1]->take(v.frames, format)};
    // Panes that the running clock drives each show their own time, so they are not compared.
    v.figures.playing = compare && running;
    if (compare && !running) {
        updateDifference(v, slots[0]->shown, slots[1]->shown);
    } else {
        v.figures.compared = false;
        v.figures.comparing = false;
    }
    for (uint32_t i = 0; i < 2; ++i)
        v.figures.panes[i] = live[i] && running ? v.browsers[i].figures : slots[i]->rendered;

    PrGpuCommands* commands = nullptr;
    if (!prGpuCommandsBegin(v.device, nullptr, &commands)) {
        // The viewer shares its device with every pane's thread, and stops instead of making it again.
        std::fprintf(stderr, "viewer: the GPU device was lost\n");
        prAppStop(app);
        return {};
    }
    if (compare && v.choices.look == Look_Difference) {
        if (v.figures.compared)
            showTexture(v, *slots[0], v.difference, v.content, pixelRatio);
    } else {
        for (uint32_t i = 0; i < paneCount; ++i)
            if (paneShown(v, i) && !liveShown[i])
                showTexture(v, *slots[i], slots[i]->texture, paneShownRect(v, i), pixelRatio);
    }
    const Slot& first = *slots[0];
    if (first.viewed || first.texture.id == 0)
        v.figures.zoom =
            zoomFor(v, paneRect(v, 0), PrVec2{v.panes[0]->source().docWidth, v.panes[0]->source().docHeight});
    else
        v.figures.zoom = viewZoom(v, first, paneRect(v, 0));
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
    PrGpuTexture drawable{};
    PrStatus acquired = prGpuSwapChainAcquire(v.swapChain, PrUVec2{width, height}, &drawable);
    if (acquired == PrStatus_Ok) {
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
    bool settled = !running && v.panes[0]->settled() && (!compare || v.panes[1]->settled()) && !v.browsers[0].busy &&
        !v.browsers[1].busy;
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

// Zooms both renders by `factor` about the point, which stays fixed in its pane.
void zoomAbout(Viewer& v, float factor, float x, float y) {
    PrRect pane = inside(paneRect(v, 1), x, y) ? paneRect(v, 1) : paneRect(v, 0);
    float cx = x - (pane.left + pane.right) / 2;
    float cy = y - (pane.top + pane.bottom) / 2;
    if (v.choices.fit) {
        v.choices.fit = false;
        v.choices.zoom = v.figures.zoom;
    }
    float zoom = std::fmin(std::fmax(v.choices.zoom * factor, 0.05f), 64.0f);
    float ratio = zoom / v.choices.zoom;
    v.choices.panX = cx - (cx - v.choices.panX) * ratio;
    v.choices.panY = cy - (cy - v.choices.panY) * ratio;
    v.choices.zoom = zoom;
}

void keyDown(Viewer& v, PrApp* app, PrAppKey key) {
    Choices& c = v.choices;
    PaneChoice& first = c.panes[0];
    uint32_t count = uint32_t(allCollections()[c.collection].documents().size());
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
    } else if (key == '0' || key == '=') {
        c.fit = key == '0';
        c.zoom = 1;
        c.panX = 0;
        c.panY = 0;
    }
}

// Returns the engine's render of a page in pane `index` if it takes input and the view shows it, and sets `out` to
// where `window`, in the window's logical pixels, is in the document's units. Returns null for a point that is not on
// the document, unless the page holds the pointer.
Loaded* PR_NULLABLE pageAt(Viewer& v, uint32_t index, PrVec2 window, PrVec2* out) {
    const PaneSource& source = v.panes[index]->source();
    if (source.loaded == nullptr || !takesInput(*source.loaded) || source.docWidth <= 0 || source.docHeight <= 0 ||
            !paneShown(v, index))
        return nullptr;
    PrRect rect = paneShownRect(v, index);
    float zoom = zoomFor(v, rect, PrVec2{source.docWidth, source.docHeight});
    PrRect at = placed(v, rect, source.docWidth, source.docHeight, zoom);
    *out = PrVec2{(window.x - at.left) / zoom, (window.y - at.top) / zoom};
    bool onPage = inside(rect, window.x, window.y) && out->x >= 0 && out->y >= 0 && out->x < source.docWidth &&
        out->y < source.docHeight;
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
            PrVec2 window = event.kind == PrAppEventKind_Scroll ? PrVec2{v.pointerX, v.pointerY} : event.position;
            PrAppEvent local = event;
            Loaded* page = nullptr;
            uint32_t index = 0;
            for (uint32_t i = 0; i < 2 && page == nullptr; ++i) {
                index = v.pagePressed ? v.pagePane : i;
                page = pageAt(v, index, window, &local.position);
            }
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
            return true;
        }
        case PrAppEventKind_KeyDown:
        case PrAppEventKind_KeyUp:
        case PrAppEventKind_Text:
        case PrAppEventKind_Clipboard:
        case PrAppEventKind_CompositionUpdate:
        case PrAppEventKind_CompositionEnd: {
            const PaneSource& source = v.panes[v.pagePane]->source();
            if (!v.pageKeys || (event.modifiers & PrAppModifierFlag_Super) != 0 || source.loaded == nullptr ||
                    !takesInput(*source.loaded))
                return false;
            input(*source.loaded, event);
            v.panes[v.pagePane]->touch();
            return true;
        }
        default:
            return false;
    }
}

void event(void* context, PrApp* app, const PrAppEvent* event) {
    Viewer& v = *static_cast<Viewer*>(context);
    if (v.ui == nullptr)
        return;
    prAppRequestFrame(app);
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
    if (!taken && event->kind == PrAppEventKind_PointerMove) {
        v.pointerX = event->position.x;
        v.pointerY = event->position.y;
    }
    if (!taken && !key && !v.dragging && toPage(v, *event))
        return;
    switch (event->kind) {
        case PrAppEventKind_KeyDown:
            if (!taken)
                keyDown(v, app, event->key);
            break;
        case PrAppEventKind_PointerDown:
            v.dragging = !taken && event->button == 0 && inside(v.content, event->position.x, event->position.y);
            v.pointerX = event->position.x;
            v.pointerY = event->position.y;
            break;
        case PrAppEventKind_PointerUp:
            v.dragging = false;
            break;
        case PrAppEventKind_PointerMove:
            if (v.dragging) {
                v.choices.panX += event->position.x - v.pointerX;
                v.choices.panY += event->position.y - v.pointerY;
            }
            v.pointerX = event->position.x;
            v.pointerY = event->position.y;
            break;
        case PrAppEventKind_Scroll:
            if (!taken && inside(v.content, v.pointerX, v.pointerY)) {
                PrVec2 pixels = prAppEventScrollPixels(event, kPrAppScrollLineHeight);
                zoomAbout(v, std::exp(-pixels.y * 0.01f), v.pointerX, v.pointerY);
            }
            break;
        default:
            break;
    }
}

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
        prAppBrowserDestroy(b.browser);
        b.browser = nullptr;
    }
    v.comparer.reset();
    prDrawDisplayDestroy(v.displayShader);
    prGpuTextureDestroy(v.device, v.difference);
    prGpuTextureDestroy(v.device, v.frame);
    uiDestroy(v.ui);
    // The documents are released before the device, because a page's canvases keep their bitmaps on it.
    v.open.reset();
    destroyConfigs(v.configs);
    prGpuSwapChainDestroy(v.swapChain);
    prGpuDeviceDestroy(v.device);
    releaseResources();
}

} // namespace

int main(int argc, char** argv) {
    prCrashHandler();
    const char* directory = nullptr;
    const char* which = nullptr;
    float pixelRatio = 1;
    Viewer viewer;
    auto usage = [](FILE* to) {
        std::fprintf(to, "usage: viewer [--document <which>] [--render <directory>] [--pixel-ratio <pixels>]"
            " [--renderer <name>] [--shaper <name>] [--hinting <name>] [--reference <name>]"
            " [--look side|flip|difference] [--snapshot <path>] [--tour <passes>] [--collection <word>] [--zoom <percent>]"
            " [--time <seconds>] [--rendering paced|back-to-back] [--present sdr|hdr] [--wpt-server <http port> <https port>]\n");
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
        } else if (std::strcmp(argv[i], "--zoom") == 0) {
            viewer.choices.fit = false;
            viewer.choices.zoom = std::strtof(value, nullptr) / 100;
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
    if (directory != nullptr)
        return renderAll(directory, collections[viewer.choices.collection], which, viewer.referenceName,
            viewer.choices.panes[0].text, viewer.choices.time, pixelRatio > 0 ? pixelRatio : 1);
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
    PrAppDesc appDesc{.title = "Prax draw viewer", .extent = {uint32_t(kWindowWidth), uint32_t(kWindowHeight)}};
    PrApp* app = prAppCreate(PrAppBackend_Default, &appDesc);
    PrAppHandler handler{&viewer, start, frame, event, shutdown};
    prAppRun(app, &handler);
    prAppDestroy(app);
    return 0;
}
