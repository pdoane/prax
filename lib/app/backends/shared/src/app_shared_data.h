// What the backends share of data transfer: owned copies of content, the sources that the system hands the program, and
// the requests that wait for their answers.

#pragma once

#include "pr/app_backend.h"
#include "pr/app_data.h"
#include "pr/base.h"
#include "pr/mutex.h"
#include "pr/raw_array.h"
#include "pr/raw_string.h"
#include "pr/thread.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// A list of strings, stored end to end in one buffer.
struct AppStrings {
    RawArray<char> bytes;
    RawArray<uint32_t> ends;        // the end offset of each string in `bytes`
    RawArray<PrStringView> views;   // a view of each string, made by appStringsViews
};

// Appends a string, empties the list and keeps its storage, or empties it and frees its storage.
void appStringsAdd(AppStrings& strings, PrStringView text);
void appStringsClear(AppStrings& strings);
void appStringsReset(AppStrings& strings);

// Returns the strings' views, valid until the strings change.
const PrStringView* PR_NULLABLE appStringsViews(AppStrings& strings);

// ---------------------------------------------------------------------------------------------------------------------
// Stores

// An owned copy of content in every form, as a backend reads it from the system or copies it from the program. The
// files hold their entries, and the paths of the content are the files' paths.
struct AppDataStore {
    PrAppDataFormFlags forms;
    PrAppDataPayload payload;
    RawArray<char> text;
    RawArray<char> html;
    AppStrings urls;
    AppStrings fileNames;
    AppStrings filePaths;
    RawArray<uint64_t> fileSizes;
    RawArray<bool> fileDirectories;
    RawArray<PrAppDataFile> files;      // by file, as appDataStoreFiles makes them
    RawArray<uint8_t> image;            // the image's rows, packed
    PrUVec2 imageExtent;
    AppStrings customTypes;
    RawArray<uint8_t> customBytes;
    RawArray<uint32_t> customEnds;      // by custom content: its end in `customBytes`
    RawArray<PrAppDataCustom> custom;   // by custom content, as appDataStoreContent makes them
};

// Replaces the store's content with a copy of `content`. A path is a file whose name is the path's last component.
void appDataStoreSet(AppDataStore& store, const PrAppDataContent& content);

// Adds a form of content to the store, and its flag.
void appDataStoreSetText(AppDataStore& store, PrStringView text);
void appDataStoreSetHtml(AppDataStore& store, PrStringView html);
void appDataStoreAddUrl(AppDataStore& store, PrStringView url);
void appDataStoreAddFile(AppDataStore& store, const PrAppDataFile& file);
void appDataStoreAddPath(AppDataStore& store, PrStringView path);
void appDataStoreSetImage(AppDataStore& store, PrPixelView image);
void appDataStoreAddCustom(AppDataStore& store, PrStringView type, PrByteView bytes);
void appDataStoreSetPayload(AppDataStore& store, PrAppDataPayload payload);

// Empties the store, keeping its storage, or frees it.
void appDataStoreClear(AppDataStore& store);
void appDataStoreReset(AppDataStore& store);

// Returns the store's content, or its files, valid until the store changes.
PrAppDataContent appDataStoreContent(AppDataStore& store);
const PrAppDataFile* PR_NULLABLE appDataStoreFiles(AppDataStore& store, uint32_t* count);

// Returns the bytes of the store's custom content of `type`, or none.
PrByteView appDataStoreCustom(AppDataStore& store, PrStringView type);

// Returns the name that a path's last component gives a file.
PrStringView appPathName(PrStringView path);

// ---------------------------------------------------------------------------------------------------------------------
// Sources

// A reference-counted source that the system handed the program, such as a drop, the answer of a file dialog, a request
// to open files, or the destination of a promise. It holds its content and an optional handle of the backend, such as
// the browser's files.
struct AppDataSourceEntry {
    PrAppDataSource id;
    uint32_t references;
    AppDataStore* store;
    void* PR_NULLABLE native;
};

// The table of an application's sources. When the last reference to a source goes, the table frees its store and passes
// its native handle to `releaseNative`. Any thread may retain and release a source, so `lock` guards the table.
struct AppDataSources {
    PrMutex lock;
    RawArray<AppDataSourceEntry> entries;
    PrAppDataSource last;               // the id of the most recently created source
    void (*PR_NULLABLE releaseNative)(void* native);
};

