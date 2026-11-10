// Studio's windows: an application's kit in a window of the platform, rendered with lib/draw on the Studio
// application's GPU device (pr/studio_app.h).
//
// A window makes what a kit draws with, over its Studio application's fonts and shaper: a layout tree, lib/ui's context
// over the tree, and a kit in Studio's theme. The theme follows the system's color scheme unless the application
// chooses one.
//
// Each frame runs the rounds that the application declares, plays the tree's scene into a draw list, and renders the
// list into the window's swap chain. The render pass clears to the scene's canvas color (prLayoutSceneCanvasColor), and
// the list does not fill the canvas. The canvas color is the root's background. If a view covers the window beneath the
// rest of the application, the canvas color is the view's background instead. Such a view declares its background on
// its box, and does not paint it, because the clear already fills it.
//
// If the swap chain holds more than 8-bit sRGB, the list renders into a texture of half floats, which the display
// shader maps to the display (pr/draw_display.h).
//
// In an application of Multiple windows, the window hosts the surfaces of its context (prUiSetHost): each menu, tooltip
// or dialog that floats over the window shows in a Popup or Dialog window that the window owns. The window's frame lays
// them out with the rest of its tree, and each of their windows draws its surface from that frame. Their windows take
// the window's cursor and drop answer, and the window's output names the one that holds the caret of the text being
// edited. Each Studio window hosts only its own surfaces.
//
// The user's request to close a window closes it unless the window's event callback takes the request.
//
// A window is used from the thread that runs its application.

#pragma once

#include "pr/app.h"
#include "pr/base.h"
#include "pr/color.h"
#include "pr/draw_list.h"
#include "pr/font_set.h"
#include "pr/gpu.h"
#include "pr/kit.h"
#include "pr/kit_theme.h"
#include "pr/kit_title_bar.h"
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

// The Studio application that a window belongs to (pr/studio_app.h).
typedef struct PrStudioApp PrStudioApp;

// Called once the window, its kit and its device are made, before the first frame. Returns false to close the window.
typedef bool (*PrStudioWindowOpenFn)(void* PR_NULLABLE context, PrStudioWindow* window);

// Called at the start of each frame, before its rounds, as the application's model takes what arrived since the last.
typedef void (*PrStudioWindowFrameFn)(void* PR_NULLABLE context, PrStudioWindow* window, const PrAppFrame* frame);

// Returns the variant of Studio's theme that the kit draws in, for the user's preferences, at the start of each frame.
typedef PrKitThemeVariant (*PrStudioWindowThemeFn)(void* PR_NULLABLE context, PrStudioWindow* window,
    const PrAppUserPreferences* preferences);

// Called with each event of the window before the window's context takes it. Returns true if the application took the
// event, which the context then never sees. The window asks for a frame either way. The events of the windows that show
// the context's surfaces go to the context alone.
typedef bool (*PrStudioWindowEventFn)(void* PR_NULLABLE context, PrStudioWindow* window, const PrAppEvent* event);

// Declares a round of the application under `root`, which is a surface on the app background that fills the viewport
// below the application's title bar, if it has one. The viewport is `extent` px.
typedef void (*PrStudioWindowBuildFn)(void* PR_NULLABLE context, PrStudioWindow* window, PrLayoutNode root, PrVec2 extent);

// Declares the content of the application's own title bar in each round, in `row`, which lies between the system's
// buttons and the window's own (prKitTitleBar). The controls in it take presses, and the rest of the bar drags the
// window. A fullscreen window has no title bar, and `row` then sits at the top of the content.
typedef void (*PrStudioWindowTitleBarFn)(void* PR_NULLABLE context, PrStudioWindow* window, PrLayoutNode row);

// Called once a frame's rounds are laid out. Returns true if another frame would show something different.
typedef bool (*PrStudioWindowLaidOutFn)(void* PR_NULLABLE context, PrStudioWindow* window);

// Called once a frame is drawn, with the draw list that it played the tree's scene into, which is valid during the
// call, the extent of the frame in target pixels, and the canvas's color, which the list leaves unfilled.
typedef void (*PrStudioWindowDrawnFn)(void* PR_NULLABLE context, PrStudioWindow* window, const PrDrawList* list,
    PrUVec2 extent, PrColor canvas);

// Called as the window closes, after its last frame and before its kit is freed, whether or not it opened.
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

    // The least and the greatest extent that the user can resize the window to (prAppWindowSetExtentLimits).
    PrUVec2 minExtent;
    PrUVec2 maxExtent;

    // What the window is to the system, and how it opens (PrAppWindowDesc). A Dialog or a Popup belongs to `parent`.
    // With a backdrop other than Opaque, the window's surface has no background, and the backdrop shows wherever the
    // application's own surfaces do not cover it. A Custom decoration shows the title bar that `titleBar` fills.
    PrAppWindowRole role;
    PrStudioWindow* PR_NULLABLE parent;
    bool modal;
    PrAppWindowDecoration decoration;
    PrAppWindowBackdrop backdrop;
    bool hidden;
    bool pointerPassthrough;
    PrAppWindowPlacement placement;
    PrAppDisplayPosition position;
    PrAppPopupPlacement popup;

    // What the window's swap chain shows. Sdr, the zero value, suits a window whose colors are all in sRGB. Hdr shows
    // colors outside sRGB's gamut and brighter than SDR white, as a color picker or a wide-gamut image needs. On a
    // display that can show them, Hdr costs textures of half floats: each image of the swap chain, and a texture of the
    // window's size that the list renders into.
    PrGpuSwapChainIntent intent;

    PrStudioWindowOpenFn PR_NULLABLE open;
    PrStudioWindowThemeFn PR_NULLABLE theme;    // or null for the variant of the user's preferred color scheme
    PrStudioWindowFrameFn PR_NULLABLE beginFrame;
    PrStudioWindowEventFn PR_NULLABLE event;
    PrStudioWindowBuildFn build;
    PrStudioWindowTitleBarFn PR_NULLABLE titleBar;  // the content of a Custom title bar, or null for an empty one
    PrStudioWindowLaidOutFn PR_NULLABLE laidOut;
    PrStudioWindowDrawnFn PR_NULLABLE drawn;
    PrStudioWindowCloseFn PR_NULLABLE close;
    PrStudioWindowTraceFn PR_NULLABLE trace;
    void* PR_NULLABLE context;
} PrStudioWindowDesc;

// Creates a window of the running Studio application `app` as `desc` describes. The window opens before its first
// frame, never during this call. Returns what prAppWindowCreate returns. On failure, `*out` is null and `close` is not
// called.
PrStatus prStudioWindowCreate(PrStudioApp* app, const PrStudioWindowDesc* desc, PrStudioWindow* PR_NULLABLE* PR_NONNULL out);

// Closes the window and frees it. During one of the window's own callbacks, the window closes once the callback
// returns.
void prStudioWindowDestroy(PrStudioWindow* PR_NULLABLE window);

// The Studio application and the platform's application that the window belongs to, the platform's window, and what the
// window's kit draws with, while it runs.
PrStudioApp* prStudioWindowStudioApp(const PrStudioWindow* window);
PrApp* prStudioWindowApp(const PrStudioWindow* window);
PrAppWindow* prStudioWindowAppWindow(const PrStudioWindow* window);
PrKit* prStudioWindowKit(const PrStudioWindow* window);
PrUi* prStudioWindowUi(const PrStudioWindow* window);
PrFontSet* prStudioWindowFonts(const PrStudioWindow* window);
PrTextShaper* prStudioWindowShaper(const PrStudioWindow* window);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
