// What Studio's applications and windows share: the state of each, which each reaches in the other, and the device that
// the windows render on.

#pragma once

#include "pr/compress.h"
#include "pr/draw_display.h"
#include "pr/draw_glyph.h"
#include "pr/draw_list.h"
#include "pr/draw_renderer.h"
#include "pr/gpu.h"
#include "pr/image.h"
#include "pr/kit.h"
#include "pr/layout_scene.h"
#include "pr/raw_array.h"
#include "pr/studio_app.h"
#include "pr/studio_window.h"
#include "pr/text.h"
#include "pr/time.h"
#include "pr/ui.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// A Studio application while it runs: its description, its platform application, and what its windows share.
struct StudioAppState {
    PrStudioAppDesc desc;
    PrApp* app;
    PrStatus status;                // Ok, or why the run failed
    bool stopping;                  // the run is shutting down, and the last window's close stops nothing

    PrTextShaper* shaper;

    // The paths of the kits' icons, which every window's kit draws (PrKitTheme::paths).
    PrPathCache* paths;

    // The codecs from the application's description. They decode the glyphs that are pictures, such as the glyphs of a
    // bitmap emoji font or an SVG font.
    const PrCompressCodecSet* PR_NULLABLE compress;
    const PrImageCodecSet* PR_NULLABLE images;

    // `textures` is the cache that realizes the windows' images. `glyphs` is the glyph decoder, which realizes its
    // images in the same cache. The cache lives for the whole run, so every kit has it from its creation, and its
    // address and its generation stay meaningful across devices. It has no device until the first device is ready, and
    // between a lost device and the next one. The cache realizes images on the description's task pool, and it wakes
    // the application when an image arrives.
    PrTextureCache* textures;
    PrDrawGlyphDecoder* PR_NULLABLE glyphs;

    // The device that every window renders on, the renderer on it, and how many times the renderer has been made. A
    // window compares the count with its own to know when to make its swap chain again. The device's creation runs on
    // the description's pool. The display shader is made on the device for the first frame whose swap chain holds more
    // than 8-bit sRGB (studioAppDisplay), because a device that never makes such a swap chain, such as a canvas's, may
    // have no shaders to run it.
    PrGpuDevice* PR_NULLABLE device;
    PrDrawRenderer* PR_NULLABLE renderer;
    PrDrawDisplay* PR_NULLABLE display;
    uint32_t generation;

    // Ok, or why there is nothing to render with, from a failure until the device renders again: through the creation
    // of the device that replaces a lost one, and not through the first.
    PrStatus deviceStatus;

    RawArray<PrStudioWindow*> windows;  // in the order they opened
};

// What a platform window renders with on its application's device, made on the device of `generation`: its swap chain,
// which presents alpha if `transparent`, the frame of the renderer, and, where the swap chain holds more than 8-bit
// sRGB, the texture of half floats that the list renders into.
struct StudioWindowTarget {
    PrGpuSwapChain* PR_NULLABLE swapChain;
    PrDrawFrame* PR_NULLABLE frame;
    uint32_t generation;
    PrGpuSwapChainIntent intent;
    bool transparent;
    PrGpuTexture rendered;
};

struct StudioWindowState;

// A portal of a window's context that shows in a platform window of its own (prUiSetHost), as the context reported it
// at the window's last frame.
struct StudioWindowPortal {
    StudioWindowState* window;
    PrUiViewportInfo info;
    PrAppWindow* appWindow;
    StudioWindowTarget target;
    PrDrawList* list;

    // What the portal's window last drew and returned: the revision of the root's drawing
    // (prLayoutNodeDrawingRevision), the cursor and the drop answer. The window draws again only when one of them
    // changes.
    uint64_t drawn;
    PrAppCursor cursor;
    PrAppDragOperation dropOperation;
};

