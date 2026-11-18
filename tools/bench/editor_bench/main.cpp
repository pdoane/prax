// Measures what the editor's frames cost after each kind of input: through the rounds that declare and lay out its
// tree, the scene, playing the scene into a draw list, and preparing and rendering that list on the platform's device.
//
// ./dev.sh bench ui runs this beside ui_bench. The editor is a fixture of the benchmark's own, in two forms over one
// model: in Studio Kit's widgets (editor_view.h), an editor as examples/editor once was, which stays as it is so that
// the measures compare from change to change while the example evolves, and in lib/ui's elements and components
// (element_view.h). Each case runs on each form, its rows named "editor ..." and "elements ...".
//
// Each case is measured in a host of its own over frames at 1440 by 900 logical pixels at the case's pixel ratio, after
// frames that settle the editor, and writes a row of JSON: one frame's whole cost, and each stage's cost in
// instructions. The GPU's execution after each frame is waited for and not counted.
//
// Usage: editor_bench [case | --screenshot DIR], where the case is named with its form, as "elements hover"
//
// If a case is named, only that case runs, repeatedly, for five minutes (harnessRepeat). This is for attaching a
// profiler. --screenshot writes each form's first state, settled, to DIR as editor.png and elements.png, so that a
// person can see what the cases measure.

#include "pr/assert.h"
#include "pr/count_of.h"
#include "pr/draw_fragment.h"
#include "pr/draw_renderer.h"
#include "pr/gpu.h"
#include "pr/layout_query.h"
#include "pr/string_ops.h"
#include "pr/text_buffer.h"

#include "tools/bench/editor_bench/editor_view.h"
#include "tools/bench/editor_bench/element_view.h"
#include "tools/bench/editor_bench/host.h"
#include "tools/harness/harness.h"
#include "tools/harness/harness_gpu.h"
#include "tools/harness/harness_png.h"

#include <stdio.h>
#include <stdlib.h>

using namespace editor;
using namespace editor_bench;
using namespace harness;
using namespace pr;

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

// A case: the function that queues the input of each frame, which is `step` frames into the case, at `time`, and the
// function that changes the model before it, if the case has one. An open case resets the host before each frame
// (hostReset), so each frame builds the editor from nothing in a new context, as a window's first frame does, while
// what the application keeps for its life, its fonts, glyphs, paths and device, stays warm. An open case's time
// includes the reset, and its stages do not. A resize case narrows the viewport by a few logical pixels each frame, as
// dragging a window's edge does, so that no frame's width repeats the last one's.
struct BenchCase {
    const char* name;
    void (*input)(Host& host, uint32_t step, double time);
    bool open = false;
    float pixelRatio = 2;           // target pixels per logical pixel: 2 as on a Mac, 1.5 as Windows often has
    bool resize = false;
    void (*model)(EditorState& state, uint32_t step) = nullptr;
    uint32_t warm = 0;              // the frames of the case that run before it is measured
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

// The entities that navigation selects in turn.
constexpr EntityId kVisits[] = {7, 3, 9, 17, 22, 5};

// The steps of navigation, each of which changes what the editor shows, as a user moving through it does.
constexpr uint32_t kNavigationSteps = 8;

// Moves through the editor: selects other entities, which the inspector shows, opens and closes its groups, switches
// the viewport's tab, opens and closes the command palette, and shows and hides the log's warnings.
void navigate(EditorState& state, uint32_t step) {
    uint32_t visit = step / kNavigationSteps;
    switch (step % kNavigationSteps) {
        case 0: state.selected = kVisits[visit % countOf(kVisits)]; break;
        case 1: state.openMesh = !state.openMesh; break;
        case 2: state.openScript = !state.openScript; break;
        case 3: state.viewportTab = (state.viewportTab + 1) % 3; break;
        case 4: state.paletteOpen = !state.paletteOpen; break;
        case 5: state.toggleLevel(PrKitLogLevel_Warning); break;
        case 6: state.openRigidbody = !state.openRigidbody; break;
        default: state.selected = kVisits[(visit + 3) % countOf(kVisits)]; break;
    }
}

constexpr BenchCase kCases[] = {
    {"unchanged", nothing},
    {"pointer moved", movePointer},
    {"hover", hoverTools},
    {"select", selectRows},
    {"scroll", scrollScene},
    {.name = "navigate", .input = nothing, .model = navigate, .warm = kNavigationSteps * uint32_t(countOf(kVisits))},
    {"open", nothing, true},
    {"resize", nothing, false, 2, true},
    {"unchanged (pixel ratio 1.5)", nothing, false, 1.5f},
    {"hover (pixel ratio 1.5)", hoverTools, false, 1.5f},
    {"scroll (pixel ratio 1.5)", scrollScene, false, 1.5f},
    {"resize (pixel ratio 1.5)", nothing, false, 1.5f, true},
};

// The forms of the editor.
enum {
    Form_Kit,
    Form_Elements,
    Form_Count,
};
typedef uint32_t Form;

constexpr const char* kFormNames[Form_Count] = {"editor", "elements"};

void buildKit(void* context, PrKit* kit, PrLayoutNode root) {
    buildEditor(kit, root, *static_cast<EditorState*>(context));
}

void buildElements(void* context, PrKit*, PrLayoutNode root) {
    buildElementEditor(*static_cast<ElementEditor*>(context), root);
}

// The host, the editor, the device that it renders on, and what the case in progress has counted.
struct Run {
    Host host;
    EditorState* state = nullptr;   // the Kit form's model
    ElementEditor* elements = nullptr;
    EditorState* model = nullptr;   // the model of the form that runs
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
    gpuSubmitAndWait(commands);
    lap(Stage_Render);
    ++r.runs;
}

bool open(Run& r, float pixelRatio, Form form) {
    r.display.pixelRatio = pixelRatio;
    if (!hostOpen(r.host) || gpuDeviceCreate(PrGpuBackend_Default, nullptr, &r.device) != PrStatus_Ok ||
            prDrawFragmentCreate(r.device, nullptr, &r.renderer) != PrStatus_Ok)
        return false;
    r.drawFrame = prDrawFrameCreate(r.renderer);
    PrUVec2 extent{uint32_t(kExtent.x * pixelRatio), uint32_t(kExtent.y * pixelRatio)};
    PrGpuTextureDesc desc = prGpuTextureDesc2d(PrGpuFormat_Bgra8, extent,
        PrGpuTextureUsageFlag_Sampled | PrGpuTextureUsageFlag_RenderTarget);
    if (prGpuTextureCreate(r.device, &desc, &r.target) != PrStatus_Ok)
        return false;
    if (form == Form_Kit) {
        r.state = new EditorState{};
        r.state->loadLevel();
        r.model = r.state;
        r.host.build = buildKit;
        r.host.context = r.state;
        r.host.attach = editorAttach;
        r.host.detach = editorDetach;
    } else {
        r.elements = new ElementEditor{};
        r.elements->state.loadLevel();
        r.model = &r.elements->state;
        r.host.build = buildElements;
        r.host.context = r.elements;
        r.host.attach = elementAttach;
        r.host.detach = elementDetach;
    }
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
    delete r.elements;
}

// Writes the frame that the run rendered last to `path`, as a PNG.
bool screenshot(Run& r, const char* path) {
    float ratio = r.display.pixelRatio;
    PrUVec2 extent{uint32_t(r.extent.x * ratio), uint32_t(r.extent.y * ratio)};
    std::vector<uint8_t> texels(size_t(extent.x) * extent.y * 4);
    gpuTextureRead(r.device, r.target, PrGpuAccessFlag_ShaderRead, PrIVec2{0, 0},
        PrPixels{texels.data(), size_t(extent.x) * 4, extent});
    // The target is BGRA, and the PNG RGBA.
    for (size_t i = 0; i < texels.size(); i += 4) {
        uint8_t b = texels[i];
        texels[i] = texels[i + 2];
        texels[i + 2] = b;
    }
    return harnessWritePng(path, PrPixelView{texels.data(), size_t(extent.x) * 4, extent});
}

} // namespace

