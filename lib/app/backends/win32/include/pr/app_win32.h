// The Win32 application backend: a Windows window and the Win32 message loop.
//
// A notification is a balloon on the window's icon in the notification area, and Windows shows it as a toast. The first
// notification adds the icon, and the end of the run removes it. A new notification replaces the previous one. A click
// on the icon also brings the window forward. A headless window posts no notifications.

#pragma once

#include "pr/app.h"
#include "pr/base.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Creates an application that is a Windows window on the Win32 message loop, as `desc` describes. The process becomes
// aware of each monitor's DPI, version 2, unless it already chose an awareness. Its browsers are Microsoft Edge
// WebView2, where the system has the WebView2 runtime.
PrApp* prAppWin32Create(const PrAppDesc* desc);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