// A window while it runs: its description, its kit and what the kit draws with, and what it renders with on its
// application's device.
struct StudioWindowState {
    PrStudioApp* studio;
    PrStudioWindowDesc desc;
    PrStatus status;                // Ok, or why the window could not open
    PrStatus drawStatus;            // Ok, or why the window does not draw, as the application last heard
    bool initial;                   // the window of the application's description, whose failure fails the run
    PrVec2 extent;                  // of the viewport, in px
    PrAppTitleBar titleBar;         // as the frame that runs reports it
    PrAppInsets safeArea;           // as the frame that runs reports it
    PrAppWindowBackdrop backdrop;   // as the frame that runs reports it
    bool transparent;               // the swap chain presents alpha, for a transparent window
    bool framed;                    // the window began a frame since the application last trimmed its texture cache
    PrAppWindow* appWindow;

    // The window's callbacks that are running, and whether the window closes once they return.
    uint32_t callbacks;
    bool closing;

    PrTimePerf lastFrame;           // when the last frame ended, or zero ticks before the first
    PrTimePerf firstEvent;          // when the oldest event that no frame has taken arrived, or zero ticks

    PrLayoutTree* tree;
    PrUi* ui;
    PrKit* kit;
    const PrLayoutScene* PR_NULLABLE scene; // the scene of the last frame, which its list plays
    PrDrawList* list;
    RawArray<PrLayoutSceneScroll> scrolls;
    StudioWindowTarget target;

    // The portals of the context that show in platform windows of their own, what the context reported of them at the
    // last frame, and the window's output of the last frame, whose cursor and drop answer their windows take too.
    RawArray<StudioWindowPortal*> portals;
    RawArray<PrUiViewportInfo> viewports;
    PrAppOutput output;

    // A capture that the application requested (prStudioWindowCapture): the function that takes it and its context, and
    // whether the next frame takes it.
    PrStudioWindowCaptureFn PR_NULLABLE capture;
    void* PR_NULLABLE captureContext;
    bool capturePending;
};

// Creates the application's texture cache and glyph decoder, which have no device yet. The description, the codecs and
// the platform's application are set.
void studioAppOpen(StudioAppState* app);

// Returns true if the application's device is ready and has a renderer. Otherwise frees a lost device, which includes
// one whose creation failed, with everything that the application and its windows made on it, and creates another.
// Makes the renderer once the device is ready. Keeps deviceStatus. A window renders only if this returns true.
bool studioAppMakeDevice(StudioAppState* app);

// Returns true while the application's device is being created. The end of the creation wakes the application.
bool studioAppCreatingDevice(const StudioAppState* app);

// Returns the display shader on the application's device, made on first use, or null if the device cannot run it.
PrDrawDisplay* PR_NULLABLE studioAppDisplay(StudioAppState* app);

// Frees the application's device and what was made on it. The texture cache stays, without its textures and without a
// device, and moves to the next device.
void studioAppCloseDevice(StudioAppState* app);

// Frees the application's device and what was made on it, the texture cache and the glyph decoder included. Every
// window must have closed.
void studioAppClose(StudioAppState* app);

// Begins a frame of `window`, and trims the application's texture cache if the frame also begins a frame of the
// application.
//
// The windows share the cache, and a trim cancels and evicts what was not asked for since the trim before it. A trim
// after each window's frame would therefore take what the other windows asked for. A frame of the application is
// instead a run of window frames in which no window draws twice. It ends, and the cache is trimmed, when a window
// begins a second frame.
//
// A window that draws no frame asks for nothing. If the trim cancels a realization, such a window may be the one that
// waits for it, so each of them is asked for a frame, in which it asks again.
void studioAppBeginWindowFrame(StudioAppState* app, StudioWindowState* window, double time);

// Removes a window that closed from its application, which stops if the window was its last and its description says
// so.
void studioAppWindowClosed(StudioAppState* app, StudioWindowState* window);

// Frees what the target made on the device of `app`.
void studioWindowTargetClose(StudioAppState* app, StudioWindowTarget* target);

// Frees what the window and its portals made on the application's device. The device is lost or is about to be freed.
// Removes the texture cache from the window's kit. The kit receives the cache again in the next frame that renders.
void studioWindowCloseDevice(StudioWindowState* window);

} // namespace pr_impl

// C opaque types
struct PrStudioApp : pr_impl::StudioAppState {};
struct PrStudioWindow : pr_impl::StudioWindowState {};

PR_ASSUME_NONNULL_END
