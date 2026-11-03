// Blobs, and the store of the URLs that name them.

#include "fetch/src/fetch_blob.h"

#include "pr/alloc.h"
#include "pr/ascii.h"
#include "pr/memory.h"
#include "pr/random.h"
#include "pr/text_buffer.h"

namespace pr_impl {

namespace {

PrStringView withoutFragment(PrStringView url) {
    for (size_t i = 0; i < url.size; ++i) {
        if (url.data[i] == '#')
            return PrStringView{url.data, i};
    }
    return url;
}

// Appends a version 4 UUID (rfc 9562, section 5.4).
void appendUuid(TextBuffer* out) {
    uint8_t bytes[16];
    prRandomBytes(bytes, sizeof(bytes));
    bytes[6] = uint8_t((bytes[6] & 0x0f) | 0x40);
    bytes[8] = uint8_t((bytes[8] & 0x3f) | 0x80);
    constexpr char kDigits[] = "0123456789abcdef";
    for (uint32_t i = 0; i < sizeof(bytes); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10)
            textAppendChar(out, '-');
        textAppendChar(out, kDigits[bytes[i] >> 4]);
        textAppendChar(out, kDigits[bytes[i] & 15]);
    }
}

PrFetchBlob* newBlob(PrByteView bytes, PrStringView type) {
    PrFetchBlob* blob = allocZeroed<PrFetchBlob>();
    blob->references.storeRelaxed(1);
    if (bytes.size != 0)
        blob->bytes.append(bytes.data, uint32_t(bytes.size));
    bool valid = true;
    for (size_t i = 0; i < type.size; ++i)
        valid = valid && uint8_t(type.data[i]) >= 0x20 && uint8_t(type.data[i]) <= 0x7e;
    TextBuffer lowered{};
    if (valid)
        textAppend(&lowered, type);
    textAsciiLower(&lowered);
    blob->type = prStringCopy(textView(&lowered));
    textReset(&lowered);
    return blob;
}

int64_t clampPosition(int64_t position, int64_t size) {
    if (position < 0)
        return position + size > 0 ? position + size : 0;
    return position < size ? position : size;
}

void resetEntry(FetchBlobUrl* entry) {
    prStringReset(&entry->url);
    prStringReset(&entry->origin);
    prFetchBlobRelease(entry->blob);
}

// A revocation on its way to the network's loop.
struct BlobRevocation {
    PrFetchBlobStore* store;
    PrString url;
    PrString origin;
};

void revokeOnLoop(void* PR_NULLABLE data) {
    BlobRevocation* revocation = static_cast<BlobRevocation*>(data);
    PrFetchBlobStore* store = revocation->store;
    prMutexLock(&store->lock);
    for (uint32_t i = 0; i < store->entries.count; ++i) {
        FetchBlobUrl& entry = store->entries.data[i];
        if (prStringView(&entry.url) == prStringView(&revocation->url) &&
                prStringView(&entry.origin) == prStringView(&revocation->origin)) {
            resetEntry(&entry);
            store->entries.erase(i);
            break;
        }
    }
    prMutexUnlock(&store->lock);
    prStringReset(&revocation->url);
    prStringReset(&revocation->origin);
    prMemoryFree(revocation);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

const PrFetchBlob* prFetchBlobCreate(PrByteView bytes, PrStringView type) {
    return newBlob(bytes, type);
}

const PrFetchBlob* prFetchBlobCreateFile(PrByteView bytes, PrStringView type, PrStringView name, double lastModified) {
    PrFetchBlob* blob = newBlob(bytes, type);
    blob->isFile = true;
    blob->name = prStringCopy(name);
    blob->lastModified = lastModified;
    return blob;
}

void prFetchBlobRetain(const PrFetchBlob* blob) {
    blob->references.fetchAdd(1);
}

void prFetchBlobRelease(const PrFetchBlob* PR_NULLABLE blob) {
    if (blob == nullptr || blob->references.fetchSub(1) != 1)
        return;
    PrFetchBlob* owned = const_cast<PrFetchBlob*>(blob);
    owned->bytes.reset();
    prStringReset(&owned->type);
    prStringReset(&owned->name);
    prMemoryFree(owned);
}

PrByteView prFetchBlobBytes(const PrFetchBlob* blob) {
    static const uint8_t kEmpty = 0;
    return PrByteView{blob->bytes.data != nullptr ? blob->bytes.data : &kEmpty, blob->bytes.count};
}

PrStringView prFetchBlobType(const PrFetchBlob* blob) {
    return prStringView(&blob->type);
}

bool prFetchBlobIsFile(const PrFetchBlob* blob) {
    return blob->isFile;
}

PrStringView prFetchBlobFileName(const PrFetchBlob* blob) {
    return prStringView(&blob->name);
}

double prFetchBlobFileLastModified(const PrFetchBlob* blob) {
    return blob->lastModified;
}

const PrFetchBlob* prFetchBlobSlice(const PrFetchBlob* blob, int64_t start, int64_t end, PrStringView type) {
    PrByteView bytes = prFetchBlobBytes(blob);
    int64_t size = int64_t(bytes.size);
    int64_t from = clampPosition(start, size);
    int64_t to = clampPosition(end, size);
    int64_t span = to > from ? to - from : 0;
    return newBlob(PrByteView{bytes.data + from, size_t(span)}, type);
}

PrString prFetchBlobConvertLineEndings(PrStringView text) {
    TextBuffer out{};
#if defined(_WIN32)
    constexpr PrStringView kNative = "\r\n";
#else
    constexpr PrStringView kNative = "\n";
#endif
    for (size_t i = 0; i < text.size; ++i) {
        if (text.data[i] == '\r') {
            textAppend(&out, kNative);
            if (i + 1 < text.size && text.data[i + 1] == '\n')
                ++i;
        } else if (text.data[i] == '\n') {
            textAppend(&out, kNative);
        } else {
            textAppendChar(&out, text.data[i]);
        }
    }
    PrString result = prStringCopy(textView(&out));
    textReset(&out);
    return result;
}

PrFetchBlobStore* prFetchBlobStoreCreate(void) {
    PrFetchBlobStore* store = allocZeroed<PrFetchBlobStore>();
    prMutexInit(&store->lock);
    return store;
}

void prFetchBlobStoreDestroy(PrFetchBlobStore* PR_NULLABLE store) {
    if (store == nullptr)
        return;
    for (uint32_t i = 0; i < store->entries.count; ++i)
        resetEntry(&store->entries.data[i]);
    store->entries.reset();
    prMutexReset(&store->lock);
    prMemoryFree(store);
}

const PrFetchBlob* PR_NULLABLE prFetchBlobStoreResolve(PrFetchBlobStore* store, PrStringView url) {
    PrStringView wanted = withoutFragment(url);
    const PrFetchBlob* blob = nullptr;
    prMutexLock(&store->lock);
    for (uint32_t i = 0; i < store->entries.count && blob == nullptr; ++i) {
        if (prStringView(&store->entries.data[i].url) == wanted) {
            blob = store->entries.data[i].blob;
            prFetchBlobRetain(blob);
        }
    }
    prMutexUnlock(&store->lock);
    return blob;
}

bool prFetchBlobIsUrl(PrStringView url) {
    return asciiStartsWithFold(url, "blob:");
}

PrString prFetchBlobUrlGenerate(PrStringView origin) {
    TextBuffer url{};
    textAppend(&url, "blob:");
    textAppend(&url, origin);
    textAppendChar(&url, '/');
    appendUuid(&url);
    PrString result = prStringCopy(textView(&url));
    textReset(&url);
    return result;
}

PrString prFetchBlobUrlCreate(PrFetchNetwork* fetcher, const PrFetchBlob* blob, PrStringView origin) {
    PrString result = prFetchBlobUrlGenerate(origin);
    PrFetchBlobStore* store = fetcher->blobStore;
    if (store == nullptr)
        return result;
    prFetchBlobRetain(blob);
    prMutexLock(&store->lock);
    store->entries.push(FetchBlobUrl{prStringClone(&result), prStringCopy(origin), blob});
    prMutexUnlock(&store->lock);
    return result;
}

void prFetchBlobUrlRevoke(PrFetchNetwork* fetcher, PrStringView url, PrStringView origin) {
    PrFetchBlobStore* store = fetcher->blobStore;
    if (store == nullptr)
        return;
    BlobRevocation* revocation = allocZeroed<BlobRevocation>();
    revocation->store = store;
    revocation->url = prStringCopy(withoutFragment(url));
    revocation->origin = prStringCopy(origin);
    prNetPost(fetcher->net, revokeOnLoop, revocation);
}
