// The WebKit backend of web views: a WKWebView, on macOS.
//
// A view shows over a host of PrSurfaceKind_NsView. While it is hidden, its page renders in a window below the desktop,
// which the window server composites and no one sees.

#pragma once

#include "pr/base.h"
#include "pr/webview.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Creates a web view whose engine is WebKit, as `desc` describes.
PrStatus prWebViewWebKitCreate(const PrWebViewDesc* desc, PrWebView* PR_NULLABLE* PR_NONNULL out);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
