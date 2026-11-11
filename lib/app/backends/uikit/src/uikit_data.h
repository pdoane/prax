// The UIKit backend's data transfer: the clipboard, the files of sources, the document picker, the share sheet, and the
// answers to their requests.

#pragma once

#include "app/backends/uikit/src/app_uikit.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Hands `answer` to the host from the main queue, which delivers it once no callback runs.
void uiKitAnswerSoon(PrApp* app, AppAnswer* answer);

// The application's functions of data transfer, and the window's share.
void uiKitDataRetain(PrApp* app, PrAppDataSource source);
void uiKitDataRelease(PrApp* app, PrAppDataSource source);
void uiKitDataRead(PrApp* app, const PrAppDataRead* read);
PrStatus uiKitDataWrite(PrApp* app, PrAppDataSource source, const PrAppDataContent* content);
void uiKitDataWriteFile(PrApp* app, PrAppDataSource source, uint32_t index, uint64_t offset, PrByteView bytes,
    PrAppDataStatusFn done, void* PR_NULLABLE context);
void uiKitFileDialog(PrApp* app, const PrAppFileDialogDesc* desc, PrAppFileDialogFn done, void* PR_NULLABLE context);
void uiKitDataCancel(PrApp* app, void* PR_NULLABLE context);
void uiKitShare(PrAppWindow* window, const PrAppDataContent* content, PrRect anchor, PrAppShareFn done,
    void* PR_NULLABLE context);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
