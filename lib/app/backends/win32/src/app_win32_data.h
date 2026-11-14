// The Win32 backend's data transfer: the clipboard, drags and drops, reads and writes of sources, and file dialogs.

#pragma once

#include "app/backends/win32/src/app_win32.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Readies the drags of the application's run, once OLE runs on its thread: the clipboard formats that they carry, the
// shell's helper that shows a drag's image over the windows, and the objects that carry them.
void win32DragsStart(Win32App* app);

// Ends the drags of the run, and frees the content of the last drag that a window started.
void win32DragsStop(Win32App* app);

// Makes the window a target of drags, or no longer one.
void win32DragsRegister(Win32Window* window);
void win32DragsRevoke(Win32Window* window);

// Starts the drag that prAppWindowDragStart asked for, once no callback runs. A drag starts only while the primary
// button's press is still down. OLE's loop carries the drag and returns once the drag ends. The loop dispatches the
// windows' messages, so frames and events go on while it runs.
void win32StartDrag(Win32Window* window);

// Answers a posted request, and frees it.
void win32AnswerRequest(Win32App* app, Win32Request* request);

// Shows the dialog of `request` in its modal loop, and answers its read with what the user chose. A dialog whose read
// was cancelled before it showed does not show.
void win32ShowDialog(Win32App* app, Win32DialogRequest* request);

// Closes the dialog that shows if its read was cancelled.
void win32CloseCancelledDialog(Win32App* app);

// A cancel may be of the dialog that shows, which the message window closes on the application's thread.
void win32DataCancel(PrApp* base, void* PR_NULLABLE context);

// Implements prAppDataRetain.
void win32DataRetain(PrApp* base, PrAppDataSource source);

// Implements prAppDataRelease.
void win32DataRelease(PrApp* base, PrAppDataSource source);

// Implements the reads of pr/app_data.h.
void win32DataRead(PrApp* base, const PrAppDataRead* read);

// Implements prAppDataWrite.
PrStatus win32DataWrite(PrApp* base, PrAppDataSource source, const PrAppDataContent* content);

// Implements prAppDataWriteFile.
void win32DataWriteFile(PrApp* base, PrAppDataSource source, uint32_t index, uint64_t offset, PrByteView bytes,
    PrAppDataStatusFn done, void* PR_NULLABLE context);

// Records the dialog, which shows from the message window once no callback runs. The types' extensions become the
// shell's patterns, as "*.png;*.jpg".
void win32FileDialog(PrApp* base, const PrAppFileDialogDesc* desc, PrAppFileDialogFn done, void* PR_NULLABLE context);

// Implements prAppWindowShare.
void win32Share(PrAppWindow* window, const PrAppDataContent*, PrRect, PrAppShareFn done, void* PR_NULLABLE context);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
