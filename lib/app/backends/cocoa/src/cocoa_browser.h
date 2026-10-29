// The Cocoa backend's browser: WebKit, offscreen or shown over the application's view.

#pragma once

#include "pr/app.h"
#include "pr/string_view.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The browser functions of the Cocoa application's record. Each implements the function that it is named for.
void cocoaBrowserDestroy(PrApp* app, PrAppBrowser* browser);
void cocoaBrowserResize(PrApp* app, PrAppBrowser* browser, PrUVec2 viewport);
void cocoaBrowserShow(PrApp* app, PrAppBrowser* browser, PrRect clip, PrRect frame);
void cocoaBrowserHide(PrApp* app, PrAppBrowser* browser);
void cocoaBrowserLoad(PrApp* app, PrAppBrowser* browser, PrStringView url, PrAppBrowserDoneFn done,
                      void* PR_NULLABLE context);
void cocoaBrowserEvaluate(PrApp* app, PrAppBrowser* browser, PrStringView script, PrAppBrowserResultFn done,
                          void* PR_NULLABLE context);
void cocoaBrowserCapture(PrApp* app, PrAppBrowser* browser, PrRect rect, PrUVec2 size, PrAppBrowserCaptureFn done,
                         void* PR_NULLABLE context);
void cocoaBrowserPrint(PrApp* app, PrAppBrowser* browser, PrStringView path, PrVec2 paper, PrAppBrowserDoneFn done,
                       void* PR_NULLABLE context);
uint32_t cocoaBrowserProcesses(PrApp* app, PrAppBrowser* browser, PrAppBrowserProcess* PR_NULLABLE out,
                               uint32_t capacity);

// Creates a browser that can show its page over `host`, the application's NSView.
PrStatus cocoaBrowserCreate(PrApp* app, void* PR_NULLABLE host, const PrAppBrowserDesc* desc,
                            PrAppBrowser* PR_NULLABLE* PR_NONNULL out);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
