// The headless host that the editor benchmark measures through: the tree, context and kit that a frame declares into,
// and its frames and their paint, in Studio Kit's dark theme.

#pragma once

#include "pr/app.h"
#include "pr/draw_list.h"
#include "pr/font_set.h"
#include "pr/kit.h"
#include "pr/kit_panel.h"
#include "pr/layout_scene.h"
#include "pr/layout_tree.h"
#include "pr/text_font.h"
#include "pr/ui.h"

#include <stdint.h>
#include <vector>

PR_ASSUME_NONNULL_BEGIN

namespace editor_bench {

// Declares the application's widgets under `root`, a column that fills the viewport, once per round.
typedef void (*BuildFn)(void* context, PrKit* kit, PrLayoutNode root);

struct Host;

// Makes or destroys what an application keeps for one of the host's contexts, such as the instance of another component
// library that the application composes with the kit.
typedef void (*ContextFn)(void* context, Host& host);

// The tree, context and kit of a headless run.
struct Host {
    PrFontSet* fonts = nullptr;
    PrTextShaper* shaper = nullptr;
    PrLayoutTree* tree = nullptr;
    PrUi* ui = nullptr;
    PrKit* kit = nullptr;
    PrDrawList* list = nullptr;     // the last frame's paint, in target pixels
    const PrLayoutScene* scene = nullptr;
    PrAppDisplay display = prAppDisplayDefault(); // the display of the last frame
    PrColor16 canvas{};                           // the canvas color of the last frame's scene, which the pass clears
                                                  // to
    uint32_t rounds = 0;                          // the rounds of the last frame

    BuildFn build = nullptr;
    void* context = nullptr;

    // `attach` is called with `context` before the first frame of each context that the host creates, and `detach`
    // before the host destroys that context.
    ContextFn attach = nullptr;
    ContextFn detach = nullptr;
    bool attached = false;          // true from a context's attach until its detach

    std::vector<PrLayoutSceneScroll> scrolls;   // the offsets that the last frame played its scene with
};

// Opens a host in Studio's dark theme, with its text in the Complete bundle under `fontDirectory`. Returns false if the
// fonts could not be installed.
bool hostOpen(Host& host, const char* fontDirectory);
void hostClose(Host& host);

// Destroys and recreates the host's tree, context and kit, and keeps its fonts, shaper and theme. The next frame builds
// from nothing, as a window's first frame does after its fonts are loaded.
void hostReset(Host& host);

// Queues an input event for the next frame.
void hostEvent(Host& host, const PrAppEvent& event);

// Runs a frame of up to `rounds` rounds over a viewport of `extent` logical pixels, for `display` and `preferences`,
// and paints it into the host's list at the display's pixel ratio.
void hostFrame(Host& host, double time, double delta, PrVec2 extent, const PrAppDisplay& display,
    const PrAppUserPreferences& preferences, uint32_t rounds);

// Returns the clear color of a pass that fills the last frame's canvas, premultiplied.
PrColor hostClearColor(const Host& host);

} // namespace editor_bench

PR_ASSUME_NONNULL_END
