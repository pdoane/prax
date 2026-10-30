// A store that keeps its records in memory (storage_memory.h).

#include "pr/storage_memory.h"

#include "pr/alloc.h"
#include "pr/thread.h"

#include "storage/src/storage_index.h"

namespace pr_impl {

namespace {

// `mutex` guards the index.
struct MemoryStorage : PrStorage {
    uint64_t limit;
    PrStorageEviction eviction;
    PrMutex mutex;
    StorageIndex index;
};

void removeAt(MemoryStorage* storage, uint32_t position) {
    storageBufferRelease(storage->index.entries.data[position].buffer);
    storageIndexRemove(&storage->index, position);
}

void destroyStorage(PrStorage* base) {
    MemoryStorage* storage = static_cast<MemoryStorage*>(base);
    for (uint32_t i = 0; i < storage->index.entries.count; ++i)
        storageBufferRelease(storage->index.entries.data[i].buffer);
    storageIndexReset(&storage->index);
    prMutexReset(&storage->mutex);
    prMemoryFree(storage);
}

bool readStorage(PrStorage* base, PrStringView key, PrStorageRecord* out) {
    MemoryStorage* storage = static_cast<MemoryStorage*>(base);
    prMutexLock(&storage->mutex);
    uint32_t position = storageIndexFind(&storage->index, key);
    if (position != kHashMissing) {
        StorageEntry& entry = storage->index.entries.data[position];
        if (storage->eviction == PrStorageEviction_LeastRecentlyUsed)
            entry.used = ++storage->index.uses;
        storageBufferRetain(entry.buffer);
        *out = PrStorageRecord{PrByteView{entry.buffer->bytes(), entry.buffer->size}, entry.buffer};
    }
    prMutexUnlock(&storage->mutex);
    return position != kHashMissing;
}

void releaseRecord(PrStorage*, PrStorageRecord* record) {
    storageBufferRelease(static_cast<StorageBuffer*>(record->handle));
}

bool writeStorage(PrStorage* base, PrStringView key, PrByteView bytes) {
    MemoryStorage* storage = static_cast<MemoryStorage*>(base);
    StorageBuffer* buffer = key.size + bytes.size <= storage->limit ? storageBufferCreate(bytes) : nullptr;
    prMutexLock(&storage->mutex);
    uint32_t position = storageIndexFind(&storage->index, key);
    uint64_t replaced = position != kHashMissing ? key.size + storage->index.entries.data[position].size : 0;
    bool fits = buffer != nullptr && (storage->eviction == PrStorageEviction_LeastRecentlyUsed ||
        storage->index.total - replaced + key.size + bytes.size <= storage->limit);
    if (!fits) {
        if (storage->eviction == PrStorageEviction_LeastRecentlyUsed && position != kHashMissing)
            removeAt(storage, position);
        prMutexUnlock(&storage->mutex);
        storageBufferRelease(buffer);
        return false;
    }
    if (position == kHashMissing) {
        position = storageIndexInsert(&storage->index, key, bytes.size, 0);
    } else {
        storageBufferRelease(storage->index.entries.data[position].buffer);
        storageIndexResize(&storage->index, position, bytes.size);
    }
    StorageEntry& entry = storage->index.entries.data[position];
    entry.used = ++storage->index.uses;
    entry.buffer = buffer;
    // The record just written is the most recently used, and fits alone, so it is never the one evicted.
    while (storage->index.total > storage->limit)
        removeAt(storage, storageIndexOldest(&storage->index));
    prMutexUnlock(&storage->mutex);
    return true;
}

void removeStorage(PrStorage* base, PrStringView key) {
    MemoryStorage* storage = static_cast<MemoryStorage*>(base);
    prMutexLock(&storage->mutex);
    uint32_t position = storageIndexFind(&storage->index, key);
    if (position != kHashMissing)
        removeAt(storage, position);
    prMutexUnlock(&storage->mutex);
}

void clearStorage(PrStorage* base) {
    MemoryStorage* storage = static_cast<MemoryStorage*>(base);
    prMutexLock(&storage->mutex);
    while (storage->index.entries.count != 0)
        removeAt(storage, storage->index.entries.count - 1);
    prMutexUnlock(&storage->mutex);
}

uint64_t sizeStorage(const PrStorage* base) {
    MemoryStorage* storage = const_cast<MemoryStorage*>(static_cast<const MemoryStorage*>(base));
    prMutexLock(&storage->mutex);
    uint64_t total = storage->index.total;
    prMutexUnlock(&storage->mutex);
    return total;
}

uint32_t countStorage(const PrStorage* base) {
    MemoryStorage* storage = const_cast<MemoryStorage*>(static_cast<const MemoryStorage*>(base));
    prMutexLock(&storage->mutex);
    uint32_t count = storage->index.entries.count;
    prMutexUnlock(&storage->mutex);
    return count;
}

PrString keyStorage(const PrStorage* base, uint32_t position) {
    MemoryStorage* storage = const_cast<MemoryStorage*>(static_cast<const MemoryStorage*>(base));
    prMutexLock(&storage->mutex);
    PrString key;
    if (position < storage->index.entries.count)
        key = prStringCopy(prStringView(&storage->index.entries.data[position].key));
    else
        key = prStringNull();
    prMutexUnlock(&storage->mutex);
    return key;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrStorage* prStorageMemoryCreate(uint64_t limitBytes, PrStorageEviction eviction) {
    MemoryStorage* storage = allocZeroed<MemoryStorage>();
    storage->destroy = destroyStorage;
    storage->read = readStorage;
    storage->release = releaseRecord;
    storage->write = writeStorage;
    storage->remove = removeStorage;
    storage->clear = clearStorage;
    storage->size = sizeStorage;
    storage->count = countStorage;
    storage->key = keyStorage;
    storage->limit = limitBytes;
    storage->eviction = eviction;
    prMutexInit(&storage->mutex);
    return storage;
}
