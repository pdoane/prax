// Measures what the editor's frames cost after each kind of input: through the rounds that declare and lay out its
// tree, the scene, playing the scene into a draw list, and preparing and rendering that list on the platform's device.
//
// ./dev.sh bench ui runs this beside ui_bench. The editor is a fixture of the benchmark's own (editor_view.h), an
// editor as examples/editor once was, which stays as it is so that the measures compare from change to change while the
// example evolves.
//
// Each case is measured in a host of its own over frames at 1440 by 900 logical pixels at the case's pixel ratio, after
// frames that settle the editor, and writes a row of JSON: one frame's whole cost, and each stage's cost in
// instructions. The GPU's execution after each frame is waited for and not counted.
//
// Usage: editor_bench [case]
//
// If a case is named, only that case runs, repeatedly, for five minutes (harnessRepeat). This is for attaching a
// profiler.

#include "pr/assert.h"
#include "pr/count_of.h"
#include "pr/draw_fragment.h"
#include "pr/draw_renderer.h"
#include "pr/gpu.h"
#include "pr/layout_query.h"
#include "pr/string_ops.h"
#include "pr/text_buffer.h"

#include "tools/bench/editor_bench/editor_view.h"
#include "tools/bench/editor_bench/host.h"
#include "tools/harness/harness.h"

#include <stdio.h>
#include <stdlib.h>

using namespace pr;
using namespace harness;
using namespace editor;
using namespace editor_bench;

