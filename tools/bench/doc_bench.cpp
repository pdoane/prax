// The document benchmark: measures what a long document costs to load, scroll, hover, navigate and resize when it is
// presented the way a browser presents it, through a view and a compositor (pr/html_view.h), and prints the rows as
// JSON.
//
// ./dev.sh bench long-doc runs this and records its rows. The page loads in one piece and parses in the event loop's
// slices. Frames are paced at 60 per second, like a window's frames. Each frame runs the page's tasks for a while and
// presents through the platform's GPU into a texture the size of the window. Meanwhile, whenever the layout thread
// finishes a job, the page commits the scene that the job recorded to a compositor and starts the next job. Each job
// takes about as long as its budget. No scripts run.
//
// A measure is a number of samples. Its row gives what a typical sample cost on the process's CPUs, as harnessCounters
// counts it, which is comparable from run to run. The row also gives the median and the worst time that a sample took,
// which show what a user waits for but vary with the machine. A frame after the load waits for its job, so that each
// sample's cost is its own. The rows that time the load as it paces with the frames have only the times:
//   - load: the whole load's cost and its end; the first frame presented; the longest time the page's frame took on
//     its thread; the longest time the compositor's frame took; the longest time a job took to be seen done, which a
//     change made meanwhile, such as a hover, waits for; the total time the layout thread spent on the jobs; and the
//     hovers made during the load, each timed to the committed scene that shows it
//   - scroll: frames presented across the document after the load, visiting each position once and then again
//   - hover: hit testing a point, and a frame after the hovered element changes
//   - navigate: a click on a link to a fragment of the document, through the frame that shows the target
//   - geometry: a script's read of the bounding rectangle of an element at the end of the document, after the script
//     changed the paint or the layout of an element in the middle, or the viewport's scroll
//   - restyle: a change to the root's font size, which restyles every element, through a read of a computed style at
//     the end of the document, as a script that waits on a restyle of the document would do
//   - resize: a frame in a viewport of another width, and back
//
// `--job-budget` sets about how long each job takes on the layout thread, in milliseconds (PrHtmlViewDesc::jobBudget).
// `--restyle-threads` sets the number of threads that a restyle divides its work among
// (PrDomAgentOptions::restyleThreads).
//
// Usage: doc_bench <page.html> [--pixel-ratio <pixels per px>] [--job-budget MS] [--restyle-threads N]

#include "pr/assert.h"
#include "pr/compress_builtin.h"
#include "pr/css_presentation.h"
#include "pr/draw_fragment.h"
#include "pr/draw_list.h"
#include "pr/draw_renderer.h"
#include "pr/fetch.h"
#include "pr/font_bundle.h"
#include "pr/font_set.h"
#include "pr/gpu.h"
#include "pr/html.h"
#include "pr/html_page.h"
#include "pr/html_view.h"
#include "pr/image_builtin.h"
#include "pr/layout_compositor.h"
#include "pr/layout_query.h"
#include "pr/layout_style.h"
#include "pr/layout_tree.h"
#include "pr/net_null.h"
#include "pr/task.h"
#include "pr/text_harfbuzz.h"
#include "pr/thread.h"

#include "tools/harness/harness.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

using namespace pr;
using namespace harness;

namespace {

// The time in milliseconds that the device has spent executing the frames presented so far.
double gpuMillis = 0;

// The window the page presents in, in CSS pixels, and the frames' pacing.
constexpr float kWidth = 1024;
constexpr float kHeight = 768;
constexpr double kFrameMillis = 1000.0 / 60;

// How long a frame runs the page's tasks, which is also about how long the layout thread spends on a job. Both match
// the browser example.
constexpr double kTaskBudget = 8;
constexpr double kJobBudget = 10;

// The longest time that a load may take before the benchmark gives up on it.
constexpr double kLoadLimit = 120000;

// The number of frames between the load's hovers, once the page presents.
constexpr uint32_t kLoadHoverFrames = 12;

// The number of samples of each measure after the load. A sample's counts barely vary, so a few samples cover what the
// measure visits.
constexpr uint32_t kScrollSamples = 40;
constexpr uint32_t kHoverSamples = 50;
constexpr uint32_t kLinkSamples = 10;
constexpr uint32_t kGeometrySamples = 10;
constexpr uint32_t kRestyleSamples = 2;
constexpr uint32_t kResizeSamples = 2;

double now() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

double clockFn(void* PR_NULLABLE) {
    return now();
}

std::string readFile(const std::filesystem::path& path, bool* ok) {
    std::ifstream file(path, std::ios::binary);
    std::stringstream text;
    text << file.rdbuf();
    *ok = file.good() || file.eof();
    return text.str();
}

// The samples of a measure: the time that each took, and what each cost.
struct Measure {
    std::vector<double> times;
    std::vector<HarnessCounters> costs;

