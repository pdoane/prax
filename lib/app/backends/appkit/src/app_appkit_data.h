// The AppKit backend's data transfer: the pasteboard, drags and drops, reads and writes of sources, and file dialogs.

#pragma once

#include "app/backends/appkit/src/app_appkit.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The types of a pasteboard that hold an image's own data, rather than its file.
NSArray<NSPasteboardType>* appKitImageTypes();

// Returns the operation that a drag's session reports as it ends.
PrAppDragOperation appKitOperationOf(NSDragOperation operation);

// Returns straight-alpha sRGB Rgba8 `pixels` as an image representation, or nil if AppKit cannot make one.
NSBitmapImageRep* PR_NULLABLE appKitImageRepOf(PrPixelView pixels);

// Returns the drag operations of an AppKit mask. The generic operation lets the destination choose. A destination moves
// within a volume, so the generic operation counts as a move.
PrAppDragOperationFlags appKitOperationsOf(NSDragOperation mask);

// Returns the AppKit mask of a set of drag operations.
NSDragOperation appKitMaskOf(PrAppDragOperationFlags operations);

// Returns the operation that a destination answers with, within the source's `mask`.
NSDragOperation appKitAnswerOf(PrAppDragOperation operation, NSDragOperation mask);

// Returns the forms of content that the pasteboard holds, other than a payload.
PrAppDataFormFlags appKitFormsOf(NSPasteboard* board);

// Adds the file at `url` to `out`, with its name, its path, its size and whether it is a directory.
void appKitAddFile(NSURL* url, AppDataStore& out);

// Reads every form of a drop's pasteboard, which is the system's only until the drop returns, into `out`. The image
// keeps its encoded bytes as custom content of its pasteboard type, which a read of the image decodes, so that a
// program that reads the text decodes no image.
void appKitReadDrop(NSPasteboard* board, AppDataStore& out);

// Writes `content` to `first`, the first item of a pasteboard, and its further files to items of their own, which it
// adds to `writers` after `first`. `token`, if not zero, names the application's drag of the content's payload.
void appKitWriteContent(const PrAppDataContent& content, uint64_t token, NSPasteboardItem* first,
    NSMutableArray<id<NSPasteboardWriting>>* writers);

// Returns true if the pasteboard carries the token of the drag that the application has in flight.
bool appKitCarriesDrag(NSPasteboard* board, const AppKitDrag& drag);

// Hands `answer` to the host from the main queue, which delivers it once no callback runs.
void appKitAnswerSoon(PrApp* app, AppAnswer* answer);

// Implement the application's functions of their names (PrApp::dataRetainFn and the others).
void appKitDataRetain(PrApp* app, PrAppDataSource source);
void appKitDataRelease(PrApp* app, PrAppDataSource source);
void appKitDataRead(PrApp* app, const PrAppDataRead* read);
PrStatus appKitDataWrite(PrApp* app, PrAppDataSource source, const PrAppDataContent* content);

// Writes a file's bytes on a queue of its own, and answers from the main queue. A write at offset zero empties the file
// first.
void appKitDataWriteFile(PrApp* app, PrAppDataSource source, uint32_t index, uint64_t offset, PrByteView bytes,
    PrAppDataStatusFn done, void* PR_NULLABLE context);

// Implement the application's and the window's functions of their names (PrApp::cancelFn and the others).
void appKitDataCancel(PrApp* app, void* PR_NULLABLE context);
void appKitFileDialog(PrApp* app, const PrAppFileDialogDesc* desc, PrAppFileDialogFn done, void* PR_NULLABLE context);
void appKitDragPromiseEnd(PrAppWindow* window, uint32_t index, PrStatus status);
void appKitDragStart(PrAppWindow* window, const PrAppDragDesc* desc);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