namespace {

// The viewport, in logical pixels, on an SDR display, in the dark theme. Each case gives the display's pixel ratio.
constexpr PrVec2 kExtent{1440, 900};
constexpr PrAppDisplay kDisplay{.pixelRatio = 2, .headroom = 1, .potentialHeadroom = 1, .gamut = PrColorSpace_Srgb};
constexpr PrAppUserPreferences kPreferences{.colorScheme = PrAppColorScheme_Dark};

// The frames that settle the editor before its cases are measured.
constexpr uint32_t kSettleFrames = 4;

// How a resize case narrows the viewport: by kResizeStep logical pixels a frame, over kResizeFrames frames before it
// starts again at the full width.
constexpr float kResizeStep = 3;
constexpr uint32_t kResizeFrames = 64;

// A case: the function that queues the input of each frame, which is `step` frames into the case, at `time`. A cold
// case resets the host before each frame (hostReset), so each frame builds the editor from nothing while fonts, glyphs
// and the device stay warm. A cold case's time includes the reset, and its stages do not. A resize case narrows the
// viewport by a few logical pixels each frame, as dragging a window's edge does, so that no frame's width repeats the
// last one's.
struct BenchCase {
    const char* name;
    void (*input)(Host& host, uint32_t step, double time);
    bool cold = false;
    float pixelRatio = 2;           // target pixels per logical pixel: 2 as on a Mac, 1.5 as Windows often has
    bool resize = false;
};

// The stages of a frame, whose instructions each case sums over its runs. The host's frame is its rounds (routing,
// building, committing and laying out), its scene, and its play into a draw list. Then the list is prepared for the
// device, rendered and submitted.
enum {
    Stage_Frame,
    Stage_Prepare,
    Stage_Render,
    Stage_Count,
};
typedef uint32_t Stage;

constexpr const char* kStageNames[Stage_Count] = {"frame", "prepare", "render"};

// Queues an event of `kind` with the pointer at `at`.
void pointer(Host& host, PrAppEventKind kind, PrVec2 at, double time) {
    hostEvent(host, PrAppEvent{.kind = kind, .time = time, .pointer = {.position = at}});
}

void click(Host& host, PrVec2 at, double time) {
    pointer(host, PrAppEventKind_PointerDown, at, time);
    pointer(host, PrAppEventKind_PointerUp, at, time);
}

void wheel(Host& host, PrVec2 at, PrVec2 delta, double time) {
    hostEvent(host, PrAppEvent{.kind = PrAppEventKind_Scroll, .time = time, .scroll = {.position = at, .delta = delta}});
}

// Moves the pointer between two points of the viewport, over nothing that responds to it.
void movePointer(Host& host, uint32_t step, double time) {
    pointer(host, PrAppEventKind_PointerMove, (step & 1) != 0 ? PrVec2{700, 500} : PrVec2{720, 510}, time);
}

// Moves the pointer between two of the toolbar's tools every eight frames, fading one in and the other out over the
// frames in between.
void hoverTools(Host& host, uint32_t step, double time) {
    pointer(host, PrAppEventKind_PointerMove, (step & 8) != 0 ? PrVec2{20, 42} : PrVec2{48, 42}, time);
}

// Clicks two of the scene's rows in turn, selecting each.
void selectRows(Host& host, uint32_t step, double time) {
    click(host, (step & 1) != 0 ? PrVec2{120, 144} : PrVec2{120, 166}, time);
}

// Scrolls the scene's rows down and back.
void scrollScene(Host& host, uint32_t step, double time) {
    wheel(host, PrVec2{120, 300}, PrVec2{0, (step & 1) != 0 ? 40.0f : -40.0f}, time);
}

void nothing(Host&, uint32_t, double) {}

constexpr BenchCase kCases[] = {
    {"editor unchanged", nothing},
    {"editor pointer moved", movePointer},
    {"editor hover", hoverTools},
    {"editor select", selectRows},
    {"editor scroll", scrollScene},
    {"editor cold", nothing, true},
    {"editor resize", nothing, false, 2, true},
    {"editor unchanged (pixel ratio 1.5)", nothing, false, 1.5f},
    {"editor hover (pixel ratio 1.5)", hoverTools, false, 1.5f},
    {"editor scroll (pixel ratio 1.5)", scrollScene, false, 1.5f},
    {"editor resize (pixel ratio 1.5)", nothing, false, 1.5f, true},
};

void buildApp(void* context, PrKit* kit, PrLayoutNode root) {
    buildEditor(kit, root, *static_cast<EditorState*>(context));
}

// The host, the editor, the device that it renders on, and what the case in progress has counted.
struct Run {
    Host host;
    EditorState* state = nullptr;
    PrAppDisplay display = kDisplay;
    PrVec2 extent = kExtent;        // the viewport, in logical pixels, which a resize case narrows
    PrGpuDevice* device = nullptr;
    PrDrawRenderer* renderer = nullptr;
    PrDrawFrame* drawFrame = nullptr;
    PrGpuTexture target{};
    double time = 0;
    uint32_t step = 0;
    double stages[Stage_Count] = {};
    uint32_t runs = 0;
};

// Runs one frame and counts its stages.
void runFrame(Run& r) {
    r.time += 1.0 / 60;
    HarnessCounters at = harnessCounters();
    auto lap = [&](Stage stage) {
        HarnessCounters now = harnessCounters();
        r.stages[stage] += now.instructions - at.instructions;
        at = now;
    };
    hostFrame(r.host, r.time, 1.0 / 60, r.extent, r.display, kPreferences, 4);
    lap(Stage_Frame);
    // A benchmark that lost its device measures nothing more.
    PrGpuCommands* commands = nullptr;
    if (!prGpuCommandsBegin(r.device, nullptr, &commands)) {
        fprintf(stderr, "editor_bench: the GPU device was lost\n");
        exit(1);
    }
    float ratio = r.display.pixelRatio;
    PrDrawTarget target{
        .extent = {uint32_t(r.extent.x * ratio), uint32_t(r.extent.y * ratio)},
        .format = PrPixelFormat_Bgra8,
        .commands = commands,
    };
    prDrawFramePrepare(r.drawFrame, r.host.list, &target);
    lap(Stage_Prepare);
    prGpuCommandsTextureBarrier(commands, r.target, PrGpuSubresources{}, PrGpuAccessFlag_ShaderRead,
        PrGpuAccessFlag_ColorAttachment);
    PrGpuPassDesc pass = prGpuPassDescColor(r.target, PrGpuLoad_Clear, hostClearColor(r.host));
    PrGpuPass* encoder = prGpuPassBegin(commands, &pass);
    prDrawFrameRender(r.drawFrame, encoder, PrIVec2{0, 0});
    prGpuPassEnd(encoder);
    prGpuCommandsTextureBarrier(commands, r.target, PrGpuSubresources{},
        PrGpuAccessFlag_ColorAttachment, PrGpuAccessFlag_ShaderRead);
    prGpuCommandsSubmitAndWait(commands);
    lap(Stage_Render);
    ++r.runs;
}

bool open(Run& r, float pixelRatio) {
    r.display.pixelRatio = pixelRatio;
    if (!hostOpen(r.host, PR_FONT_BUNDLE_DIR) ||
            prGpuDeviceCreate(PrGpuBackend_Default, nullptr, &r.device) != PrStatus_Ok ||
            prDrawFragmentCreate(r.device, nullptr, &r.renderer) != PrStatus_Ok)
        return false;
    r.drawFrame = prDrawFrameCreate(r.renderer);
    PrUVec2 extent{uint32_t(kExtent.x * pixelRatio), uint32_t(kExtent.y * pixelRatio)};
    PrGpuTextureDesc desc = prGpuTextureDesc2d(PrGpuFormat_Bgra8, extent,
        PrGpuTextureUsageFlag_Sampled | PrGpuTextureUsageFlag_RenderTarget);
    if (prGpuTextureCreate(r.device, &desc, &r.target) != PrStatus_Ok)
        return false;
    r.state = new EditorState{};
    r.state->loadLevel();
    r.host.build = buildApp;
    r.host.context = r.state;
    r.host.attach = editorAttach;
    r.host.detach = editorDetach;
    for (uint32_t i = 0; i < kSettleFrames; ++i)
        runFrame(r);
    return true;
}

void close(Run& r) {
    if (r.device != nullptr)
        prGpuTextureDestroy(r.device, r.target);
    prDrawFrameDestroy(r.drawFrame);
    prDrawRendererDestroy(r.renderer);
    prGpuDeviceDestroy(r.device);
    hostClose(r.host);
    delete r.state;
}

} // namespace

