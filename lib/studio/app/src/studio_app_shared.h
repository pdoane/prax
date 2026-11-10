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

    // The decoder for glyphs that are images, such as those of a bitmap emoji font, and the codecs that it uses.
    PrCompressCodec* PR_NULLABLE compress;
    PrImageCodec* PR_NULLABLE images;
    PrDrawGlyphDecoder* PR_NULLABLE glyphs;

    // The device that every window renders on, the renderer and the display shader on it, and how many times the device
    // has been made, so that a window knows to make its swap chain again on a new device. The display shader is made
    // with the first frame that maps to its display, since a device without shaders, such as Canvas2d, never maps.
    PrGpuDevice* PR_NULLABLE device;
    PrDrawRenderer* PR_NULLABLE renderer;
    PrDrawDisplay* PR_NULLABLE display;
    uint32_t generation;

    RawArray<PrStudioWindow*> windows;  // in the order they opened
};

// What a platform window renders with on its application's device, made on the device of `generation`: its swap chain,
// which presents alpha if `transparent`, the frame of the renderer, and, where the swap chain holds more than 8-bit
// sRGB, the texture of half floats that the list renders into.
struct StudioWindowTarget {
    PrGpuSwapChain* PR_NULLABLE swapChain;
    PrDrawFrame* PR_NULLABLE frame;
    uint32_t generation;
    bool transparent;
    PrGpuTexture rendered;
};

struct StudioWindowState;

// A surface of a window's context that shows in a platform window of its own (prUiSetHost), as the context reported it
// at the window's last frame.
struct StudioWindowSurface {
    StudioWindowState* window;
    PrUiViewportInfo info;
    PrAppWindow* appWindow;
    StudioWindowTarget target;
    PrDrawList* list;

    // What the surface's window last drew and returned: the revision of the root's drawing
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
    PrStatus status;                // Ok, or why the window closed
    bool initial;                   // the window of the application's description, whose failure fails the run
    PrVec2 extent;                  // of the viewport, in px
    PrAppTitleBar titleBar;         // as the frame that runs reports it
    PrAppWindowBackdrop backdrop;   // as the frame that runs reports it
    PrAppWindow* appWindow;

    // The window's callbacks that are running, and whether the window closes once they return.
    uint32_t callbacks;
    bool closing;

    double lastFrame;               // when the last frame ended
    double firstEvent;              // when the oldest event that no frame has taken arrived, or zero

    PrLayoutTree* tree;
    PrUi* ui;
    PrKit* kit;
    const PrLayoutScene* PR_NULLABLE scene; // the scene of the last frame, which its list plays
    PrDrawList* list;
    RawArray<PrLayoutSceneScroll> scrolls;
    StudioWindowTarget target;

    // The surfaces of the context that show in platform windows of their own, what the context reported of them at the
    // last frame, and the window's output of the last frame, whose cursor and drop answer their windows take too.
    RawArray<StudioWindowSurface*> surfaces;
    RawArray<PrUiViewportInfo> viewports;
    PrAppOutput output;
};

// Makes the application's device and the renderer on it, freeing what the application and every window made on the
// device before, if there was one. Returns false if the platform has no device to render with.
bool studioAppMakeDevice(StudioAppState* app);

// Frees the application's device and what was made on it.
void studioAppCloseDevice(StudioAppState* app);

// Removes a window that closed from its application, which stops if the window was its last and its description says
// so.
void studioAppWindowClosed(StudioAppState* app, StudioWindowState* window);

// Frees what the target made on the device of `app`.
void studioWindowTargetClose(StudioAppState* app, StudioWindowTarget* target);

// Frees what the window and its surfaces made on their application's device.
void studioWindowCloseDevice(StudioWindowState* window);

} // namespace pr_impl

// C opaque types
struct PrStudioApp : pr_impl::StudioAppState {};
struct PrStudioWindow : pr_impl::StudioWindowState {};

PR_ASSUME_NONNULL_END
