// Studio's applications: a platform application (pr/app.h) and what its Studio windows share (pr/studio_window.h).
//
// An application makes, for all of its windows, the platform's GPU device, the renderer and the display shader on it,
// the platform's text shaper, and the decoder of glyphs that are images, over the font set that the program provides.
// It creates the platform application, the device and the renderer with their selectors' Default backends, so the
// environment chooses them (pr/backend.h). Each window renders into its own swap chain on the shared device, on the
// thread that runs the application. A lost device is made again for every window.
//
// A wake (prAppWake) asks every window for a frame, which takes up what woke the application. The application stops as
// its last window closes, unless its description keeps it running.
//
// An application is used from the thread that runs it, except for prAppWake on its platform application.

#pragma once

#include "pr/app.h"
#include "pr/base.h"
#include "pr/draw_renderer.h"
#include "pr/font_set.h"
#include "pr/gpu.h"
#include "pr/status.h"
#include "pr/studio_window.h"
#include "pr/task.h"
#include "pr/text.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Called once the application runs and its device is made, after the window of the description opens, if it has one.
// Returns false if the application cannot run, which stops the run.
typedef bool (*PrStudioAppStartFn)(void* PR_NULLABLE context, PrStudioApp* app);

// Called with the platform application's events (PrAppHandler::event), after the application takes up a wake.
typedef void (*PrStudioAppEventFn)(void* PR_NULLABLE context, PrStudioApp* app, const PrAppEvent* event);

// Called as the run stops, while the windows that are still open stay open. They close once it returns.
typedef void (*PrStudioAppShutdownFn)(void* PR_NULLABLE context, PrStudioApp* app);

// What the application does as its last window closes.
enum {
    PrStudioAppLastWindow_Stop,     // the application stops
    PrStudioAppLastWindow_Run,      // the application runs on with no windows, as a Mac application does
};
typedef uint8_t PrStudioAppLastWindow;

// What an application is made with, and its callbacks.
typedef struct PrStudioAppDesc {
    // The fonts for every window's text. The program owns them, and they must outlive the run.
    PrFontSet* fonts;

    // Options for the platform application (prAppCreate), or null for the defaults.
    const PrAppOptions* PR_NULLABLE options;

    // The task pool that creates the device and loads the images that the windows draw. If it is null, the device is
    // created when the run starts (PrGpuDeviceOptions), and each image is loaded in the frame that first draws it.
    PrTaskPool* PR_NULLABLE pool;

    // The image codecs that decode the windows' images and the fonts' image glyphs, such as a bitmap emoji font's, and
    // the compression codecs that they decompress through, or null for none. The program chooses the formats
    // (pr/image_select.h makes a set of every one), so that it carries only those. They must outlive the run.
    const PrCompressCodecSet* PR_NULLABLE compress;
    const PrImageCodecSet* PR_NULLABLE images;

    // A window that the application opens as it starts, or null for none.
    const PrStudioWindowDesc* PR_NULLABLE window;

    PrStudioAppLastWindow lastWindow;
    PrStudioAppStartFn PR_NULLABLE start;
    PrStudioAppEventFn PR_NULLABLE event;
    PrStudioAppShutdownFn PR_NULLABLE shutdown;
    void* PR_NULLABLE context;
} PrStudioAppDesc;

// Runs the application that `desc` describes until it stops, then frees what the run created. Returns NotSupported if
// the build has no application backend, or if `desc` has no pool and the device cannot be created. Returns InvalidState
// if the description's window failed to open, and Ok otherwise. The windows render once the device is ready. If the
// device cannot be created on the pool, a later frame tries again, as it does for a lost device.
PrStatus prStudioAppRun(const PrStudioAppDesc* desc);

// Stops the run, as prAppStop does.
void prStudioAppStop(PrStudioApp* app);

// The platform's application, and what the windows share, while the application runs. The device and the renderer are
// null until the device is ready, and after a loss until it is made again.
PrApp* prStudioAppApp(const PrStudioApp* app);
PrFontSet* prStudioAppFonts(const PrStudioApp* app);
PrTextShaper* prStudioAppShaper(const PrStudioApp* app);
PrGpuDevice* PR_NULLABLE prStudioAppDevice(const PrStudioApp* app);
PrDrawRenderer* PR_NULLABLE prStudioAppRenderer(const PrStudioApp* app);
const PrCompressCodecSet* PR_NULLABLE prStudioAppCompress(const PrStudioApp* app);
const PrImageCodecSet* PR_NULLABLE prStudioAppImages(const PrStudioApp* app);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