int main(int argc, char** argv) {
    prCrashHandler();
    bool shots = argc == 3 && PrStringView(argv[1]) == "--screenshot";
    if (argc > 2 && !shots) {
        fprintf(stderr, "usage: editor_bench [form case | --screenshot DIR]\n");
        return 2;
    }
    PrStringView only = argc == 2 ? argv[1] : PrStringView{};
    static Run r;
    if (shots) {
        for (Form form = 0; form < Form_Count; ++form) {
            if (!open(r, kDisplay.pixelRatio, form)) {
                fprintf(stderr, "editor_bench: no fonts among its resources, or no GPU device\n");
                return 1;
            }
            char path[512];
            snprintf(path, sizeof path, "%s/%s.png", argv[2], kFormNames[form]);
            if (!screenshot(r, path)) {
                fprintf(stderr, "editor_bench: could not write %s\n", path);
                return 1;
            }
            close(r);
            r = Run{};
        }
        return 0;
    }
    printf("[");
    bool first = true;
    for (uint32_t pass = 0; pass < Form_Count * countOf(kCases); ++pass) {
        Form form = pass / uint32_t(countOf(kCases));
        const BenchCase& c = kCases[pass % countOf(kCases)];
        char name[96];
        snprintf(name, sizeof name, "%s %s", kFormNames[form], c.name);
        if (only.data != nullptr && only != PrStringView(name))
            continue;
        // Each case starts from the editor's first state, in a host of its own, so that what a case measures does not
        // depend on the cases before it, or on how many frames the harness ran them for.
        if (!first) {
            close(r);
            r = Run{};
        }
        if (!open(r, c.pixelRatio, form)) {
            fprintf(stderr, "editor_bench: no fonts among its resources, or no GPU device\n");
            return 1;
        }
        auto run = [&] {
            if (c.open)
                hostReset(r.host);
            if (c.resize)
                r.extent.x = kExtent.x - kResizeStep * float(1 + r.step % kResizeFrames);
            if (c.model != nullptr)
                c.model(*r.model, r.step);
            c.input(r.host, r.step++, r.time);
            runFrame(r);
        };
        // A case that warms runs its frames once first, so that what the application keeps for its life, such as its
        // fonts, glyphs and paths, is warm, and the measure is of what the case changes.
        for (uint32_t i = 0; i < c.warm; ++i)
            run();
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
            first ? "" : ",", name, cost.millis, r.host.rounds, stats.blocksLaidOut, stats.layersRecorded,
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