    // Takes a sample of `run`.
    template <typename Run>
    void sample(Run run) {
        HarnessCounters from = harnessCounters();
        from.gpu = gpuMillis;
        run();
        HarnessCounters to = harnessCounters();
        to.gpu = gpuMillis;
        add(harnessSpent(from, to));
    }

    void add(const HarnessCounters& cost) {
        times.push_back(cost.millis);
        costs.push_back(cost);
    }

    // Returns the mean cost of the samples, excluding the fifth with the most instructions and the fifth with the
    // least. A one-off cost in one sample, such as filling a cache, does not move it.
    HarnessCounters typical() const {
        std::vector<HarnessCounters> sorted = costs;
        std::sort(sorted.begin(), sorted.end(),
            [](const HarnessCounters& a, const HarnessCounters& b) { return a.instructions < b.instructions; });
        size_t trim = sorted.size() / 5;
        HarnessCounters sum{};
        for (size_t i = trim; i < sorted.size() - trim; ++i)
            sum = harnessSum(sum, sorted[i]);
        return harnessEach(sum, double(sorted.size() - 2 * trim));
    }
};

struct Bench {
    PrFontSet* fonts = nullptr;
    PrFontSystem* system = nullptr;
    PrTextShaper* shaper = nullptr;
    PrTextFont* font = nullptr;     // the page's default text font
    PrCompressCodec* compress = nullptr;
    PrImageCodec* images = nullptr;  // decodes the page's images
    PrDomAgent* agent = nullptr;

    // The loop of the page's fetches, which the page's thread runs before it looks for a task, and the built-in
    // fetcher, which reads the page's file: URLs.
    PrNet* net = nullptr;
    PrFetchNetwork* fetcher = nullptr;