int main(int argc, char** argv) {
    prCrashHandler();
    if (argc > 2) {
        fprintf(stderr, "usage: editor_bench [case]\n");
        return 2;
    }
    PrStringView only = argc == 2 ? argv[1] : PrStringView{};
    static Run r;
    printf("[");
    bool first = true;
    for (const BenchCase& c : kCases) {
        if (only.data != nullptr && only != c.name)
            continue;
        // Each case starts from the editor's first state, in a host of its own, so that what a case measures does not
        // depend on the cases before it, or on how many frames the harness ran them for.
        if (!first) {
            close(r);
            r = Run{};
        }
        if (!open(r, c.pixelRatio)) {
            fprintf(stderr, "editor_bench: no fonts under %s, or no GPU device\n", PR_FONT_BUNDLE_DIR);
            return 1;
        }
        auto run = [&] {
            if (c.cold)
                hostReset(r.host);
            if (c.resize)
                r.extent.x = kExtent.x - kResizeStep * float(1 + r.step % kResizeFrames);
            c.input(r.host, r.step++, r.time);
            runFrame(r);
        };
        if (only.data != nullptr)
            harnessRepeat(run);
        for (double& stage : r.stages)
            stage = 0;
        r.runs = 0;
        HarnessCounters cost = harnessMeasure(run);
        PrLayoutTreeStats stats = prLayoutTreeStats(r.host.tree);
        PrTextBuffer row{};
        harnessAppendFormat(&row, "%s\n  {\"name\": \"%s\", \"time\": %.6f, \"rounds\": %u, \"blocks\": %u, \"layers\": %u, "
            "\"commands\": %u, \"made\": %u",
            first ? "" : ",", c.name, cost.millis, r.host.rounds, stats.blocksLaidOut, stats.layersRecorded,
            stats.commandsRecorded, stats.frameNodesMade);
        for (uint32_t s = 0; s < Stage_Count; ++s)
            harnessAppendFormat(&row, ", \"%s\": %.0f", kStageNames[s], r.stages[s] / double(r.runs));
        HarnessCounts counts{};
        harnessCountsAdd(&counts, "time", cost);
        harnessCountsWrite(&counts, &row);
        prTextBufferAppend(&row, "}");
        fwrite(row.data, 1, row.size, stdout);
        prTextBufferReset(&row);
        first = false;
    }
    printf("\n]\n");
    close(r);
    return 0;
}
