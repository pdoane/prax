// Data transfer: the clipboard, the selection, drag and drop, files that the user chooses or opens, and file dialogs.
//
// The program reads content from a source. The clipboard and the selection are sources that last as long as the
// application. The system hands the program other sources in an event or a callback, such as a drop, the answer of a
// file dialog, or a request to open files. Such a source is valid during that event or callback. The program retains a
// source to keep it longer, and releases it when done. prAppEventRetain keeps the source that an event hands over. A
// pending read holds a reference to its source.
//
// Reads are asynchronous. A read returns immediately, and its callback runs later on the application's thread, never
// during the read or during another callback. The callback receives the read's status: Ok, NotAllowed if the user or
// the system refused the read, NotSupported if the platform cannot read the form, or NotFound if the source has no such
// file. The content that a callback receives is valid during the call.
//
// Any thread may start a read. prAppCancel cancels the reads of a context.

#pragma once

#include "pr/app_application.h"
#include "pr/base.h"
#include "pr/byte_view.h"
#include "pr/pixels.h"
#include "pr/string_view.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// A source of content.
typedef uint64_t PrAppDataSource;

// The sources that always exist. A read of None answers with no content. The selection is the text that the user most
// recently selected, which a middle click pastes on X11 and Wayland. On a platform without a selection, it is empty.
enum {
    kPrAppDataSourceNone = 0,
    kPrAppDataSourceClipboard = 1,
    kPrAppDataSourceSelection = 2,
};

// The forms of content. Content may hold the same thing in several forms, such as a link as both a URL and text.
enum {
    PrAppDataFormFlag_Text = 1 << 0,
    PrAppDataFormFlag_Html = 1 << 1,
    PrAppDataFormFlag_Urls = 1 << 2,
    PrAppDataFormFlag_Files = 1 << 3,
    PrAppDataFormFlag_Image = 1 << 4,
    PrAppDataFormFlag_Custom = 1 << 5,
    PrAppDataFormFlag_Payload = 1 << 6,
};
typedef uint8_t PrAppDataFormFlags;

// Returns the name of one data form flag, as in "Html", or the null view if `flag` is not exactly one.
PrStringView prAppDataFormName(PrAppDataFormFlags flag);

// Content of a type that the program names, such as "org.prax.studio.nodes". Other processes that know the type can
// read its bytes.
typedef struct PrAppDataCustom {
    PrStringView type;
    PrByteView bytes;
} PrAppDataCustom;

// A value that content carries between the windows of one application, such as a tab that moves to another window. The
// program chooses both members, and only the application that wrote them can read them.
typedef struct PrAppDataPayload {
    uint64_t kind;
    uint64_t value;
} PrAppDataPayload;

// Content in each form that `forms` lists. Text and HTML are UTF-8, and URLs are absolute. Each path is the absolute
// UTF-8 path of a file or directory. The image is straight-alpha sRGB Rgba8.
typedef struct PrAppDataContent {
    PrAppDataFormFlags forms;
    PrStringView text;
    PrStringView html;
    const PrStringView* PR_NULLABLE urls;
    uint32_t urlCount;
    const PrStringView* PR_NULLABLE paths;
    uint32_t pathCount;
    PrPixelView image;
    const PrAppDataCustom* PR_NULLABLE custom;
    uint32_t customCount;
    PrAppDataPayload payload;
} PrAppDataContent;

// A file in a source. `name` is the file name that the user sees. `path` is the absolute path, or empty if the platform
// does not give one, as on the web and on mobile systems. The program then reads and writes the file through the source
// (prAppDataReadFile). `size` is in bytes, and zero for a directory.
typedef struct PrAppDataFile {
    PrStringView name;
    PrStringView path;
    uint64_t size;
    bool directory;
} PrAppDataFile;

// Retains `source` beyond the event or callback that handed it over, or releases it. Each retain needs a matching
// release. Calls on the clipboard, the selection and None do nothing.
void prAppDataRetain(PrApp* app, PrAppDataSource source);
void prAppDataRelease(PrApp* app, PrAppDataSource source);

