// What the backends share of data transfer.

#include "app/backends/shared/src/app_shared_data.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/memory.h"

namespace pr_impl {

namespace {

// The view of `count` bytes at `data`, or the empty view.
PrStringView viewOf(const char* PR_NULLABLE data, uint32_t count) {
    return PrStringView{data != nullptr ? data : "", count};
}

// The first id of a source that the sources make, past the clipboard and the selection.
constexpr PrAppDataSource kFirstSource = kPrAppDataSourceSelection + 1;

// Returns the entry of `source`, or null. The lock is held.
AppDataSourceEntry* PR_NULLABLE entryOf(AppDataSources& sources, PrAppDataSource source) {
    for (uint32_t i = 0; i < sources.entries.count; ++i) {
        if (sources.entries.data[i].id == source)
            return &sources.entries.data[i];
    }
    return nullptr;
}

void freeEntry(AppDataSources& sources, AppDataSourceEntry& entry) {
    appDataStoreReset(*entry.store);
    prMemoryFree(entry.store);
    if (entry.native != nullptr && sources.releaseNative != nullptr)
        sources.releaseNative(entry.native);
}

// The requests whose callback runs on this thread, or null. A cancel from within the callback does not wait for it.
thread_local const AppDataRequests* PR_NULLABLE answeringOn = nullptr;

// Removes the pending request `id`, writes it to `out`, and marks its context as answering. Returns false if it is
// gone.
bool take(AppDataRequests& requests, uint32_t id, AppDataRequest* out) {
    prMutexLock(&requests.lock);
    bool found = false;
    for (uint32_t i = 0; i < requests.requests.count; ++i) {
        if (requests.requests.data[i].id == id) {
            *out = requests.requests.data[i];
            requests.requests.erase(i);
            found = true;
            break;
        }
    }
    if (found) {
        requests.answering = true;
        requests.answeringContext = out->context;
        answeringOn = &requests;
    }
    prMutexUnlock(&requests.lock);
    return found;
}

// Ends the answer that take began, releases the request's source and type, and releases the cancels that wait for it.
void endAnswer(AppDataRequests& requests, AppDataRequest& request) {
    prMutexLock(&requests.lock);
    requests.answering = false;
    answeringOn = nullptr;
    prConditionBroadcast(&requests.answered);
    prMutexUnlock(&requests.lock);
    appDataSourceRelease(*requests.sources, request.source);
    prRawStringReset(&request.type);
}

template <typename Fn>
Fn callbackOf(const AppDataRequest& request) {
    return reinterpret_cast<Fn>(request.callback);
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------------
// Strings

void appStringsAdd(AppStrings& strings, PrStringView text) {
    if (text.size != 0)
        strings.bytes.append(text.data, uint32_t(text.size));
    strings.ends.push(strings.bytes.count);
}

void appStringsClear(AppStrings& strings) {
    strings.bytes.clear();
    strings.ends.clear();
    strings.views.clear();
}

void appStringsReset(AppStrings& strings) {
    strings.bytes.reset();
    strings.ends.reset();
    strings.views.reset();
}

const PrStringView* PR_NULLABLE appStringsViews(AppStrings& strings) {
    strings.views.clear();
    uint32_t start = 0;
    for (uint32_t i = 0; i < strings.ends.count; ++i) {
        uint32_t end = strings.ends.data[i];
        strings.views.push(viewOf(strings.bytes.data != nullptr ? strings.bytes.data + start : nullptr, end - start));
        start = end;
    }
    return strings.views.data;
}

// ---------------------------------------------------------------------------------------------------------------------
// Stores

void appDataStoreSet(AppDataStore& store, const PrAppDataContent& content) {
    appDataStoreClear(store);
    if ((content.forms & PrAppDataFormFlag_Text) != 0)
        appDataStoreSetText(store, content.text);
    if ((content.forms & PrAppDataFormFlag_Html) != 0)
        appDataStoreSetHtml(store, content.html);
    if ((content.forms & PrAppDataFormFlag_Urls) != 0) {
        store.forms |= PrAppDataFormFlag_Urls;
        for (uint32_t i = 0; i < content.urlCount; ++i)
            appDataStoreAddUrl(store, content.urls[i]);
    }
    if ((content.forms & PrAppDataFormFlag_Files) != 0) {
        store.forms |= PrAppDataFormFlag_Files;
        for (uint32_t i = 0; i < content.pathCount; ++i)
            appDataStoreAddPath(store, content.paths[i]);
    }
    if ((content.forms & PrAppDataFormFlag_Image) != 0)
        appDataStoreSetImage(store, content.image);
    if ((content.forms & PrAppDataFormFlag_Custom) != 0) {
        store.forms |= PrAppDataFormFlag_Custom;
        for (uint32_t i = 0; i < content.customCount; ++i)
            appDataStoreAddCustom(store, content.custom[i].type, content.custom[i].bytes);
    }
    if ((content.forms & PrAppDataFormFlag_Payload) != 0)
        appDataStoreSetPayload(store, content.payload);
}

void appDataStoreSetText(AppDataStore& store, PrStringView text) {
    store.forms |= PrAppDataFormFlag_Text;
    store.text.clear();
    if (text.size != 0)
        store.text.append(text.data, uint32_t(text.size));
}

void appDataStoreSetHtml(AppDataStore& store, PrStringView html) {
    store.forms |= PrAppDataFormFlag_Html;
    store.html.clear();
    if (html.size != 0)
        store.html.append(html.data, uint32_t(html.size));
}

void appDataStoreAddUrl(AppDataStore& store, PrStringView url) {
    store.forms |= PrAppDataFormFlag_Urls;
    appStringsAdd(store.urls, url);
}

void appDataStoreAddFile(AppDataStore& store, const PrAppDataFile& file) {
    store.forms |= PrAppDataFormFlag_Files;
    appStringsAdd(store.fileNames, file.name);
    appStringsAdd(store.filePaths, file.path);
    store.fileSizes.push(file.size);
    store.fileDirectories.push(file.directory);
}

void appDataStoreAddPath(AppDataStore& store, PrStringView path) {
    appDataStoreAddFile(store, PrAppDataFile{.name = appPathName(path), .path = path});
}

void appDataStoreSetImage(AppDataStore& store, PrPixelView image) {
    store.forms |= PrAppDataFormFlag_Image;
    store.image.clear();
    store.imageExtent = PrUVec2{};
    if (image.data == nullptr)
        return;
    for (uint32_t y = 0; y < image.extent.y; ++y)
        store.image.append(image.data + size_t(y) * image.stride, image.extent.x * 4);
    store.imageExtent = image.extent;
}

void appDataStoreAddCustom(AppDataStore& store, PrStringView type, PrByteView bytes) {
    store.forms |= PrAppDataFormFlag_Custom;
    appStringsAdd(store.customTypes, type);
    if (bytes.size != 0)
        store.customBytes.append(bytes.data, uint32_t(bytes.size));
    store.customEnds.push(store.customBytes.count);
}

void appDataStoreSetPayload(AppDataStore& store, PrAppDataPayload payload) {
    store.forms |= PrAppDataFormFlag_Payload;
    store.payload = payload;
}

void appDataStoreClear(AppDataStore& store) {
    store.forms = 0;
    store.payload = PrAppDataPayload{};
    store.text.clear();
    store.html.clear();
    appStringsClear(store.urls);
    appStringsClear(store.fileNames);
    appStringsClear(store.filePaths);
    store.fileSizes.clear();
    store.fileDirectories.clear();
    store.files.clear();
    store.image.clear();
    store.imageExtent = PrUVec2{};
    appStringsClear(store.customTypes);
    store.customBytes.clear();
    store.customEnds.clear();
    store.custom.clear();
}

void appDataStoreReset(AppDataStore& store) {
    store.text.reset();
    store.html.reset();
    appStringsReset(store.urls);
    appStringsReset(store.fileNames);
    appStringsReset(store.filePaths);
    store.fileSizes.reset();
    store.fileDirectories.reset();
    store.files.reset();
    store.image.reset();
    appStringsReset(store.customTypes);
    store.customBytes.reset();
    store.customEnds.reset();
    store.custom.reset();
    store = AppDataStore{};
}

PrAppDataContent appDataStoreContent(AppDataStore& store) {
    const PrStringView* types = appStringsViews(store.customTypes);
    store.custom.clear();
    uint32_t start = 0;
    for (uint32_t i = 0; i < store.customEnds.count; ++i) {
        uint32_t end = store.customEnds.data[i];
        store.custom.push(PrAppDataCustom{
            .type = types[i],
            .bytes = PrByteView{store.customBytes.data != nullptr ? store.customBytes.data + start : nullptr, end - start},
        });
        start = end;
    }
    PrAppDataContent content{
        .forms = store.forms,
        .text = viewOf(store.text.data, store.text.count),
        .html = viewOf(store.html.data, store.html.count),
        .urls = appStringsViews(store.urls),
        .urlCount = store.urls.ends.count,
        .paths = appStringsViews(store.filePaths),
        .pathCount = store.filePaths.ends.count,
        .custom = store.custom.data,
        .customCount = store.custom.count,
        .payload = store.payload,
    };
    if (store.image.count != 0)
        content.image = PrPixelView{store.image.data, size_t(store.imageExtent.x) * 4, store.imageExtent};
    return content;
}

const PrAppDataFile* PR_NULLABLE appDataStoreFiles(AppDataStore& store, uint32_t* count) {
    const PrStringView* names = appStringsViews(store.fileNames);
    const PrStringView* paths = appStringsViews(store.filePaths);
    store.files.clear();
    for (uint32_t i = 0; i < store.fileSizes.count; ++i) {
        store.files.push(PrAppDataFile{
            .name = names[i],
            .path = paths[i],
            .size = store.fileSizes.data[i],
            .directory = store.fileDirectories.data[i],
        });
    }
    *count = store.files.count;
    return store.files.data;
}

PrByteView appDataStoreCustom(AppDataStore& store, PrStringView type) {
    PrAppDataContent content = appDataStoreContent(store);
    for (uint32_t i = 0; i < content.customCount; ++i) {
        if (content.custom[i].type == type)
            return content.custom[i].bytes;
    }
    return PrByteView{};
}

PrStringView appPathName(PrStringView path) {
    size_t end = path.size;
    while (end > 1 && (path.data[end - 1] == '/' || path.data[end - 1] == '\\'))
        --end;
    size_t start = end;
    while (start > 0 && path.data[start - 1] != '/' && path.data[start - 1] != '\\')
        --start;
    return PrStringView{path.data + start, end - start};
}

// ---------------------------------------------------------------------------------------------------------------------
// Sources

void appDataSourcesInit(AppDataSources& sources) {
    prMutexInit(&sources.lock);
}

void appDataSourcesReset(AppDataSources& sources) {
    for (uint32_t i = 0; i < sources.entries.count; ++i)
        freeEntry(sources, sources.entries.data[i]);
    sources.entries.reset();
    prMutexReset(&sources.lock);
}

PrAppDataSource appDataSourceCreate(AppDataSources& sources, AppDataStore* store, void* PR_NULLABLE native) {
    prMutexLock(&sources.lock);
    if (sources.last < kFirstSource - 1)
        sources.last = kFirstSource - 1;
    PrAppDataSource id = ++sources.last;
    sources.entries.push(AppDataSourceEntry{.id = id, .references = 1, .store = store, .native = native});
    prMutexUnlock(&sources.lock);
    return id;
}

void appDataSourceRetain(AppDataSources& sources, PrAppDataSource source) {
    prMutexLock(&sources.lock);
    if (AppDataSourceEntry* entry = entryOf(sources, source))
        ++entry->references;
    prMutexUnlock(&sources.lock);
}

void appDataSourceRelease(AppDataSources& sources, PrAppDataSource source) {
    prMutexLock(&sources.lock);
    AppDataSourceEntry freed{};
    bool last = false;
    for (uint32_t i = 0; i < sources.entries.count; ++i) {
        AppDataSourceEntry& entry = sources.entries.data[i];
        if (entry.id != source)
            continue;
        PR_ASSERT(entry.references != 0);
        if (--entry.references == 0) {
            freed = entry;
            last = true;
            sources.entries.erase(i);
        }
        break;
    }
    prMutexUnlock(&sources.lock);
    // The backend's handle is freed outside the lock, as freeing it may call back into the sources.
    if (last)
        freeEntry(sources, freed);
}

AppDataStore* PR_NULLABLE appDataSourceStore(AppDataSources& sources, PrAppDataSource source) {
    prMutexLock(&sources.lock);
    AppDataSourceEntry* entry = entryOf(sources, source);
    AppDataStore* store = entry != nullptr ? entry->store : nullptr;
    prMutexUnlock(&sources.lock);
    return store;
}

void* PR_NULLABLE appDataSourceNative(AppDataSources& sources, PrAppDataSource source) {
    prMutexLock(&sources.lock);
    AppDataSourceEntry* entry = entryOf(sources, source);
    void* native = entry != nullptr ? entry->native : nullptr;
    prMutexUnlock(&sources.lock);
    return native;
}

// ---------------------------------------------------------------------------------------------------------------------
// Requests

void appDataRequestsInit(AppDataRequests& requests, AppDataSources& sources) {
    prMutexInit(&requests.lock);
    prConditionInit(&requests.answered);
    requests.sources = &sources;
}

void appDataRequestsReset(AppDataRequests& requests) {
    for (uint32_t i = 0; i < requests.requests.count; ++i)
        prRawStringReset(&requests.requests.data[i].type);
    requests.requests.reset();
    prConditionReset(&requests.answered);
    prMutexReset(&requests.lock);
}

uint32_t appDataRequestsAdd(AppDataRequests& requests, PrAppDataSource source, AppDataAnswer answer,
        void (*callback)(void), void* PR_NULLABLE context, PrStringView type) {
    appDataSourceRetain(*requests.sources, source);
    prMutexLock(&requests.lock);
    if (++requests.lastId == 0)
        requests.lastId = 1;
    uint32_t id = requests.lastId;
    requests.requests.push(AppDataRequest{
        .id = id,
        .source = source,
        .answer = answer,
        .callback = callback,
        .context = context,
        .type = prRawStringCopy(type),
    });
    prMutexUnlock(&requests.lock);
    return id;
}

bool appDataRequestsFind(AppDataRequests& requests, uint32_t id, PrAppDataSource* source, AppDataAnswer* answer) {
    prMutexLock(&requests.lock);
    bool found = false;
    for (uint32_t i = 0; i < requests.requests.count && !found; ++i) {
        if (requests.requests.data[i].id == id) {
            *source = requests.requests.data[i].source;
            *answer = requests.requests.data[i].answer;
            found = true;
        }
    }
    prMutexUnlock(&requests.lock);
    return found;
}

void appDataRequestsCancel(AppDataRequests& requests, void* PR_NULLABLE context) {
    RawArray<AppDataRequest> cancelled{};
    prMutexLock(&requests.lock);
    uint32_t kept = 0;
    for (uint32_t i = 0; i < requests.requests.count; ++i) {
        if (requests.requests.data[i].context != context)
            requests.requests.data[kept++] = requests.requests.data[i];
        else
            cancelled.push(requests.requests.data[i]);
    }
    requests.requests.resize(kept);
    while (requests.answering && requests.answeringContext == context && answeringOn != &requests)
        prConditionWait(&requests.answered, &requests.lock);
    prMutexUnlock(&requests.lock);
    for (uint32_t i = 0; i < cancelled.count; ++i) {
        appDataSourceRelease(*requests.sources, cancelled.data[i].source);
        prRawStringReset(&cancelled.data[i].type);
    }
    cancelled.reset();
}

void appDataRequestsAnswerFromStore(AppDataRequests& requests, uint32_t id, PrStatus status, AppDataStore* PR_NULLABLE store) {
    AppDataRequest request{};
    if (!take(requests, id, &request))
        return;
    AppDataStore empty{};
    AppDataStore& from = store != nullptr ? *store : empty;
    PrAppDataContent content = appDataStoreContent(from);
    switch (request.answer) {
        case PrAppDataReadKind_Forms:
            callbackOf<PrAppDataFormsFn>(request)(request.context, status, content.forms);
            break;
        case PrAppDataReadKind_Text:
            callbackOf<PrAppDataTextFn>(request)(request.context, status, content.text);
            break;
        case PrAppDataReadKind_Html:
            callbackOf<PrAppDataTextFn>(request)(request.context, status, content.html);
            break;
        case PrAppDataReadKind_Urls:
            callbackOf<PrAppDataUrlsFn>(request)(request.context, status, content.urls, content.urlCount);
            break;
        case PrAppDataReadKind_Files: {
            uint32_t count = 0;
            const PrAppDataFile* files = appDataStoreFiles(from, &count);
            callbackOf<PrAppDataFilesFn>(request)(request.context, status, files, count);
            break;
        }
        case PrAppDataReadKind_Image:
            callbackOf<PrAppDataImageFn>(request)(request.context, status, content.image);
            break;
        case PrAppDataReadKind_Custom: {
            PrByteView bytes = appDataStoreCustom(from, prRawStringView(&request.type));
            callbackOf<PrAppDataBytesFn>(request)(request.context, status, bytes);
            break;
        }
        default:
            PR_ASSERT(false);
            break;
    }
    appDataStoreReset(empty);
    endAnswer(requests, request);
}

void appDataRequestsAnswerBytes(AppDataRequests& requests, uint32_t id, PrStatus status, PrByteView bytes) {
    AppDataRequest request{};
    if (!take(requests, id, &request))
        return;
    PR_ASSERT(request.answer == PrAppDataReadKind_File);
    callbackOf<PrAppDataBytesFn>(request)(request.context, status, bytes);
    endAnswer(requests, request);
}

void appDataRequestsAnswerStatus(AppDataRequests& requests, uint32_t id, PrStatus status) {
    AppDataRequest request{};
    if (!take(requests, id, &request))
        return;
    PR_ASSERT(request.answer == AppDataAnswer_Status);
    callbackOf<PrAppDataStatusFn>(request)(request.context, status);
    endAnswer(requests, request);
}

void appDataRequestsAnswerSource(AppDataRequests& requests, uint32_t id, PrStatus status, PrAppDataSource source) {
    AppDataRequest request{};
    if (!take(requests, id, &request))
        return;
    PR_ASSERT(request.answer == AppDataAnswer_Source);
    callbackOf<PrAppFileDialogFn>(request)(request.context, status, source);
    endAnswer(requests, request);
}

} // namespace pr_impl
