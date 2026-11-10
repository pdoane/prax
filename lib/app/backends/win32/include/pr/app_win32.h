// The Win32 application backend: Windows windows and the Win32 message loop.
//
// A window's surface is a PrAppSurfaceKind_Hwnd. The application has a message-only window of its own, which receives
// its wakes and owns the clipboard's data, so that it runs while it has no windows.
//
// A notification is a balloon on the application's icon in the notification area, and Windows shows it as a toast. The
// first notification adds the icon, and the end of the run removes it. A new notification replaces the previous one. A
// click on the icon brings the last active window forward. A headless application posts no notifications.

#pragma once

#include "pr/app.h"
#include "pr/base.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Creates an application whose windows are Windows windows on the Win32 message loop. The process becomes aware of each
// monitor's DPI, version 2, unless it already chose an awareness.
PrApp* prAppWin32Create(const PrAppOptions* options);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
