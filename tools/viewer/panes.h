// The viewer's panes. Each pane renders what it shows independently, and the viewer thread shows each pane's latest
// render.

#pragma once

#include "pr/app.h"
#include "pr/app_display.h"
#include "pr/app_user_preferences.h"
#include "pr/draw_renderer.h"
#include "pr/gpu.h"

#include "tools/viewer/documents.h"
#include "tools/viewer/renderers.h"
#include "tools/viewer/ui.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

PR_ASSUME_NONNULL_BEGIN

namespace viewer {

// Returns the milliseconds from `begin` to now.
double millisSince(std::chrono::steady_clock::time_point begin);

// A texture that a document renders into, with the state of the last render into it. The texture rests in the accesses
// of shader reads.
struct Slot {
    PrGpuDevice* PR_NULLABLE device = nullptr;     // the device of the texture
    PrGpuTexture texture{};
    PrDrawFrame* PR_NULLABLE drawn = nullptr;       // kept until the next prepare
    PrDrawList* PR_NULLABLE prepared = nullptr;     // the list that `drawn` prepared, unchanged while the frame lives
    std::string shown;                              // what the texture holds; empty for nothing
    Rendered rendered;
    float pixelRatio = 1;                           // the texture's target pixels per unit of the document
    bool viewed = false;                            // true if the render is of the view's region, shown at `at`
    PrRect at{};                                    // viewed: where the texture goes, in the window's logical pixels
    uint64_t frame = 0;                             // the last of the viewer's frames that draws the slot
};

// Releases the slot's texture, frame and list, and forgets what it holds.
void releaseSlot(Slot& slot);

// Makes the texture, or the slot's texture, `width` by `height` target pixels in `format`: Bgra8, or RgbaF16 for HDR,
// like the swap chain's textures. Resizing a slot or changing its format releases what the slot held.
PrStatus sizeTexture(PrGpuDevice* device, PrGpuTexture& texture, uint32_t width, uint32_t height,
                     PrPixelFormat format = PrPixelFormat_Bgra8);
PrStatus sizeSlot(PrGpuDevice* device, Slot& slot, uint32_t width, uint32_t height,
                  PrPixelFormat format = PrPixelFormat_Bgra8);

// Returns `count` premultiplied pixels of the color in `format`, Bgra8 or RgbaF16.
std::vector<uint8_t> pixelsOf(PrColor color, size_t count, PrPixelFormat format = PrPixelFormat_Bgra8);

// Returns RgbaF16 pixels as Bgra8, clamped.
std::vector<uint8_t> narrowed(PrPixelView texels);

// Where a pane's render goes: the format of its pixels, the display that a page resolves its colors for and that an
// RgbaF16 render maps its colors for, and the user's preferences, which a page follows.
struct PaneOutput {
    PrPixelFormat format = PrPixelFormat_Bgra8;
    PrAppDisplay display = prAppDisplayDefault();
    PrAppUserPreferences preferences{};
};

// Records the commands that render the list into the slot with the renderer, over the background. A renderer on the
// device prepares the list, then encodes it into a pass that clears the slot's texture to the background. A renderer in
// memory renders into pixels filled with the background, which are then written to the texture. The result's `ms` is
// how long preparation or rendering took.
Rendered renderSlot(Slot& slot, const std::vector<RendererConfig>& configs, uint32_t config, const PrDrawList* list,
                    PrColor background, PrGpuCommands* commands, PaneOutput output);

// The time that documents draw at. Every pane reads the clock when it starts a render, and the panes of one document
// start together. The clock holds a fixed time, or runs on from when it was set.
class SharedClock {
public:
    // Sets the time to `seconds`, and whether it runs on from there.
    void set(double seconds, bool running);

    // Return the current time in seconds, and whether the clock runs.
    double now();
    bool running();

private:
    std::mutex mutex_;
    double base_ = 0;
    std::chrono::steady_clock::time_point at_;
    bool running_ = false;
};

// Measures frames per second, over the frames that a source finished in the last second.
class FpsMeter {
public:
    // Counts a finished frame, and returns the frames per second.
    double done();

private:
    std::vector<std::chrono::steady_clock::time_point> times_;
};

// What a pane shows and how it renders it, as given by the viewer thread.
struct PaneSource {
    std::string stem;                   // what the pane shows, except the time; empty for nothing
    std::shared_ptr<Loaded> loaded;     // the source that the engine or an in-process player draws, kept loaded
    std::string url;                    // the page that the browser renders
    uint32_t config = 0;
    bool backToBack = false;            // true to render again as soon as a render ends, rather than once it is taken
    float docWidth = 0;                 // the size that the document lays out for
    float docHeight = 0;
    float pixelRatio = 1;               // the texture's target pixels per unit of the document

    // If set, the render is of the view's region: the part of the document that the pane shows, in a texture no larger
    // than the pane. The texture's pixels hold the document's origin at `origin`, and the texture goes to `at` in the
    // window's logical pixels. `region` is that part in the document's units. Otherwise, the render is of the whole
    // document, which the view places and zooms as it shows it.
    bool viewed = false;
    PrVec2 origin{0, 0};
    PrRect at{};
    PrRect region{};