// Initializes the table, or frees every source in it regardless of its references.
void appDataSourcesInit(AppDataSources& sources);
void appDataSourcesReset(AppDataSources& sources);

// Makes a source of `store`, which it takes, and of the backend's `native`, with one reference, and returns its id,
// which is never None, the clipboard or the selection.
PrAppDataSource appDataSourceCreate(AppDataSources& sources, AppDataStore* store, void* PR_NULLABLE native);

// Adds a reference to a source that the sources made, or drops one, freeing the source with its last. A call on any
// other source does nothing.
void appDataSourceRetain(AppDataSources& sources, PrAppDataSource source);
void appDataSourceRelease(AppDataSources& sources, PrAppDataSource source);

// Returns the store and the backend's handle of a source, which a reference keeps, or null for a source that the
// sources do not hold.
AppDataStore* PR_NULLABLE appDataSourceStore(AppDataSources& sources, PrAppDataSource source);
void* PR_NULLABLE appDataSourceNative(AppDataSources& sources, PrAppDataSource source);

// ---------------------------------------------------------------------------------------------------------------------
// Requests

// The kind of answer that a request waits for. A read waits for the kind of data that it reads, a PrAppDataReadKind.
// The other requests wait for one of the values below, which follow the read kinds.
enum {
    AppDataAnswer_Status = PrAppDataReadKind_File + 1,  // the status of a write or a share
    AppDataAnswer_Source,                               // the source that a file dialog chose
};
typedef uint8_t AppDataAnswer;

// A pending request: its id, its source, which it holds a reference to, what it answers with, its callback, and the
// custom type that it reads.
struct AppDataRequest {
    uint32_t id;
    PrAppDataSource source;
    AppDataAnswer answer;
    void (*callback)(void);
    void* PR_NULLABLE context;
    PrRawString type;
};

// The pending requests of an application. Any thread may add and cancel them, so `lock` guards the list and the answer
// that runs.
struct AppDataRequests {
    PrMutex lock;
    RawArray<AppDataRequest> requests;
    uint32_t lastId;
    AppDataSources* sources;

    // The context of the callback that runs, if `answering`. A cancel of that context on another thread waits on
    // `answered` until the callback returns.
    bool answering;
    void* PR_NULLABLE answeringContext;
    PrCondition answered;
};

// Initializes the requests, whose sources `sources` holds, or frees them.
void appDataRequestsInit(AppDataRequests& requests, AppDataSources& sources);
void appDataRequestsReset(AppDataRequests& requests);

// Adds a pending request of `source`, retaining it, and returns its id, which is never zero. `callback` is the callback
// of `answer`'s type. A request of custom content copies its `type`.
uint32_t appDataRequestsAdd(AppDataRequests& requests, PrAppDataSource source, AppDataAnswer answer,
    void (*callback)(void), void* PR_NULLABLE context, PrStringView type = {});

// Writes the source and the answer of the pending request `id`, and returns true, or false if it is gone, as after a
// cancel.
bool appDataRequestsFind(AppDataRequests& requests, uint32_t id, PrAppDataSource* source, AppDataAnswer* answer);

// Cancels the pending requests of `context`. If a callback of `context` runs on another thread, this waits for it to
// return.
void appDataRequestsCancel(AppDataRequests& requests, void* PR_NULLABLE context);

// Completes the request `id` by calling its callback, and releases its source. A request that is gone does nothing. A
// backend calls these on the application's thread, never during another callback.
//
// `FromStore` answers a request of content in the form that it asks for, from `store`, which may be null for none.
void appDataRequestsAnswerFromStore(AppDataRequests& requests, uint32_t id, PrStatus status, AppDataStore* PR_NULLABLE store);
void appDataRequestsAnswerBytes(AppDataRequests& requests, uint32_t id, PrStatus status, PrByteView bytes);
void appDataRequestsAnswerStatus(AppDataRequests& requests, uint32_t id, PrStatus status);
void appDataRequestsAnswerSource(AppDataRequests& requests, uint32_t id, PrStatus status, PrAppDataSource source);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