    PrHtmlPage* page = nullptr;
    PrHtmlView* view = nullptr;
    PrLayoutCompositor* compositor = nullptr;
    PrGpuDevice* device = nullptr;
    PrDrawRenderer* renderer = nullptr;
    PrDrawFrame* frame = nullptr;
    PrGpuTexture target{};
    uint32_t width = 0;
    uint32_t height = 0;
    float pixelRatio = 2;           // target pixels per CSS pixel
    double jobBudget = kJobBudget;
    uint32_t restyleThreads = 0;    // in the form that PrDomAgentOptions takes
    PrTaskPool* workers = nullptr;  // the threads that share the agent's work with the page's thread, such as restyles
    double presented = 0;           // how long the last present took, up to the device finishing it
    std::mutex mutex;
    std::condition_variable wake;   // signaled by the layout thread when a job finishes
    bool jobDone = false;           // guarded by `mutex`
    double jobStarted = 0;          // when the running job started, or 0; guarded by `mutex`
    double jobsTook = 0;            // total time the layout thread spent on the jobs so far; guarded by `mutex`
};

// Wakes the page's thread when the layout thread finishes its jobs.
void jobDone(void* PR_NULLABLE context) {
    Bench& b = *static_cast<Bench*>(context);
    std::lock_guard<std::mutex> lock(b.mutex);
    b.jobDone = true;
    if (b.jobStarted != 0)
        b.jobsTook += now() - b.jobStarted;
    b.jobStarted = 0;
    b.wake.notify_one();
}

// Waits until `until` on the clock, or until the layout thread finishes its jobs. Returns true if the jobs finished.
bool waitForJob(Bench& b, double until) {
    std::unique_lock<std::mutex> lock(b.mutex);
    double rest = until - now();
    if (rest > 0)
        b.wake.wait_for(lock, std::chrono::duration<double, std::milli>(rest), [&] { return b.jobDone; });
    bool done = b.jobDone;
    b.jobDone = false;
    return done;
}

// Presents a frame into the target and waits for the device, as a window's present would before the next frame. The
// pass clears to the frame's canvas color, which fills the canvas in place of the list.
void present(void* PR_NULLABLE context, PrDrawList* list, PrColor16 canvasColor) {
    Bench& b = *static_cast<Bench*>(context);
    double started = now();
    PrGpuCommands* commands = nullptr;
    if (!prGpuCommandsBegin(b.device, nullptr, &commands))
        return;
    PrDrawTarget target{.extent = {b.width, b.height}, .format = PrPixelFormat_Bgra8, .commands = commands};
    if (prDrawFramePrepare(b.frame, list, &target) == PrStatus_Ok) {
        PrColor canvas = prColorFromColor16(canvasColor);
        PrColor clear{canvas.r * canvas.a, canvas.g * canvas.a, canvas.b * canvas.a, canvas.a};
        prGpuCommandsTextureBarrier(commands, b.target, PrGpuSubresources{}, PrGpuAccessFlag_ShaderRead,
            PrGpuAccessFlag_ColorAttachment);
        PrGpuPassDesc pass = prGpuPassDescColor(b.target, PrGpuLoad_Clear, clear);
        PrGpuPass* encoder = prGpuPassBegin(commands, &pass);
        prDrawFrameRender(b.frame, encoder, PrIVec2{0, 0});
        prGpuPassEnd(encoder);
        prGpuCommandsTextureBarrier(commands, b.target, PrGpuSubresources{},
            PrGpuAccessFlag_ColorAttachment, PrGpuAccessFlag_ShaderRead);
    }
    gpuMillis += prGpuCommandsSubmitAndWait(commands) * 1000;
    b.presented = now() - started;
}

bool setUp(Bench& b) {
    b.fonts = prFontSetCreate(nullptr);
    PrFontBundle bundle = PrFontBundle_Complete;
    prFontBundleInstall(b.fonts, &bundle, 1, PR_FONT_BUNDLE_DIR);
    b.system = prFontSystemCreate(PrFontSystemBackend_Default);
    prFontSetSetSystem(b.fonts, b.system);
    b.shaper = prTextHarfBuzzCreate();
    b.compress = prCompressBuiltinCreate();
    b.images = prImageBuiltinCreate(b.compress);
    PrTextFontDesc defaultFontDesc{.set = b.fonts, .style = prFontStyleDefault()};
    b.font = prTextFontCreate(&defaultFontDesc);

    if (prGpuDeviceCreate(PrGpuBackend_Default, nullptr, &b.device) != PrStatus_Ok ||
            prDrawFragmentCreate(b.device, nullptr, &b.renderer) != PrStatus_Ok)
        return false;
    b.frame = prDrawFrameCreate(b.renderer);
    b.width = uint32_t(kWidth * b.pixelRatio);
    b.height = uint32_t(kHeight * b.pixelRatio);
    PrGpuTextureDesc textureDesc = prGpuTextureDesc2d(PrGpuFormat_Bgra8, PrUVec2{b.width, b.height},
        PrGpuTextureUsageFlag_Sampled | PrGpuTextureUsageFlag_RenderTarget);
    return prGpuTextureCreate(b.device, &textureDesc, &b.target) == PrStatus_Ok;
}

void tearDown(Bench& b) {
    prHtmlViewDestroy(b.view);
    prLayoutCompositorDestroy(b.compositor);
    prHtmlPageDestroy(b.page);
    prNetNullRun(b.net);
    prFetchNetworkDestroy(b.fetcher);
    prDomAgentDestroy(b.agent);
    prTaskPoolDestroy(b.workers);
    prNetDestroy(b.net);
    prTextFontRelease(b.font);
    prImageCodecDestroy(b.images);
    prCompressCodecDestroy(b.compress);
    prTextShaperDestroy(b.shaper);
    prFontSetDestroy(b.fonts);
    prFontSystemDestroy(b.system);
    if (b.device != nullptr)
        prGpuTextureDestroy(b.device, b.target);
    prDrawFrameDestroy(b.frame);
    prDrawRendererDestroy(b.renderer);
    prGpuDeviceDestroy(b.device);
}

// Returns true if a job's layout changed what the page shows.
bool changedBy(const PrHtmlViewUpdate& update) {
    const PrLayoutTreeStats& laid = update.layout;
    return update.committed && (laid.blocksLaidOut != 0 || laid.linesLaidOut != 0 || laid.layersRecorded != 0);
}

// Runs a frame after the load: brings the rendering up to date in a viewport of `extent`, waits for the job that lays
// it out, then commits the job's scene and presents it.
void frameNow(Bench& b, PrVec2 extent) {
    prHtmlViewSetViewport(b.view, extent);
    prHtmlViewUpdate(b.view);
    prHtmlViewWait(b.view);
    prHtmlViewUpdate(b.view);
    prLayoutCompositorFrame(b.compositor);
}

// Prints a row: the comma before it unless it is the first row, its name, the time, the fields in `rest`, and the cost
// of a sample of `measure` if there is one.
void row(bool& first, const char* name, double time, const char* rest, const Measure* PR_NULLABLE measure) {
    PrTextBuffer out{};
    harnessAppendFormat(&out, "%s\n  {\"name\": \"%s\", \"time\": %.4f%s", first ? "" : ",", name, time, rest);
    if (measure != nullptr && !measure->costs.empty()) {
        HarnessCounts counts{};
        harnessCountsAdd(&counts, "time", measure->typical());
        harnessCountsWrite(&counts, &out);
    }
    prTextBufferAppend(&out, "}");
    fwrite(out.data, 1, out.size, stdout);
    prTextBufferReset(&out);
    first = false;
}

// Prints a measure's row: the median and the worst time of its samples, and what a sample cost.
void row(bool& first, const char* name, const Measure& measure) {
    std::vector<double> sorted = measure.times;
    std::sort(sorted.begin(), sorted.end());
    double median = sorted.empty() ? 0 : sorted[sorted.size() / 2];
    double worst = sorted.empty() ? 0 : sorted.back();
    char rest[96];
    std::snprintf(rest, sizeof(rest), ", \"worst\": %.4f, \"samples\": %zu", worst, sorted.size());
    row(first, name, median, rest, &measure);
}

// Prints a row with only a time.
void row(bool& first, const char* name, double time) {
    row(first, name, time, "", nullptr);
}

} // namespace