// ---------------------------------------------------------------------------------------------------------------------
// Reading

// The callbacks of reads. Each one receives the read's status and content. The content is empty if the source does not
// hold that form.
typedef void (*PrAppDataFormsFn)(void* PR_NULLABLE context, PrStatus status, PrAppDataFormFlags forms);
typedef void (*PrAppDataTextFn)(void* PR_NULLABLE context, PrStatus status, PrStringView text);
typedef void (*PrAppDataUrlsFn)(void* PR_NULLABLE context, PrStatus status, const PrStringView* PR_NULLABLE urls,
    uint32_t count);
typedef void (*PrAppDataFilesFn)(void* PR_NULLABLE context, PrStatus status, const PrAppDataFile* PR_NULLABLE files,
    uint32_t count);
typedef void (*PrAppDataImageFn)(void* PR_NULLABLE context, PrStatus status, PrPixelView pixels);
typedef void (*PrAppDataBytesFn)(void* PR_NULLABLE context, PrStatus status, PrByteView bytes);

// Starts a read of the forms that `source` holds, or of its content in one form. A read of custom content names the
// type.
void prAppDataReadForms(PrApp* app, PrAppDataSource source, PrAppDataFormsFn done, void* PR_NULLABLE context);
void prAppDataReadText(PrApp* app, PrAppDataSource source, PrAppDataTextFn done, void* PR_NULLABLE context);
void prAppDataReadHtml(PrApp* app, PrAppDataSource source, PrAppDataTextFn done, void* PR_NULLABLE context);
void prAppDataReadUrls(PrApp* app, PrAppDataSource source, PrAppDataUrlsFn done, void* PR_NULLABLE context);
void prAppDataReadFiles(PrApp* app, PrAppDataSource source, PrAppDataFilesFn done, void* PR_NULLABLE context);
void prAppDataReadImage(PrApp* app, PrAppDataSource source, PrAppDataImageFn done, void* PR_NULLABLE context);
void prAppDataReadCustom(PrApp* app, PrAppDataSource source, PrStringView type, PrAppDataBytesFn done, void* PR_NULLABLE context);

// Starts a read of up to `size` bytes at `offset` in the file at `index` of the source's files. A read that reaches the
// end of the file answers with fewer bytes. A read of a directory answers NotSupported.
void prAppDataReadFile(PrApp* app, PrAppDataSource source, uint32_t index, uint64_t offset, uint64_t size,
    PrAppDataBytesFn done, void* PR_NULLABLE context);

// ---------------------------------------------------------------------------------------------------------------------
// Writing

// Receives the status of a write.
typedef void (*PrAppDataStatusFn)(void* PR_NULLABLE context, PrStatus status);

// Replaces the content of the clipboard or the selection with a copy of `content`. Returns InvalidData for any other
// source, NotSupported if the platform has no selection, and Io if the system refuses. Any thread may call it.
PrStatus prAppDataWrite(PrApp* app, PrAppDataSource source, const PrAppDataContent* content);

// Writes `text` as the only form of content, like prAppDataWrite.
PrStatus prAppDataWriteText(PrApp* app, PrAppDataSource source, PrStringView text);

// Starts a write of `bytes` at `offset` in the file at `index` of the source's files, such as the file in the answer of
// a Save dialog (prAppFileDialog). A write at offset zero first truncates the file. `done` receives Ok, NotAllowed if
// the system refuses, or Io.
void prAppDataWriteFile(PrApp* app, PrAppDataSource source, uint32_t index, uint64_t offset, PrByteView bytes,
    PrAppDataStatusFn done, void* PR_NULLABLE context);

// ---------------------------------------------------------------------------------------------------------------------
// Drags

// What the target of a drop does with the content: copies it, moves it, or links to it. After a move, the source
// removes the content when the drag ends.
enum {
    PrAppDragOperation_None,
    PrAppDragOperation_Copy,
    PrAppDragOperation_Move,
    PrAppDragOperation_Link,
};
typedef uint8_t PrAppDragOperation;

