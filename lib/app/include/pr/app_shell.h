// The shell: notifications, the system's handlers for URLs and files, sharing, recent documents and the application's
// badge.

#pragma once

#include "pr/app_application.h"
#include "pr/app_data.h"
#include "pr/base.h"
#include "pr/geom.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// A desktop notification. The title and body are UTF-8.
typedef struct PrAppNotificationDesc {
    PrStringView title;
    PrStringView body;
    uint64_t tag;               // the program's value, which the Notification event reports when the user clicks
} PrAppNotificationDesc;

// Posts a notification, which the system copies. When the user clicks it, the application activates and receives a
// Notification event with its tag. The system may ask for the user's permission on the first post. The system decides
// how to show the notification, and may withhold it, for example during Do Not Disturb.
//
// Returns NotSupported if the backend or the process cannot post notifications, NotAllowed if the user refused them,
// InvalidState if the application is not running, and Io if the system refuses.
PrStatus prAppNotificationPost(PrApp* app, const PrAppNotificationDesc* desc);

// Opens `url` with the system's handler for it. Returns Syntax if `url` is not a URL, and Io if the system cannot open
// it.
PrStatus prAppOpenUrl(PrApp* app, PrStringView url);

// Shows the file or directory at `path`, selected, in the system's file manager. Returns NotFound if nothing exists at
// the path, NotSupported if the backend has no file manager, and Io if the system refuses.
PrStatus prAppRevealPath(PrApp* app, PrStringView path);

// Moves the file or directory at `path` to the trash. Returns NotFound if nothing exists at the path, NotSupported if
// the backend has no trash, and Io if the system refuses.
PrStatus prAppTrashPath(PrApp* app, PrStringView path);

// Receives the result of a share. The status is Ok both if the user shared and if the user closed the sheet without
// choosing a service.
typedef void (*PrAppShareFn)(void* PR_NULLABLE context, PrStatus status);

// Shows the system's share sheet for a copy of `content`, where the user picks a service such as Mail, Messages or
// AirDrop. `anchor` is the rectangle that the sheet points to, such as the share button, in the window's logical
// pixels. `done` receives NotSupported if the backend has no share sheet.
void prAppWindowShare(PrAppWindow* window, const PrAppDataContent* content, PrRect anchor, PrAppShareFn done,
    void* PR_NULLABLE context);

// Adds `path` to the application's recent documents, or clears the list. The system shows the list in the menu of the
// Dock icon or the taskbar button.
void prAppRecentDocumentAdd(PrApp* app, PrStringView path);
void prAppRecentDocumentsClear(PrApp* app);

// Shows `badge` on the application's Dock icon or taskbar button, such as a count of unread messages. An empty badge
// removes it.
void prAppSetBadge(PrApp* app, PrStringView badge);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
