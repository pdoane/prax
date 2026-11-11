// The UIKit backend's data transfer: the clipboard, the files of sources, the document picker, the share sheet, and the
// answers to their requests.

#pragma once

#include "app/backends/uikit/src/app_uikit.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// What answers a request: content from a store, a file's bytes, a write's status, or a dialog's source.
enum {
    UiKitAnswerKind_Store,
    UiKitAnswerKind_Bytes,
    UiKitAnswerKind_Status,
    UiKitAnswerKind_Source,
};
typedef uint8_t UiKitAnswerKind;

// The answer to the request `id`, which owns what it answers with. A Source answer holds a reference to its source.
struct UiKitAnswer {
    UiKitAnswerKind kind;
    uint32_t id;
    PrStatus status;
    AppDataStore store;
    AppDataStore* PR_NULLABLE external;     // a source's store, which a Store answer answers from in place of its own
    RawArray<uint8_t> bytes;
    PrAppDataSource source;
};

// Makes an answer of `kind` to the request `id`, or frees one.
UiKitAnswer* uiKitAnswerOf(UiKitAnswerKind kind, uint32_t id, PrStatus status);
void uiKitAnswerDestroy(UiKitAnswer* PR_NULLABLE answer);

// Hands `answer` to the host from the main queue, which delivers it once no callback runs.
void uiKitAnswerSoon(PrApp* app, UiKitAnswer* answer);

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
