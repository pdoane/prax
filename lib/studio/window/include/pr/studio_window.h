// Studio's window: an application's kit in a window of the platform, rendered with lib/draw on the platform's GPU.
//
// A window makes what a kit draws with: a font set that holds the Noto bundle, the platform's shaper, a layout tree,
// lib/ui's context over it, and a kit in Studio's theme, which follows the system's color scheme unless the application
// chooses one. Each frame runs the kit's rounds, which the application declares, plays the tree's scene into a draw
// list, and renders the list into the window's swap chain. Where the swap chain holds more than 8-bit sRGB, the list
// renders into a texture of half floats, which the display shader maps to the display (pr/draw_display.h). A device
// that is lost is made again.
//
// One thread runs a window: the thread that calls prStudioWindowRun, which the platform's application runs on.

#pragma once

#include "pr/app.h"
#include "pr/base.h"
#include "pr/color.h"
#include "pr/draw_list.h"
#include "pr/font_set.h"
#include "pr/kit.h"
#include "pr/kit_theme.h"
#include "pr/layout_tree.h"
#include "pr/status.h"
#include "pr/string_view.h"
#include "pr/text.h"
#include "pr/ui.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// A window while it runs, and what its kit draws with.
typedef struct PrStudioWindow PrStudioWindow;

// Called once the window's kit and application are made, before the first frame. Returns false to end the run.
typedef bool (*PrStudioWindowOpenFn)(void* PR_NULLABLE context, PrStudioWindow* window);

// Called at the start of each frame, before its rounds, as the application's model takes what arrived since the last.
typedef void (*PrStudioWindowFrameFn)(void* PR_NULLABLE context, PrStudioWindow* window, const PrAppFrame* frame);

// Returns the variant of Studio's theme that the kit draws in, for the user's preferences, at the start of each frame.
typedef PrKitThemeVariant (*PrStudioWindowThemeFn)(void* PR_NULLABLE context, PrStudioWindow* window,
    const PrAppUserPreferences* preferences);

// Called with each event before the window's context takes it. Returns true if the application took the event, which
// the context then never sees. The window asks for a frame either way.
typedef bool (*PrStudioWindowEventFn)(void* PR_NULLABLE context, PrStudioWindow* window, const PrAppEvent* event);

// Declares a round of the application under `root`, a surface on the app's ground that fills the viewport, which is
// `extent` px.
typedef void (*PrStudioWindowBuildFn)(void* PR_NULLABLE context, PrStudioWindow* window, PrLayoutNode root, PrVec2 extent);

// Called once a frame's rounds are laid out. Returns true if another frame would show something different.
typedef bool (*PrStudioWindowLaidOutFn)(void* PR_NULLABLE context, PrStudioWindow* window);

// Called once a frame is drawn, with the draw list that it played the tree's scene into, which is valid during the
// call, the extent of the frame in target pixels, and the canvas's color, which the list leaves unfilled.
typedef void (*PrStudioWindowDrawnFn)(void* PR_NULLABLE context, PrStudioWindow* window, const PrDrawList* list,
    PrUVec2 extent, PrColor canvas);

// Called as the run ends, after the last frame and before the kit is freed.
typedef void (*PrStudioWindowCloseFn)(void* PR_NULLABLE context, PrStudioWindow* window);

// How long a frame took, by its phases, in milliseconds: waiting for the swap chain, running the rounds, taking the
// tree's scene, playing it into the draw list, and rendering the list. The gap is the time since the last frame ended,
// and the input wait the time since the oldest event that no frame had taken arrived, or zero.
typedef struct PrStudioWindowFrameTimes {
    PrVec2 extent;                  // of the viewport, in px
    uint32_t rounds;
    double total;
    double wait;
    double build;
    double scene;
    double play;
    double render;
    double gap;
    double inputWait;
} PrStudioWindowFrameTimes;

// Called with the times of each frame.
typedef void (*PrStudioWindowTraceFn)(void* PR_NULLABLE context, const PrStudioWindowFrameTimes* times);

// What a window shows, and the application's callbacks.
typedef struct PrStudioWindowDesc {
    PrStringView title;
    PrUVec2 extent;                 // of the window at first, in px
    PrStringView fontDirectory;     // where the files of the Noto bundle are (pr/font_bundle.h)
    PrStudioWindowOpenFn PR_NULLABLE open;
    PrStudioWindowThemeFn PR_NULLABLE theme;    // or null for the variant of the user's preferred color scheme
    PrStudioWindowFrameFn PR_NULLABLE beginFrame;
    PrStudioWindowEventFn PR_NULLABLE event;
    PrStudioWindowBuildFn build;
    PrStudioWindowLaidOutFn PR_NULLABLE laidOut;
    PrStudioWindowDrawnFn PR_NULLABLE drawn;
    PrStudioWindowCloseFn PR_NULLABLE close;
    PrStudioWindowTraceFn PR_NULLABLE trace;
    void* PR_NULLABLE context;
} PrStudioWindowDesc;

// Runs a window until its application stops, and then frees it. Returns NotFound if the fonts cannot be installed,
// InvalidState if `open` returned false, NotSupported if the window has no device to render with, and Ok once the
// application stops. Unless the fonts cannot be installed, `close` is called as the run ends, whether or not the
// application ran.
PrStatus prStudioWindowRun(const PrStudioWindowDesc* desc);

// What the window's kit draws with, while it runs.
PrApp* prStudioWindowApp(const PrStudioWindow* window);
PrKit* prStudioWindowKit(const PrStudioWindow* window);
PrUi* prStudioWindowUi(const PrStudioWindow* window);
PrFontSet* prStudioWindowFonts(const PrStudioWindow* window);
PrTextShaper* prStudioWindowShaper(const PrStudioWindow* window);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