// Returns the name of a drag operation, as in "Copy", or the null view if `operation` is not one.
PrStringView prAppDragOperationName(PrAppDragOperation operation);

// A set of drag operations, with the bit 1 << operation for each.
enum {
    PrAppDragOperationFlag_Copy = 1 << PrAppDragOperation_Copy,
    PrAppDragOperationFlag_Move = 1 << PrAppDragOperation_Move,
    PrAppDragOperationFlag_Link = 1 << PrAppDragOperation_Link,
    PrAppDragOperationFlag_All =
        PrAppDragOperationFlag_Copy | PrAppDragOperationFlag_Move | PrAppDragOperationFlag_Link,
};
typedef uint8_t PrAppDragOperationFlags;

// A file that a drag promises to create when it is dropped. For example, the program can drag an image that it draws,
// and create the image file only when the user drops it on the desktop. `name` is the file name, with its extension.
typedef struct PrAppDragPromise {
    PrStringView name;
} PrAppDragPromise;

// The description of a drag that a window starts (prAppWindowDragStart).
//
// The system offers `content` to every application. A drop of one of the `promises` asks the program for the file's
// content (PrAppEventKind_DragPromise). `operations` lists the operations that the source allows. `preview` is the
// image that follows the pointer, in device pixels at the frame's pixel ratio, or empty for none. `previewOffset` is
// the pointer's position on the preview, in logical pixels from its top left.
typedef struct PrAppDragDesc {
    PrAppDataContent content;
    const PrAppDragPromise* PR_NULLABLE promises;
    uint32_t promiseCount;
    PrAppDragOperationFlags operations;
    PrPixelView preview;
    PrVec2 previewOffset;
} PrAppDragDesc;

// ---------------------------------------------------------------------------------------------------------------------
// File dialogs

// The purpose of a file dialog: choosing files or directories to open, or a file to save.
enum {
    PrAppFileDialogKind_Open,
    PrAppFileDialogKind_Save,
};
typedef uint8_t PrAppFileDialogKind;

// Options for an Open dialog.
enum {
    PrAppFileDialogFlag_Multiple = 1 << 0,      // the user may choose more than one
    PrAppFileDialogFlag_Directories = 1 << 1,   // the user chooses directories instead of files
};
typedef uint8_t PrAppFileDialogFlags;

// A type of file that a dialog offers. `name` labels the type, such as "Images". `extensions` lists its extensions
// without dots, separated by commas, such as "png,jpg".
typedef struct PrAppFileType {
    PrStringView name;
    PrStringView extensions;
} PrAppFileType;

// The description of a file dialog. `parent` is the window that owns the dialog, or null. On macOS, the dialog shows as
// a sheet on the parent. `directory` is the directory that the dialog shows first, or empty to let the system choose.
// `name` is the file name that a Save dialog suggests. `types` lists the file types that the user may choose, or none
// to allow any type. A Save dialog adds the first extension of the first type to a name without an extension.
typedef struct PrAppFileDialogDesc {
    PrAppFileDialogKind kind;
    PrAppFileDialogFlags flags;
    PrAppWindow* PR_NULLABLE parent;
    PrStringView title;
    PrStringView directory;
    PrStringView name;
    const PrAppFileType* PR_NULLABLE types;
    uint32_t typeCount;
} PrAppFileDialogDesc;

// Receives the answer of a file dialog: a source that holds the files that the user chose, or None if the user
// canceled. For a Save dialog, the source holds the file to write (prAppDataWriteFile).
typedef void (*PrAppFileDialogFn)(void* PR_NULLABLE context, PrStatus status, PrAppDataSource source);

// Shows a file dialog and returns immediately. The dialog copies `desc`. `done` receives NotSupported if the backend
// has no file dialogs, as for a headless application. Destroying the parent closes the dialog, which then answers as if
// the user canceled.
void prAppFileDialog(PrApp* app, const PrAppFileDialogDesc* desc, PrAppFileDialogFn done, void* PR_NULLABLE context);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