int main(int argc, char** argv) {
    prCrashHandler();
    const char* page = nullptr;
    Bench b;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--pixel-ratio") == 0 && i + 1 < argc)
            b.pixelRatio = std::strtof(argv[++i], nullptr);
        else if (std::strcmp(argv[i], "--job-budget") == 0 && i + 1 < argc)
            b.jobBudget = std::strtod(argv[++i], nullptr);
        else if (std::strcmp(argv[i], "--restyle-threads") == 0 && i + 1 < argc)
            b.restyleThreads = uint32_t(std::strtoul(argv[++i], nullptr, 10));
        else
            page = argv[i];
    }
    if (page == nullptr) {
        fprintf(stderr, "usage: doc_bench <page.html> [--pixel-ratio <pixels per px>] [--job-budget MS] [--restyle-threads N]\n");
        return 2;
    }
    bool ok = false;
    std::string source = readFile(page, &ok);
    if (!ok || !setUp(b)) {
        fprintf(stderr, "doc_bench: cannot read %s, or no GPU device\n", page);
        return 1;
    }
    uint32_t threads = b.restyleThreads != 0 ? b.restyleThreads : prThreadHardwareCount();
    if (threads > 1)
        b.workers = prTaskPoolCreate(PrTaskPoolBackend_Default, threads - 1);
    PrDomAgentOptions agentOptions = prDomAgentOptionsDefault();
    agentOptions.restyleThreads = b.restyleThreads;
    agentOptions.restylePool = b.workers;
    b.agent = prDomAgentCreate(&agentOptions);
    prDomAgentSetLayoutDone(b.agent, jobDone, &b);
    b.net = prNetNullCreate();
    PrFetchNetworkDesc fetcherDesc{.net = b.net, .compress = b.compress};
    b.fetcher = prFetchNetworkCreate(PrFetchNetworkBackend_Default, &fetcherDesc);
    PrHtmlHost host = prHtmlHostDefault();
    host.context = &b;
    host.fetcher = b.fetcher;
    host.agent = b.agent;
    host.font = b.font;
    host.shaper = b.shaper;
    host.images = b.images;
    host.compress = b.compress;
    host.now = clockFn;
    b.page = prHtmlPageCreate(&host);
    PrLayoutCompositorDesc compositorDesc{.context = &b, .now = clockFn, .present = present, .canvasFilled = true};
    b.compositor = prLayoutCompositorCreate(&compositorDesc);
    PrHtmlViewDesc viewDesc{.page = b.page, .compositor = b.compositor, .jobBudget = b.jobBudget};
    b.view = prHtmlViewCreate(&viewDesc);
    std::string url = "file://" + std::filesystem::absolute(page).lexically_normal().string();
    PrVec2 extent{kWidth, kHeight};
    PrAppDisplay display = prAppDisplayDefault();
    display.pixelRatio = b.pixelRatio;
    prHtmlViewSetDisplay(b.view, &display);
    prHtmlViewSetViewport(b.view, extent);

    // The load, frame by frame.
    HarnessCounters loadFrom = harnessCounters();
    double started = now();
    prHtmlPageBeginLoad(b.page, url, nullptr);
    prHtmlPageAppendLoad(b.page, source);
    prHtmlPageEndLoad(b.page);
    PrCssPresentation* presentation = prHtmlPagePresentation(b.page);
    double firstFrame = 0;
    double lastChange = 0;
    double longestPage = 0;
    double longestCompositor = 0;
    double longestRender = 0;       // the longest time a job took to be seen done; a change made meanwhile waits for it
    double jobStarted = 0;          // when the running job started, or 0
    uint32_t jobs = 0;
    bool settled = false;
    PrDomDocument* document = prHtmlWindowDocument(prHtmlPageWindow(b.page));
    uint32_t seed = 1;
    auto random = [&](float range) {
        seed = seed * 1103515245u + 12345u;
        return float((seed >> 8) % 10000) / 10000.0f * range;
    };
    // A hover during the load: when it was made, and the jobs started before it, whose scenes do not show it.
    Measure loadHovers;
    double hoverMade = 0;
    uint32_t hoverJobs = 0;
    uint32_t frames = 0;  // the frames presented since the first, which pace the load's hovers

    // Runs the loop of the page's fetches, and returns true if a task of the page can run now.
    auto taskReady = [&] {
        prNetNullRun(b.net);
        return prDomAgentNextTaskDelay(b.agent) == 0;
    };

    // Commits the scene of the finished job, if there is one, and starts the next job if there is work for it. Returns
    // true if the committed job changed what the page shows.
    auto service = [&] {
        if (jobStarted != 0 && !prCssPresentationIsBusy(presentation)) {
            longestRender = std::max(longestRender, now() - jobStarted);
            jobStarted = 0;
        }
        {
            // The job that an update starts is timed from before the update, because the layout thread may finish the
            // job before the update returns.
            std::lock_guard<std::mutex> lock(b.mutex);
            b.jobStarted = now();
        }
        PrHtmlViewUpdate update = prHtmlViewUpdate(b.view);
        bool changed = changedBy(update);
        if (changed)
            lastChange = now();
        // The committed scene is the last job's, and shows a hover made before that job started.
        if (update.committed && hoverMade != 0 && jobs > hoverJobs) {
            loadHovers.times.push_back(now() - hoverMade);
            hoverMade = 0;
        }
        if (update.started) {
            jobStarted = now();
            ++jobs;
        } else {
            std::lock_guard<std::mutex> lock(b.mutex);
            b.jobStarted = 0;
        }
        return changed;
    };
    while (!settled && now() - started < kLoadLimit) {
        double frameStart = now();
        // The pointer moves over the page as it loads, as a user's would. The hit test waits for the running job.
        if (firstFrame != 0 && hoverMade == 0 && ++frames % kLoadHoverFrames == 0) {
            hoverMade = now();
            PrAppEvent move{
                .kind = PrAppEventKind_PointerMove,
                .pointer = {.position = PrVec2{random(kWidth), random(kHeight)}},
            };
            prHtmlViewEvent(b.view, &move, 0);
            hoverJobs = jobs;
        }
        bool changed = service();
        bool loaded = prHtmlDocumentReadyState(prHtmlWindowDocument(prHtmlPageWindow(b.page))) ==
            PrHtmlDocumentReadyState_Complete;
        settled = jobs > 1 && !changed && loaded && !taskReady() && prCssPresentationIsCurrent(presentation);
        // The page's tasks run for the frame's budget while the layout thread lays out. The layout thread takes the
        // next job as soon as it is done with one.
        double deadline = frameStart + kTaskBudget;
        while (now() < deadline && taskReady() && prDomAgentRunTask(b.agent))
            service();
        double pageDone = now();
        longestPage = std::max(longestPage, pageDone - frameStart);
        prLayoutCompositorFrame(b.compositor);
        double compositorDone = now();
        longestCompositor = std::max(longestCompositor, compositorDone - pageDone);
        if (firstFrame == 0 && prLayoutCompositorStats(b.compositor).frames != 0)
            firstFrame = compositorDone - started;
        // Until the next frame, the page starts the next job as soon as the layout thread is done with one.
        while (waitForJob(b, frameStart + kFrameMillis))
            service();
    }
    prDomAgentSetLayoutDone(b.agent, nullptr, nullptr);
    prHtmlViewWait(b.view);
    prHtmlViewUpdate(b.view);
    Measure loaded;
    loaded.add(harnessSpent(loadFrom, harnessCounters()));
    bool first = true;
    printf("[");
    row(first, "load", lastChange - started, "", &loaded);
    row(first, "load first frame", firstFrame);
    row(first, "load longest page frame", longestPage);
    row(first, "load longest compositor frame", longestCompositor);
    row(first, "load longest presentation", longestRender);
    row(first, "load presentation busy", b.jobsTook);
    row(first, "load hover", loadHovers);

    // Scrolls the viewport through the document on the compositor, to each position once and then again.
    PrLayoutScrollInfo info{};
    prLayoutNodeScrollInfo(prCssPresentationTree(presentation), kPrLayoutNodeNone, &info);
    Measure cold;
    Measure warm;
    for (uint32_t pass = 0; pass < 2; ++pass) {
        PrLayoutUnit at{0};
        for (uint32_t i = 0; i < kScrollSamples; ++i) {
            PrLayoutUnit to{int32_t(int64_t(info.range.bottom.raw) * i / (kScrollSamples - 1))};
            prLayoutCompositorScroll(b.compositor, 0, kPrLayoutNodeNone, PrLayoutVec{PrLayoutUnit{0}, to - at});
            at = to;
            (pass == 0 ? cold : warm).sample([&] { prLayoutCompositorFrame(b.compositor); });
        }
        prLayoutCompositorScroll(b.compositor, 0, kPrLayoutNodeNone, PrLayoutVec{PrLayoutUnit{0}, -at});
    }
    // The page takes the offsets back at its next frame.
    frameNow(b, extent);
    row(first, "scroll frame first visit", cold);
    row(first, "scroll frame", warm);

    // Hovers points of the window, at the top of the document.
    Measure hits;
    Measure hovers;
    for (uint32_t i = 0; i < kHoverSamples; ++i) {
        PrLayoutVec point = prLayoutVecFromPx(PrVec2{random(kWidth), random(kHeight)});
        PrDomNode* node = nullptr;
        hits.sample([&] { node = prCssPresentationHitTest(presentation, point); });
        PrDomElement* element = node != nullptr ? prDomElementQuery(prDomNodeAsEventTarget(node)) : nullptr;
        if (element == nullptr && node != nullptr)
            element = prDomNodeParentElement(node);
        hovers.sample([&] {
            prHtmlDocumentSetHover(document, element);
            frameNow(b, extent);
        });
    }
    row(first, "hover hit test", hits);
    row(first, "hover frame", hovers);

    // Clicks links to fragments of the document, spread through it.
    Measure navigations;
    PrDomHtmlCollection* links = prDomHtmlCollectionCreateByTagName(prDomDocumentAsNode(document), "a");
    std::vector<PrDomElement*> fragments;
    for (uint32_t i = 0; i < prDomHtmlCollectionLength(links); ++i) {
        PrDomElement* link = prDomHtmlCollectionElementAt(links, i);
        PrStringView href = prDomElementGetAttribute(link, "href");
        if (href.size > 1 && href.data[0] == '#')
            fragments.push_back(link);
    }
    for (uint32_t i = 0; i < kLinkSamples && !fragments.empty(); ++i) {
        PrDomElement* link = fragments[size_t(i) * fragments.size() / kLinkSamples];
        PrDomMouseEventInit init = prDomMouseEventInitDefault();
        init.ui.event.bubbles = true;
        init.ui.event.cancelable = true;
        navigations.sample([&] {
            PrDomMouseEvent* click = prDomMouseEventCreate("click", &init);
            prDomEventTargetDispatchEvent(prDomNodeAsEventTarget(prDomElementAsNode(link)),
                prDomMouseEventAsEvent(click), nullptr);
            prDomEventRelease(prDomMouseEventAsEvent(click));
            frameNow(b, extent);
        });
    }
    prDomHtmlCollectionDestroy(links);
    row(first, "navigate", navigations);

    // Reads geometry after a change, as a script that writes and then reads does. Each read waits for what the change
    // needs.
    if (!fragments.empty()) {
        PrDomElement* written = fragments[fragments.size() / 2];
        PrDomElement* read = fragments.back();
        Measure afterPaint;
        Measure afterLayout;
        Measure afterScroll;
        auto sample = [&](Measure& measure) { measure.sample([&] { prCssElementGetBoundingClientRect(read); }); };
        for (uint32_t i = 0; i < kGeometrySamples; ++i) {
            prHtmlViewWait(b.view);
            prDomElementSetAttribute(written, "style", i % 2 == 0 ? "color: red" : "color: blue");
            sample(afterPaint);
            prDomElementSetAttribute(written, "style", i % 2 == 0 ? "padding-top: 3px" : "padding-top: 5px");
            sample(afterLayout);
            PrLayoutVec scroll = prLayoutVecFromPx(PrVec2{0, float(100 + 50 * i)});
            prCssPresentationSetScroll(presentation, kPrLayoutNodeNone, scroll);
            sample(afterScroll);
        }
        prDomElementRemoveAttribute(written, "style");
        prCssPresentationSetScroll(presentation, kPrLayoutNodeNone, PrLayoutVec{});
        frameNow(b, extent);
        prHtmlViewWait(b.view);
        row(first, "geometry after paint", afterPaint);
        row(first, "geometry after layout", afterLayout);
        row(first, "geometry after scroll", afterScroll);
    }

    // Restyles the whole document for a script that reads a style and waits for the restyle.
    Measure restyles;
    PrDomElement* root = prDomDocumentElement(document);
    PrDomElement* last = fragments.empty() ? root : fragments.back();
    PrCssDeclarationName color{};
    const PrCssStyleDeclaration* computed =
        prCssStylePropertiesAsCssStyleDeclaration(prCssElementComputedStyle(last, PrStringView{}));
    prCssStyleDeclarationNameFromText(computed, "color", &color);
    for (uint32_t i = 0; i < kRestyleSamples; ++i) {
        prDomElementSetAttribute(root, "style", i % 2 == 0 ? "font-size: 17px" : "font-size: 16px");
        restyles.sample([&] {
            PrRawString value = prCssStyleDeclarationGetPropertyValue(computed, &color);
            prRawStringReset(&value);
        });
        // The frame's job lays out the new styles before the next sample.
        frameNow(b, extent);
        prHtmlViewWait(b.view);
    }
    prDomElementRemoveAttribute(root, "style");
    frameNow(b, extent);
    row(first, "restyle", restyles);

    // Resizes the viewport and back.
    Measure resizes;
    for (uint32_t i = 0; i < kResizeSamples; ++i) {
        PrVec2 size = i % 2 == 0 ? PrVec2{kWidth - 124, kHeight} : extent;
        resizes.sample([&] { frameNow(b, size); });
    }
    row(first, "resize", resizes);
    printf("\n]\n");
    tearDown(b);
    return 0;
}
