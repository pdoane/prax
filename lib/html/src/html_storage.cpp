// Web Storage (html_storage.h).

#include "html/src/html_storage.h"

#include "pr/alloc.h"
#include "pr/array.h"
#include "pr/assert.h"
#include "pr/mutex.h"
#include "pr/storage_disk.h"
#include "pr/storage_memory.h"
#include "pr/text_buffer.h"

#include "html/src/html_window.h"

namespace pr_impl {

// An origin's area, made the first time the origin asked for one.
struct ProviderArea {
    PrString origin;
    PrStorage* store;
};

// A provider. `mutex` guards `areas`.
struct HtmlStorageProvider {
    PrString directory;     // the null string for areas in memory
    uint64_t quota;
    PrTaskPool* PR_NULLABLE pool;
    PrMutex mutex;
    Array<ProviderArea> areas;
};

namespace {

// Appends to `out` the name of the directory of `origin`'s area: the origin, with each byte that a file name may not
// hold written as '_' and two hex digits.
void appendDirectoryName(PrStringView origin, TextBuffer* out) {
    constexpr char kHex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < origin.size; ++i) {
        unsigned char c = static_cast<unsigned char>(origin.data[i]);
        bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-';
        if (plain) {
            textAppendChar(out, char(c));
        } else {
            textAppendChar(out, '_');
            textAppendChar(out, kHex[c >> 4]);
            textAppendChar(out, kHex[c & 15]);
        }
    }
}

// Returns a new area for `origin`, or null if its directory cannot be made.
PrStorage* PR_NULLABLE createArea(const HtmlStorageProvider* provider, PrStringView origin) {
    if (prStringIsNull(&provider->directory))
        return prStorageMemoryCreate(provider->quota, PrStorageEviction_None);
    TextBuffer path{};
    textAppend(&path, prStringView(&provider->directory));
    textAppendChar(&path, '/');
    appendDirectoryName(origin, &path);
    PrStorageDiskDesc desc{
        .directory = textView(&path),
        .limitBytes = provider->quota,
        .eviction = PrStorageEviction_None,
        .pool = provider->pool,
    };
    PrStorage* store = nullptr;
    prStorageDiskCreate(&desc, &store);
    textReset(&path);
    return store;
}

// Returns `origin`'s area in the provider, made if it is the origin's first, or null if it cannot be made.
PrStorage* PR_NULLABLE providerArea(HtmlStorageProvider* provider, PrStringView origin) {
    prMutexLock(&provider->mutex);
    PrStorage* store = nullptr;
    for (uint32_t i = 0; i < provider->areas.count && store == nullptr; ++i) {
        if (prStringView(&provider->areas.data[i].origin) == origin)
            store = provider->areas.data[i].store;
    }
    if (store == nullptr) {
        store = createArea(provider, origin);
        if (store != nullptr)
            provider->areas.push(ProviderArea{prStringCopy(origin), store});
    }
    prMutexUnlock(&provider->mutex);
    return store;
}

} // namespace

void htmlStorageReset(HtmlStorage* storage) {
    prStringReset(&storage->origin);
    storage->area = nullptr;
}

} // namespace pr_impl

using namespace pr_impl;

struct PrHtmlStorageProvider : HtmlStorageProvider {};

PrHtmlStorageProvider* prHtmlStorageProviderCreate(const PrHtmlStorageProviderDesc* desc) {
    PrHtmlStorageProvider* provider = allocZeroed<PrHtmlStorageProvider>();
    provider->directory = desc->directory.data != nullptr ? prStringCopy(desc->directory) : prStringNull();
    provider->quota = desc->quotaBytes;
    provider->pool = desc->pool;
    PR_ASSERT(desc->directory.data == nullptr || desc->pool != nullptr);
    prMutexInit(&provider->mutex);
    return provider;
}

void prHtmlStorageProviderDestroy(PrHtmlStorageProvider* PR_NULLABLE provider) {
    if (provider == nullptr)
        return;
    for (uint32_t i = 0; i < provider->areas.count; ++i) {
        prStorageDestroy(provider->areas.data[i].store);
        prStringReset(&provider->areas.data[i].origin);
    }
    provider->areas.reset();
    prStringReset(&provider->directory);
    prMutexReset(&provider->mutex);
    prMemoryFree(provider);
}

PrHtmlStorage* PR_NULLABLE prHtmlWindowStorage(PrHtmlWindow* window, PrHtmlStorageKind kind) {
    PrHtmlStorage* storage = &window->storage[kind];
    if (storage->provider == nullptr)
        return nullptr;
    PrString origin = htmlWindowOrigin(window);
    // A file: URL's origin is the implementation's to define (url/#concept-url-origin). Its document's origin is
    // opaque, and its storage is the one area that every file: document shares, so that a page opened from its files
    // keeps what it stores.
    if (prStringView(&origin) == "null" && prUrlScheme(window->document->url) == "file")
        prStringReplace(&origin, "file://");
    if (storage->area == nullptr || prStringView(&storage->origin) != prStringView(&origin)) {
        htmlStorageReset(storage);
        // html/#dom-localstorage: an opaque origin has no storage.
        if (prStringView(&origin) != "null")
            storage->area = providerArea(storage->provider, prStringView(&origin));
        storage->origin = origin;
    } else {
        prStringReset(&origin);
    }
    return storage->area != nullptr ? storage : nullptr;
}

uint32_t prHtmlStorageLength(const PrHtmlStorage* self) {
    return prStorageCount(self->area);
}

PrString prHtmlStorageKey(const PrHtmlStorage* self, uint32_t index) {
    return prStorageKey(self->area, index);
}

PrString prHtmlStorageGetItem(const PrHtmlStorage* self, PrStringView key) {
    PrStorageRecord record{};
    if (!prStorageRead(self->area, key, &record))
        return prStringNull();
    PrString value = prStringCopy(PrStringView{reinterpret_cast<const char*>(record.bytes.data), record.bytes.size});
    prStorageRecordRelease(self->area, &record);
    return value;
}

PrStatus prHtmlStorageSetItem(PrHtmlStorage* self, PrStringView key, PrStringView value) {
    PrStorageRecord record{};
    if (prStorageRead(self->area, key, &record)) {
        bool same = PrStringView{reinterpret_cast<const char*>(record.bytes.data), record.bytes.size} == value;
        prStorageRecordRelease(self->area, &record);
        if (same)
            return PrStatus_Ok;
    }
    PrByteView bytes{reinterpret_cast<const uint8_t*>(value.data), value.size};
    return prStorageWrite(self->area, key, bytes) ? PrStatus_Ok : PrStatus_Limit;
}

void prHtmlStorageRemoveItem(PrHtmlStorage* self, PrStringView key) {
    prStorageRemove(self->area, key);
}

void prHtmlStorageClear(PrHtmlStorage* self) {
    prStorageClear(self->area);
}
