// The Win32 backend's browser: WebView2, offscreen or shown over the application's window.

#pragma once

#include "pr/app.h"
#include "pr/string_view.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The browser functions of the Win32 application's record. Each implements the function that it is named for.
void win32BrowserDestroy(PrApp* app, PrAppBrowser* browser);
void win32BrowserResize(PrApp* app, PrAppBrowser* browser, PrUVec2 viewport);
void win32BrowserShow(PrApp* app, PrAppBrowser* browser, PrRect clip, PrRect frame);
void win32BrowserHide(PrApp* app, PrAppBrowser* browser);
void win32BrowserLoad(PrApp* app, PrAppBrowser* browser, PrStringView url, PrAppBrowserDoneFn done, void* PR_NULLABLE context);
void win32BrowserEvaluate(PrApp* app, PrAppBrowser* browser, PrStringView script, PrAppBrowserResultFn done,
    void* PR_NULLABLE context);
void win32BrowserCapture(PrApp* app, PrAppBrowser* browser, PrRect rect, PrUVec2 size, PrAppBrowserCaptureFn done,
    void* PR_NULLABLE context);
void win32BrowserPrint(PrApp* app, PrAppBrowser* browser, PrStringView path, PrVec2 paper, PrAppBrowserDoneFn done,
    void* PR_NULLABLE context);
uint32_t win32BrowserProcesses(PrApp* app, PrAppBrowser* browser, PrAppBrowserProcess* PR_NULLABLE out, uint32_t capacity);

// Creates a browser that can show its page over `host`, the application's HWND. Returns NotSupported if the system has
// no WebView2 runtime.
PrStatus win32BrowserCreate(PrApp* app, void* PR_NULLABLE host, const PrAppBrowserDesc* desc,
    PrAppBrowser* PR_NULLABLE* PR_NONNULL out);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