    uint32_t width = 0;                 // the texture's size
    uint32_t height = 0;
    PrColor background{1, 1, 1, 1};     // what the render draws over
    PaneOutput output;

    // Where a render's pixels go to be compared, or null if the renders are not compared.
    std::function<void(const std::string& shown, PrPixelView texels)> compare = nullptr;
};

// What the viewer thread and a pane share. The context outlives both.
struct PaneContext {
    PrApp* app;
    PrGpuDevice* device;
    const std::vector<RendererConfig>* configs;
    std::vector<std::unique_ptr<std::mutex>>* configLocks; // one per renderer, held while the renderer is used
    SharedClock* clock;

    // The viewer's frames that the device has finished, with the mutex that guards the count and the condition that
    // signals its change. A pane waits on this before it writes a slot.
    std::atomic<uint64_t>* framesDone;
    std::mutex* framesMutex;
    std::condition_variable* framesDoneChanged;
};

// A pane, which renders into three textures used as a swap chain.
//
// A pane that the engine or an in-process player draws renders on its own thread, continuously while it shows a source,
// so that its figures are those of a steady stream of frames. The display paces the renders: once the source is
// rendered, the next render starts only after the viewer thread has taken the latest one, so a pane renders at most
// once for each of the viewer's frames. A change of source is rendered without waiting. A source that renders back to
// back is not paced, so that its figures are those of the device working without pause. Each render reads the clock's
// time, renders into the texture that is neither shown nor the latest, waits for the device if the render is on it, and
// makes that texture the latest. The pane measures its frame rate as it goes. A pane that the browser draws has no
// thread. The browser's callbacks fill it through the same textures, because only the browser's own thread drives the
// browser.
//
// The viewer thread and the panes use the device concurrently, each with its own commands and textures (pr/gpu.h). A
// pane writes only a texture that the viewer does not show, once the frames that showed it are done, and it waits for
// its own commands.
class Pane {
public:
    // Creates a pane that renders on its own thread if `threaded` is true, and is otherwise filled by its caller.
    Pane(const PaneContext& context, bool threaded);
    Pane(const Pane&) = delete;
    Pane& operator=(const Pane&) = delete;
    ~Pane();

    // Sets what the pane shows from now on. A source equal to the last one changes nothing.
    void show(PaneSource source);

    // Notes that the source changed in place, as a page does when it takes input, so that the pane renders it again at
    // once and is not settled until that render is taken.
    void touch();

    // Returns what the pane shows, as the viewer thread last set it.
    const PaneSource& source() const { return source_; }

    // For the viewer thread: returns the latest render, which the viewer's frame `frame` draws, if it is in `format`,
    // or else the render that the viewer last took, so that a frame never draws a render made for another output. The
    // pane does not write the slot until that frame is done.
    const Slot& take(uint64_t frame, PrPixelFormat format);

    // Returns true if the latest render is of the pane's source at the clock's current time, and the viewer has taken
    // it.
    bool settled();

    // Returns true if the latest render is of the pane's source at any time, and the viewer has taken it. A pane that
    // shows nothing is current.
    bool current();

    // For a pane that its caller fills. begin() returns the index of the slot that the next render goes into, once the
    // frames that drew the slot are done. slot() is the slot at an index. publish() makes the slot the latest, as a
    // render of the source as it was at `version`.
    uint32_t begin();
    Slot& slot(uint32_t index) { return slots_[index]; }
    void publish(uint32_t index, uint64_t version);

    // Returns the number of times the pane's source has changed. A render records this number to show that it is
    // current.
    uint64_t version();

    // Returns true if the latest render, taken or not, is of the current source and shows `shown`.
    bool holds(const std::string& shown);

    // Returns true if the latest render, taken or not, is of the current source and in `format`, or if the pane has no
    // render that another format would show differently: it shows nothing, a reference's page, or a failed render.
    bool rendersIn(PrPixelFormat format);

    // Stops the pane's thread. The thread leaves nothing of its own pending on the device. The pane keeps its textures
    // until it is destroyed.
    void stop();

private:
    void run();
    void render(const PaneSource& source, double time, Slot& slot);

    PaneContext context_;
    PaneSource source_;                 // the source that the viewer thread reads, guarded by the mutex
    std::mutex mutex_;
    std::condition_variable changed_;
    PaneSource wanted_;                 // what the thread renders next
    uint64_t version_ = 0;              // the number of changes to `wanted_`
    uint64_t latestVersion_ = 0;        // the version that the latest render is of
    Slot slots_[3];
    uint32_t shown_ = 0;
    uint32_t latest_ = 0;
    bool rendering_ = false;
    bool stopping_ = false;
    FpsMeter fps_;
    std::thread thread_;
};

// Returns what a pane shows at a time, as the comparer and the slots name it.
std::string shownAt(const std::string& stem, double time);

} // namespace viewer

PR_ASSUME_NONNULL_END
