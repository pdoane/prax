// The Win32 backend's shell and device: notifications, URLs, the trash, recent documents, the taskbar, power and
// keeping awake.

#pragma once

#include "app/backends/win32/src/app_win32.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Handles a user action on the icon in the notification area. A click on the balloon of the last notification delivers
// the click to the application. A click on the icon brings the last active window forward.
void win32NotifyIconAction(Win32App* app, UINT action);

// Returns the taskbar, created on first use, or null if the shell has none.
ITaskbarList3* PR_NULLABLE win32TaskbarOf(Win32App* app);

// Shows the window's progress and the application's badge on the window's taskbar button.
void win32ApplyTaskbar(Win32Window* window);

// Copies `text` to `out`, a NUL-terminated field of `capacity` UTF-16 units. Text that does not fit is cut before the
// first character that does not fit, so a surrogate pair is never split.
void win32CopyField(Win32App* app, PrStringView text, wchar_t* out, size_t capacity);

// A notification is a balloon on the application's icon in the notification area, which Windows 10 and later show as a
// toast. A real toast needs an application user model ID, which only a packaged or installed application has. A balloon
// needs nothing from the process. A new balloon replaces the previous one.
PrStatus win32NotificationPost(PrApp* base, const PrAppNotificationDesc* desc);

// Implements prAppOpenUrl.
PrStatus win32OpenUrl(PrApp* base, PrStringView url);

// Implements prAppRevealPath.
PrStatus win32RevealPath(PrApp* base, PrStringView path);

// Implements prAppTrashPath.
PrStatus win32TrashPath(PrApp* base, PrStringView path);

// Implements prAppRecentDocumentAdd.
void win32RecentDocumentAdd(PrApp* base, PrStringView path);

// Implements prAppRecentDocumentsClear.
void win32RecentDocumentsClear(PrApp*);

// Implements prAppSetBadge.
void win32SetBadge(PrApp* base, PrStringView badge);

// Implements prAppPower.
PrAppPower win32Power(const PrApp*);

// Implements prAppSetAwake.
void win32SetAwake(PrApp*, PrAppAwakeFlags flags);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
