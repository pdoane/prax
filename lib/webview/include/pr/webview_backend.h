// The interface that web view backends implement. Callers use the functions of pr/webview.h, which call these. Only
// backends include this header.

#pragma once

#include "pr/base.h"
#include "pr/webview.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The functions of a view's backend, at the start of its state. Each implements the function of pr/webview.h that it is
// named for. A null `printFn` is an engine that does not print, and a null `processesFn` one that runs its pages in the
// program's own process.
struct PrWebView {
    void (*destroyFn)(PrWebView* view);
    void (*resizeFn)(PrWebView* view, PrUVec2 viewport);
    void (*loadFn)(PrWebView* view, PrStringView url, PrWebViewDoneFn done, void* PR_NULLABLE context);
    void (*evaluateFn)(PrWebView* view, PrStringView script, PrWebViewResultFn done, void* PR_NULLABLE context);
    void (*captureFn)(PrWebView* view, PrRect rect, PrUVec2 size, PrWebViewCaptureFn done, void* PR_NULLABLE context);
    void (*showFn)(PrWebView* view, const PrSurface* host, PrRect clip, PrRect frame);
    void (*hideFn)(PrWebView* view);
    void (*PR_NULLABLE printFn)(PrWebView* view, PrStringView path, PrVec2 paper, PrWebViewDoneFn done,
        void* PR_NULLABLE context);
    uint32_t (*PR_NULLABLE processesFn)(PrWebView* view, PrWebViewProcess* PR_NULLABLE out, uint32_t capacity);
};

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
