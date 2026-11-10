// The WebView2 backend of web views: Microsoft Edge WebView2, on Windows, where the system has the WebView2 runtime.
//
// A view shows over a host of PrAppSurfaceKind_Hwnd. While it is hidden, its page renders in a window of its own beyond
// the left edge of the desktop, which no one sees.

#pragma once

#include "pr/base.h"
#include "pr/webview.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Creates a web view whose engine is WebView2, as `desc` describes. Returns NotSupported if the system has no WebView2
// runtime.
PrStatus prWebViewWebView2Create(const PrWebViewDesc* desc, PrWebView* PR_NULLABLE* PR_NONNULL out);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
