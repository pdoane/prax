// The viewer's controls: the lists, choices and figures beside the documents, in Studio Kit.

#pragma once

#include "pr/app.h"
#include "pr/geom.h"
#include "pr/gpu.h"
#include "pr/pixels.h"
#include "pr/status.h"

#include "tools/harness/compare.h"
#include "tools/viewer/documents.h"
#include "tools/viewer/renderers.h"

#include <string>
#include <vector>

PR_ASSUME_NONNULL_BEGIN

namespace viewer {

using harness::ImageDifference;

// What the documents' area shows: the first pane alone, or both panes compared.
enum { View_Single, View_Compare };
typedef uint8_t View;

// The clock's rate in frames per second, and one step of the clock in seconds.
constexpr double kFrameRate = 60;
constexpr double kFrame = 1.0 / kFrameRate;

// How the comparison view shows its two renders.
enum { Look_SideBySide, Look_Flip, Look_Difference };
typedef uint8_t Look;

// What renders a pane: the engine, Prax, in a configuration of its renderers, or one of the document's references, such
// as another player or an image.
enum { Renderer_Prax, Renderer_Reference };
typedef uint8_t Renderer;

// What a pane shows.
struct PaneChoice {
    Renderer renderer = Renderer_Prax;

    // Prax: the renderer that renders the document, and the text renderers that it loads with, so that a pane loads its
    // own copy of the document.
    uint32_t config = 0;
    TextBackends text{};

    // Reference: one of the document's references (referencesOf). The first is the reference that its suite compares
    // the engine with. A pane whose document has no such reference shows Prax.
    uint32_t reference = 0;

    // For each family of renderers, the renderer that the pane last showed from it.
    std::vector<uint32_t> shown;
};

// What the viewer shows. The controls and the keys change it.
struct Choices {
    uint32_t collection = 0;
    uint32_t document = 0;

    // The panes, which are alike. The single view shows the first. Comparison shows the first on the left and the
    // second on the right, or flips between them, or shows their difference. The keys that choose renderers choose the
    // first's.
    PaneChoice panes[2];

    View view = View_Single;
    Look look = Look_SideBySide;
    bool showingSecond = false;     // Flip: true if the second pane shows
    bool fit = false;               // Compare: true to fit the renders to their panes, false to show them at `zoom`
    float zoom = 1;                 // logical pixels per unit of the document; 1 is its natural size
    float panX = 0;                 // the renders' offset from their panes' centers, in logical pixels
    float panY = 0;
    bool documentChanged = false;   // true if a key moved to another document, so that the list scrolls to it
    double time = 0;                // seconds on the clock that the document draws at
    bool playing = true;            // true if the document's clock runs

    // True if the panes render back to back, as fast as the device allows, to measure. Otherwise, the display paces
    // them, one render for each refresh.
    bool backToBack = false;

    // True if the viewer presents in half floats, so that colors past SDR white and past sRGB's gamut show on a display
    // that can show them.
    bool hdr = false;
};

// The figures of one render of a document.
struct Rendered {
    uint32_t commands = 0;
    double recordMs = 0;        // recording the document into the list
    uint32_t width = 0;         // target pixels
    uint32_t height = 0;

    // The whole frame on its source's thread: recording, rendering and completion on the GPU.
    double ms = 0;

    double rendererMs = 0;      // the part of `ms` the renderer spent on the CPU: preparing and encoding, or rendering
    double gpuMs = 0;           // the part of `ms` that the GPU spent executing
    bool onGpu = false;         // true if the renderer renders on the GPU
    PrStatus status = PrStatus_Ok;

    // The frames per second that the source finishes, measured on the source's own thread: a pane's frames, or the
    // browser's round trips.
    double fps = 0;

    // The time that the render shows, in seconds, and the document's frame at that time. Each is negative if unknown,
    // as for a page that plays on its own.
    double time = 0;
    double frame = -1;

    // Set for the system's browser rather than a renderer: `ms` is a capture's round trip, and there is no recording,
    // renderer or GPU time.
    bool captured = false;

    // True if the render was one of renders back to back, false if the display paced it. Paced renders find the device
    // idle between them, at lower clocks, so their times are longer than those of renders back to back.
    bool backToBack = false;
};

// The figures that the controls show: each pane's and, in comparison, how the panes differ.
struct Figures {
    Rendered panes[2];
    float zoom = 1;             // the comparison's zoom as shown, fitted or not
    bool compared = false;      // true if the difference is of the renders shown
    bool comparing = false;     // true while a difference of the shown renders is being computed
    bool playing = false;       // true if the clock runs, so that the renders are not compared
    ImageDifference difference{};
};

// The controls and what they draw with.
struct Ui;

// Shows the family's renderer in the pane, in the configuration that the pane last showed, and remembers the current
// family's configuration.
void chooseFamily(PaneChoice& pane, const std::vector<RendererConfig>& configs, uint32_t family);

// Returns the pane's name: Prax's configuration, or the reference's name. `references` are the shown document's.
std::string paneName(const PaneChoice& pane, const std::vector<RendererConfig>& configs,
    const std::vector<Reference>& references);

// Creates controls drawn on the device, a device of prGpuDeviceCreate, which renders them into the frame over the
// documents.
Ui* uiCreate(PrGpuDevice* device, PrApp* app);

// Destroys the controls.
void uiDestroy(Ui* ui);

// Passes an input to the controls. Returns true if they take it, so that nothing else acts on it. The pointer is theirs
// over them and while a press on them is held. The keys are theirs while a text field or a menu has focus, and the keys
// that move in the list of documents are always theirs. A page that holds the keyboard is sent its keys without passing
// them here.
bool uiEvent(Ui* ui, const PrAppEvent& event);

// Lays out the controls for the frame, and applies what they changed to `choices`. `pageKeys` is true while a page
// holds the keyboard, so that the controls give up focus. `references` are the shown document's own references. Returns
// the rectangle left for documents, in logical pixels.
PrRect uiLayout(Ui* ui, const PrAppFrame* frame, bool pageKeys, const std::vector<RendererConfig>& configs,
    const std::vector<Reference>& references, const Figures& figures, Choices& choices);

// The cursor that the controls show at the pointer.
PrAppOutput uiOutput(const Ui* ui);

// Draws the texture into the frame at `destination`, clipped to `clip`, under the controls laid out last. Both
// rectangles are in logical pixels. If `nearest` is true, the texture is sampled by nearest texel, and zoomed pixels
// show as squares. The device scales the texture as it draws the frame. The texture is in the accesses of shader reads
// while the frame draws it.
void uiImage(Ui* ui, PrGpuTexture texture, PrRect clip, PrRect destination, bool nearest);

// Prepares the images and controls of the frame's layout, for a frame of `extent` target pixels in `format`. This comes
// before the pass that uiEncode draws in.
void uiPrepare(Ui* ui, PrGpuCommands* commands, PrUVec2 extent, PrPixelFormat format);

// Draws what uiPrepare prepared into `pass`, which is open over the frame.
void uiEncode(Ui* ui, PrGpuPass* pass);

} // namespace viewer

PR_ASSUME_NONNULL_END
