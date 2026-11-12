// Storage: records kept by key, held in memory or on disk.

#include "pr/storage.h"

#include "pr/assert.h"
#include "pr/storage_backend.h"

void prStorageDestroy(PrStorage* PR_NULLABLE storage) {
    if (storage != nullptr)
        storage->destroyFn(storage);
}

bool prStorageRead(PrStorage* storage, PrStringView key, PrStorageRecord* out) {
    *out = PrStorageRecord{PrByteView{nullptr, 0}, nullptr};
    return storage->readFn(storage, key, out);
}

void prStorageRecordRelease(PrStorage* storage, PrStorageRecord* record) {
    if (record->handle != nullptr)
        storage->releaseFn(storage, record);
    *record = PrStorageRecord{PrByteView{nullptr, 0}, nullptr};
}

bool prStorageWrite(PrStorage* storage, PrStringView key, PrByteView bytes) {
    PR_ASSERT(bytes.data != nullptr || bytes.size == 0);
    return storage->writeFn(storage, key, bytes);
}

void prStorageRemove(PrStorage* storage, PrStringView key) {
    storage->removeFn(storage, key);
}

void prStorageClear(PrStorage* storage) {
    storage->clearFn(storage);
}

uint64_t prStorageSize(const PrStorage* storage) {
    return storage->sizeFn(storage);
}

uint32_t prStorageCount(const PrStorage* storage) {
    return storage->countFn(storage);
}

PrRawString prStorageKey(const PrStorage* storage, uint32_t position) {
    return storage->keyFn(storage, position);
}
