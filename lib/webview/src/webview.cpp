// Web views: the interface's contract around each backend's functions.

#include "pr/webview.h"

#include "pr/ascii.h"
#include "pr/assert.h"
#include "pr/webview_backend.h"

#if defined(PR_WEBVIEW_HAS_WEBKIT)
#include "pr/webview_webkit.h"
#endif
#if defined(PR_WEBVIEW_HAS_WEBVIEW2)
#include "pr/webview_webview2.h"
#endif

namespace pr_impl {

namespace {

constexpr PrStringView kBackendNames[PrWebViewBackend_Count] = {"default", "webkit", "webview2"};

// The backends that the build has, which Default creates the first of, and then Default, so that a build without
// backends creates none.
constexpr PrWebViewBackend kBuilt[] = {
#if defined(PR_WEBVIEW_HAS_WEBKIT)
    PrWebViewBackend_WebKit,
#endif
#if defined(PR_WEBVIEW_HAS_WEBVIEW2)
    PrWebViewBackend_WebView2,
#endif
    PrWebViewBackend_Default,
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrWebViewBackend prWebViewBackendFromName(PrStringView name) {
    for (PrWebViewBackend backend = 0; backend < PrWebViewBackend_Count; ++backend) {
        if (prAsciiEqualsFold(kBackendNames[backend], name))
            return backend;
    }
    return PrWebViewBackend_Count;
}

PrStringView prWebViewBackendName(PrWebViewBackend backend) {
    return backend < PrWebViewBackend_Count ? kBackendNames[backend] : PrStringView{};
}

bool prWebViewBackendIsBuilt(PrWebViewBackend backend) {
    for (PrWebViewBackend built : kBuilt) {
        if (built != PrWebViewBackend_Default && (built == backend || backend == PrWebViewBackend_Default))
            return true;
    }
    return false;
}

PrStatus prWebViewCreate(PrWebViewBackend backend, const PrWebViewDesc* desc, PrWebView* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    PR_ASSERT(desc->pixelRatio >= 0);
    PrStatus status = PrStatus_NotSupported;
    switch (backend == PrWebViewBackend_Default ? kBuilt[0] : backend) {
#if defined(PR_WEBVIEW_HAS_WEBKIT)
        case PrWebViewBackend_WebKit: status = prWebViewWebKitCreate(desc, out); break;
#endif
#if defined(PR_WEBVIEW_HAS_WEBVIEW2)
        case PrWebViewBackend_WebView2: status = prWebViewWebView2Create(desc, out); break;
#endif
        default: break;
    }
    PR_ASSERT((status == PrStatus_Ok) == (*out != nullptr));
    return status;
}

void prWebViewDestroy(PrWebView* PR_NULLABLE view) {
    if (view != nullptr)
        view->destroyFn(view);
}

void prWebViewResize(PrWebView* view, PrUVec2 viewport) {
    view->resizeFn(view, viewport);
}

void prWebViewLoad(PrWebView* view, PrStringView url, PrWebViewDoneFn done, void* PR_NULLABLE context) {
    view->loadFn(view, url, done, context);
}

void prWebViewEvaluate(PrWebView* view, PrStringView script, PrWebViewResultFn done, void* PR_NULLABLE context) {
    view->evaluateFn(view, script, done, context);
}

void prWebViewCapture(PrWebView* view, PrRect rect, PrUVec2 size, PrWebViewCaptureFn done, void* PR_NULLABLE context) {
    view->captureFn(view, rect, size, done, context);
}

void prWebViewShow(PrWebView* view, const PrAppSurface* host, PrRect clip, PrRect frame) {
    view->showFn(view, host, clip, frame);
}

void prWebViewHide(PrWebView* view) {
    view->hideFn(view);
}

PrStatus prWebViewPrint(PrWebView* view, PrStringView path, PrVec2 paper, PrWebViewDoneFn done, void* PR_NULLABLE context) {
    if (view->printFn == nullptr)
        return PrStatus_NotSupported;
    view->printFn(view, path, paper, done, context);
    return PrStatus_Ok;
}

uint32_t prWebViewProcesses(PrWebView* view, PrWebViewProcess* PR_NULLABLE out, uint32_t capacity) {
    return view->processesFn != nullptr ? view->processesFn(view, out, capacity) : 0;
}
