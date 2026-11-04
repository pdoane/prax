// Web Storage (html_storage.h).

#include "html/src/html_storage.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/file.h"
#include "pr/mutex.h"
#include "pr/raw_array.h"
#include "pr/storage_disk.h"
#include "pr/storage_memory.h"
#include "pr/text_buffer.h"

#include "html/src/html_window.h"

namespace pr_impl {

// An origin's area, made the first time the origin asked for one.
struct ProviderArea {
    PrRawString origin;
    PrStorage* store;
};

// A provider. `mutex` guards `areas`.
struct HtmlStorageProvider {
    PrRawString directory;     // the null string for areas in memory
    uint64_t quota;
    PrTaskPool* PR_NULLABLE pool;
    PrMutex mutex;
    RawArray<ProviderArea> areas;
};

namespace {

// Appends to `out` the name of the directory of `origin`'s area: the origin, with each byte that a file name may not
// hold written as '_' and two hex digits.
void appendDirectoryName(PrStringView origin, PrTextBuffer* out) {
    constexpr char kHex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < origin.size; ++i) {
        unsigned char c = static_cast<unsigned char>(origin.data[i]);
        bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-';
        if (plain) {
            prTextBufferAppendChar(out, char(c));
        } else {
            prTextBufferAppendChar(out, '_');
            prTextBufferAppendChar(out, kHex[c >> 4]);
            prTextBufferAppendChar(out, kHex[c & 15]);
        }
    }
}

// Opens the area on disk whose directory in the provider's directory is `name`, or returns null if it cannot be made.
PrStorage* PR_NULLABLE openDiskArea(const HtmlStorageProvider* provider, PrStringView name) {
    PrTextBuffer path{};
    prTextBufferAppend(&path, prRawStringView(&provider->directory));
    prTextBufferAppendChar(&path, '/');
    prTextBufferAppend(&path, name);
    PrStorageDiskDesc desc{
        .directory = prTextBufferView(&path),
        .limitBytes = provider->quota,
        .eviction = PrStorageEviction_None,
        .pool = provider->pool,
    };
    PrStorage* store = nullptr;
    prStorageDiskCreate(&desc, &store);
    prTextBufferReset(&path);
    return store;
}

// Returns a new area for `origin`, or null if its directory cannot be made.
PrStorage* PR_NULLABLE createArea(const HtmlStorageProvider* provider, PrStringView origin) {
    if (prRawStringIsEmpty(&provider->directory))
        return prStorageMemoryCreate(provider->quota, PrStorageEviction_None);
    PrTextBuffer name{};
    appendDirectoryName(origin, &name);
    PrStorage* store = openDiskArea(provider, prTextBufferView(&name));
    prTextBufferReset(&name);
    return store;
}

// Returns `origin`'s area in the provider, made if it is the origin's first, or null if it cannot be made.
PrStorage* PR_NULLABLE providerArea(HtmlStorageProvider* provider, PrStringView origin) {
    prMutexLock(&provider->mutex);
    PrStorage* store = nullptr;
    for (uint32_t i = 0; i < provider->areas.count && store == nullptr; ++i) {
        if (prRawStringView(&provider->areas.data[i].origin) == origin)
            store = provider->areas.data[i].store;
    }
    if (store == nullptr) {
        store = createArea(provider, origin);
        if (store != nullptr)
            provider->areas.push(ProviderArea{prRawStringCopy(origin), store});
    }
    prMutexUnlock(&provider->mutex);
    return store;
}

} // namespace

void htmlStorageReset(HtmlStorage* storage) {
    prRawStringReset(&storage->origin);
    storage->area = nullptr;
}

} // namespace pr_impl

using namespace pr_impl;

struct PrHtmlStorageProvider : HtmlStorageProvider {};

PrHtmlStorageProvider* prHtmlStorageProviderCreate(const PrHtmlStorageProviderDesc* desc) {
    PrHtmlStorageProvider* provider = allocZeroed<PrHtmlStorageProvider>();
    provider->directory = desc->directory.isEmpty() ? prRawStringNull() : prRawStringCopy(desc->directory);
    provider->quota = desc->quotaBytes;
    provider->pool = desc->pool;
    PR_ASSERT(desc->directory.isEmpty() || desc->pool != nullptr);
    prMutexInit(&provider->mutex);
    return provider;
}

void prHtmlStorageProviderDestroy(PrHtmlStorageProvider* PR_NULLABLE provider) {
    if (provider == nullptr)
        return;
    for (uint32_t i = 0; i < provider->areas.count; ++i) {
        prStorageDestroy(provider->areas.data[i].store);
        prRawStringReset(&provider->areas.data[i].origin);
    }
    provider->areas.reset();
    prRawStringReset(&provider->directory);
    prMutexReset(&provider->mutex);
    prMemoryFree(provider);
}

void prHtmlStorageProviderClear(PrHtmlStorageProvider* provider) {
    prMutexLock(&provider->mutex);
    for (uint32_t i = 0; i < provider->areas.count; ++i)
        prStorageClear(provider->areas.data[i].store);
    // The areas of origins that have not asked for theirs in this run are opened only to be cleared. The names of their
    // directories are gathered first, since the callback must not change the provider's directory.
    struct Gather {
        const HtmlStorageProvider* provider;
        RawArray<PrRawString> closed;
    } gather{provider, {}};
    if (!prRawStringIsEmpty(&provider->directory)) {
        prFileEachEntry(prRawStringView(&provider->directory), [](void* PR_NULLABLE context, const PrFileEntry* entry) {
            Gather& g = *static_cast<Gather*>(context);
            if (entry->info.kind != PrFileKind_Directory)
                return;
            PrTextBuffer name{};
            for (uint32_t i = 0; i < g.provider->areas.count; ++i) {
                prTextBufferClear(&name);
                appendDirectoryName(prRawStringView(&g.provider->areas.data[i].origin), &name);
                if (prTextBufferView(&name) == entry->name) {
                    prTextBufferReset(&name);
                    return;
                }
            }
            prTextBufferReset(&name);
            g.closed.push(prRawStringCopy(entry->name));
        }, &gather);
    }
    for (uint32_t i = 0; i < gather.closed.count; ++i) {
        if (PrStorage* store = openDiskArea(provider, prRawStringView(&gather.closed.data[i]))) {
            prStorageClear(store);
            prStorageDestroy(store);
        }
        prRawStringReset(&gather.closed.data[i]);
    }
    gather.closed.reset();
    prMutexUnlock(&provider->mutex);
}

PrHtmlStorage* PR_NULLABLE prHtmlWindowStorage(PrHtmlWindow* window, PrHtmlStorageKind kind) {
    PrHtmlStorage* storage = &window->storage[kind];
    if (storage->provider == nullptr)
        return nullptr;
    PrRawString origin = htmlWindowOrigin(window);
    // A file: URL's origin is the implementation's to define (url/#concept-url-origin). Its document's origin is
    // opaque, and its storage is the one area that every file: document shares, so that a page opened from its files
    // keeps what it stores.
    if (prRawStringView(&origin) == "null" && prUrlScheme(window->document->url) == "file")
        prRawStringReplace(&origin, "file://");
    if (storage->area == nullptr || prRawStringView(&storage->origin) != prRawStringView(&origin)) {
        htmlStorageReset(storage);
        // html/#dom-localstorage: an opaque origin has no storage.
        if (prRawStringView(&origin) != "null")
            storage->area = providerArea(storage->provider, prRawStringView(&origin));
        storage->origin = origin;
    } else {
        prRawStringReset(&origin);
    }
    return storage->area != nullptr ? storage : nullptr;
}

uint32_t prHtmlStorageLength(const PrHtmlStorage* storage) {
    return prStorageCount(storage->area);
}

PrRawString prHtmlStorageKey(const PrHtmlStorage* storage, uint32_t index) {
    return prStorageKey(storage->area, index);
}

PrRawString prHtmlStorageGetItem(const PrHtmlStorage* storage, PrStringView key) {
    PrStorageRecord record{};
    if (!prStorageRead(storage->area, key, &record))
        return prRawStringNull();
    PrStringView text = {reinterpret_cast<const char*>(record.bytes.data), record.bytes.size};
    PrRawString value = prRawStringCopy(text);
    prStorageRecordRelease(storage->area, &record);
    return value;
}

PrStatus prHtmlStorageSetItem(PrHtmlStorage* storage, PrStringView key, PrStringView value) {
    PrStorageRecord record{};
    if (prStorageRead(storage->area, key, &record)) {
        bool same = PrStringView{reinterpret_cast<const char*>(record.bytes.data), record.bytes.size} == value;
        prStorageRecordRelease(storage->area, &record);
        if (same)
            return PrStatus_Ok;
    }
    PrByteView bytes{reinterpret_cast<const uint8_t*>(value.data), value.size};
    return prStorageWrite(storage->area, key, bytes) ? PrStatus_Ok : PrStatus_Limit;
}

void prHtmlStorageRemoveItem(PrHtmlStorage* storage, PrStringView key) {
    prStorageRemove(storage->area, key);
}

void prHtmlStorageClear(PrHtmlStorage* storage) {
    prStorageClear(storage->area);
}
