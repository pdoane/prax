// A store that keeps its records on disk (storage_disk.h).

#include "pr/storage_disk.h"

#include "pr/alloc.h"
#include "pr/file.h"
#include "pr/mem.h"
#include "pr/string_ops.h"
#include "pr/task.h"
#include "pr/text_buffer.h"
#include "pr/thread.h"

#include "storage/src/storage_index.h"

namespace pr_impl {

namespace {

// The start of the index file, and the version of its layout. The layout is in the machine's byte order: each record is
// the size of its key, its key, the size of its bytes, its last use and the number of its file.
constexpr uint8_t kIndexMagic[4] = {'P', 'R', 'S', 'I'};
constexpr uint32_t kIndexVersion = 1;

enum {
    DiskJobKind_Write,          // writes a record's buffer to its file
    DiskJobKind_Remove,         // removes a record's file
};
typedef uint8_t DiskJobKind;

struct DiskJob {
    DiskJobKind kind;
    uint64_t file;
    StorageBuffer* PR_NULLABLE buffer;
};

// A store. A record's entry holds its buffer from its write until its file is written. The jobs run in the order they
// were queued, in a drain that the pool runs (drain): one at a time, posted when a job is queued or the index changes
// while none is posted. `mutex` guards the fields after it.
struct DiskStorage : PrStorage {
    PrRawString directory;
    uint64_t limit;
    PrStorageEviction eviction;
    PrTaskPool* pool;
    PrMutex mutex;
    PrCondition drained;        // signaled when a drain ends
    StorageIndex index;
    uint64_t nextFile;
    RawArray<DiskJob> jobs;
    bool dirty;                 // true if the index changed since a drain last wrote it
    bool draining;              // true from when a drain is posted until it ends
};

// A record read from a store: the buffer of a record that is not yet written, or the mapping of its file.
struct DiskRecord {
    StorageBuffer* PR_NULLABLE buffer;
    PrByteView mapping;
};

void appendPath(const DiskStorage* storage, uint64_t file, PrTextBuffer* out) {
    prTextBufferAppend(out, prRawStringView(&storage->directory));
    prTextBufferAppendChar(out, '/');
    if (file == 0)
        prTextBufferAppend(out, "index");
    else
        prTextBufferAppendU64(out, file, 16);
}

// Queues the removal of the file of the entry at `position`, and removes the entry. Called with the mutex held.
void removeAt(DiskStorage* storage, uint32_t position) {
    StorageEntry& entry = storage->index.entries.data[position];
    storage->jobs.push(DiskJob{DiskJobKind_Remove, entry.file, nullptr});
    storageBufferRelease(entry.buffer);
    storageIndexRemove(&storage->index, position);
    storage->dirty = true;
}

// Writes the index's records, as the store holds them now, to the index file. Called without the mutex held.
void writeIndex(DiskStorage* storage) {
    PrTextBuffer bytes{};
    prMutexLock(&storage->mutex);
    storage->dirty = false;
    prTextBufferAppend(&bytes, PrStringView{reinterpret_cast<const char*>(kIndexMagic), 4});
    prTextBufferAppend(&bytes, PrStringView{reinterpret_cast<const char*>(&kIndexVersion), 4});
    uint32_t count = storage->index.entries.count;
    prTextBufferAppend(&bytes, PrStringView{reinterpret_cast<const char*>(&count), 4});
    for (uint32_t i = 0; i < count; ++i) {
        const StorageEntry& entry = storage->index.entries.data[i];
        PrStringView key = prRawStringView(&entry.key);
        uint32_t keySize = uint32_t(key.size);
        prTextBufferAppend(&bytes, PrStringView{reinterpret_cast<const char*>(&keySize), 4});
        prTextBufferAppend(&bytes, key);
        prTextBufferAppend(&bytes, PrStringView{reinterpret_cast<const char*>(&entry.size), 8});
        prTextBufferAppend(&bytes, PrStringView{reinterpret_cast<const char*>(&entry.used), 8});
        prTextBufferAppend(&bytes, PrStringView{reinterpret_cast<const char*>(&entry.file), 8});
    }
    prMutexUnlock(&storage->mutex);
    PrTextBuffer path{};
    appendPath(storage, 0, &path);
    PrStringView written = prTextBufferView(&bytes);
    prFileWrite(prTextBufferView(&path), prStringBytes(written));
    prTextBufferReset(&path);
    prTextBufferReset(&bytes);
}

// Runs one job, without the mutex held.
void runJob(DiskStorage* storage, const DiskJob& job) {
    PrTextBuffer path{};
    appendPath(storage, job.file, &path);
    if (job.kind == DiskJobKind_Remove) {
        prFileRemove(prTextBufferView(&path));
        prTextBufferReset(&path);
        return;
    }
    bool written = prFileWrite(prTextBufferView(&path), PrByteView{job.buffer->bytes(), job.buffer->size});
    prTextBufferReset(&path);
    // The entry reads its file from now on, unless a later write replaced it or the file was not written.
    prMutexLock(&storage->mutex);
    for (uint32_t i = 0; i < storage->index.entries.count; ++i) {
        StorageEntry& entry = storage->index.entries.data[i];
        if (entry.file != job.file)
            continue;
        if (written) {
            storageBufferRelease(entry.buffer);
            entry.buffer = nullptr;
        } else {
            storageBufferRelease(entry.buffer);
            storageIndexRemove(&storage->index, i);
        }
        storage->dirty = true;
        break;
    }
    prMutexUnlock(&storage->mutex);
    storageBufferRelease(job.buffer);
}

// Runs the jobs in the order they were queued, and writes the index once none waits, on a thread of the store's pool.
void drain(void* PR_NULLABLE context) {
    DiskStorage* storage = static_cast<DiskStorage*>(context);
    prMutexLock(&storage->mutex);
    for (;;) {
        if (storage->jobs.count != 0) {
            DiskJob job = storage->jobs.data[0];
            storage->jobs.erase(0);
            prMutexUnlock(&storage->mutex);
            runJob(storage, job);
            prMutexLock(&storage->mutex);
        } else if (storage->dirty) {
            prMutexUnlock(&storage->mutex);
            writeIndex(storage);
            prMutexLock(&storage->mutex);
        } else {
            break;
        }
    }
    storage->draining = false;
    prConditionBroadcast(&storage->drained);
    prMutexUnlock(&storage->mutex);
}

// Unlocks the store's mutex, and posts a drain if the store has work for one and none is posted.
void unlockAndDrain(DiskStorage* storage) {
    bool post = !storage->draining && (storage->jobs.count != 0 || storage->dirty);
    storage->draining = storage->draining || post;
    prMutexUnlock(&storage->mutex);
    if (post)
        prTaskPoolPost(storage->pool, drain, storage);
}

// Reads the index file into the store's index. A file that is missing or does not parse leaves the index empty.
void readIndex(DiskStorage* storage) {
    PrTextBuffer path{};
    appendPath(storage, 0, &path);
    PrByteView mapping{};
    bool mapped = prFileMap(prTextBufferView(&path), &mapping);
    prTextBufferReset(&path);
    if (!mapped)
        return;
    const uint8_t* at = mapping.data;
    const uint8_t* end = mapping.data + mapping.size;
    auto take = [&](void* out, size_t size) {
        if (size_t(end - at) < size)
            return false;
        prMemCopy(out, at, size);
        at += size;
        return true;
    };
    uint8_t magic[4];
    uint32_t version = 0;
    uint32_t count = 0;
    if (take(magic, 4) && prMemEquals(magic, kIndexMagic, 4) && take(&version, 4) && version == kIndexVersion &&
            take(&count, 4)) {
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t keySize = 0;
            if (!take(&keySize, 4) || size_t(end - at) < keySize)
                break;
            PrStringView key{reinterpret_cast<const char*>(at), keySize};
            at += keySize;
            uint64_t size = 0;
            uint64_t used = 0;
            uint64_t file = 0;
            if (!take(&size, 8) || !take(&used, 8) || !take(&file, 8) || file == 0)
                break;
            if (storageIndexFind(&storage->index, key) != kHashMissing)
                continue;
            uint32_t position = storageIndexInsert(&storage->index, key, size, used);
            storage->index.entries.data[position].file = file;
            if (used > storage->index.uses)
                storage->index.uses = used;
            if (file >= storage->nextFile)
                storage->nextFile = file + 1;
        }
    }
    prFileUnmap(&mapping);
}

// Store functions

void destroyStorage(PrStorage* base) {
    DiskStorage* storage = static_cast<DiskStorage*>(base);
    prMutexLock(&storage->mutex);
    storage->dirty = true;
    unlockAndDrain(storage);
    prMutexLock(&storage->mutex);
    while (storage->draining)
        prConditionWait(&storage->drained, &storage->mutex);
    prMutexUnlock(&storage->mutex);
    for (uint32_t i = 0; i < storage->index.entries.count; ++i)
        storageBufferRelease(storage->index.entries.data[i].buffer);
    storageIndexReset(&storage->index);
    storage->jobs.reset();
    prRawStringReset(&storage->directory);
    prConditionReset(&storage->drained);
    prMutexReset(&storage->mutex);
    prMemoryFree(storage);
}

bool readStorage(PrStorage* base, PrStringView key, PrStorageRecord* out) {
    DiskStorage* storage = static_cast<DiskStorage*>(base);
    prMutexLock(&storage->mutex);
    uint32_t position = storageIndexFind(&storage->index, key);
    if (position == kHashMissing) {
        prMutexUnlock(&storage->mutex);
        return false;
    }
    StorageEntry& entry = storage->index.entries.data[position];
    if (storage->eviction == PrStorageEviction_LeastRecentlyUsed) {
        entry.used = ++storage->index.uses;
        storage->dirty = true;
    }
    DiskRecord* record = allocZeroed<DiskRecord>();
    if (entry.buffer != nullptr) {
        storageBufferRetain(entry.buffer);
        record->buffer = entry.buffer;
        unlockAndDrain(storage);
        *out = PrStorageRecord{PrByteView{record->buffer->bytes(), record->buffer->size}, record};
        return true;
    }
    uint64_t file = entry.file;
    uint64_t size = entry.size;
    unlockAndDrain(storage);
    PrTextBuffer path{};
    appendPath(storage, file, &path);
    bool mapped = size == 0 || prFileMap(prTextBufferView(&path), &record->mapping);
    prTextBufferReset(&path);
    if (mapped && record->mapping.size == size) {
        *out = PrStorageRecord{record->mapping, record};
        return true;
    }
    // A file that is missing or of another size than the index says is not the record. The entry goes, unless another
    // write replaced it meanwhile.
    prFileUnmap(&record->mapping);
    prMemoryFree(record);
    prMutexLock(&storage->mutex);
    position = storageIndexFind(&storage->index, key);
    if (position != kHashMissing && storage->index.entries.data[position].file == file)
        removeAt(storage, position);
    unlockAndDrain(storage);
    return false;
}

void releaseRecord(PrStorage*, PrStorageRecord* record) {
    DiskRecord* disk = static_cast<DiskRecord*>(record->handle);
    storageBufferRelease(disk->buffer);
    prFileUnmap(&disk->mapping);
    prMemoryFree(disk);
}

bool writeStorage(PrStorage* base, PrStringView key, PrByteView bytes) {
    DiskStorage* storage = static_cast<DiskStorage*>(base);
    StorageBuffer* buffer = key.size + bytes.size <= storage->limit ? storageBufferCreate(bytes) : nullptr;
    prMutexLock(&storage->mutex);
    uint32_t position = storageIndexFind(&storage->index, key);
    uint64_t replaced = position != kHashMissing ? key.size + storage->index.entries.data[position].size : 0;
    bool fits = buffer != nullptr && (storage->eviction == PrStorageEviction_LeastRecentlyUsed ||
        storage->index.total - replaced + key.size + bytes.size <= storage->limit);
    if (!fits) {
        if (storage->eviction == PrStorageEviction_LeastRecentlyUsed && position != kHashMissing)
            removeAt(storage, position);
        unlockAndDrain(storage);
        storageBufferRelease(buffer);
        return false;
    }
    if (position == kHashMissing) {
        position = storageIndexInsert(&storage->index, key, bytes.size, 0);
    } else {
        StorageEntry& previous = storage->index.entries.data[position];
        storage->jobs.push(DiskJob{DiskJobKind_Remove, previous.file, nullptr});
        storageBufferRelease(previous.buffer);
        storageIndexResize(&storage->index, position, bytes.size);
    }
    StorageEntry& entry = storage->index.entries.data[position];
    entry.used = ++storage->index.uses;
    entry.file = storage->nextFile++;
    entry.buffer = buffer;
    storageBufferRetain(buffer);
    storage->jobs.push(DiskJob{DiskJobKind_Write, entry.file, buffer});
    // The record just written is the most recently used, and fits alone, so it is never the one evicted.
    while (storage->index.total > storage->limit)
        removeAt(storage, storageIndexOldest(&storage->index));
    storage->dirty = true;
    unlockAndDrain(storage);
    return true;
}

void removeStorage(PrStorage* base, PrStringView key) {
    DiskStorage* storage = static_cast<DiskStorage*>(base);
    prMutexLock(&storage->mutex);
    uint32_t position = storageIndexFind(&storage->index, key);
    if (position != kHashMissing)
        removeAt(storage, position);
    unlockAndDrain(storage);
}

void clearStorage(PrStorage* base) {
    DiskStorage* storage = static_cast<DiskStorage*>(base);
    prMutexLock(&storage->mutex);
    while (storage->index.entries.count != 0)
        removeAt(storage, storage->index.entries.count - 1);
    unlockAndDrain(storage);
}

uint64_t sizeStorage(const PrStorage* base) {
    DiskStorage* storage = const_cast<DiskStorage*>(static_cast<const DiskStorage*>(base));
    prMutexLock(&storage->mutex);
    uint64_t total = storage->index.total;
    prMutexUnlock(&storage->mutex);
    return total;
}

uint32_t countStorage(const PrStorage* base) {
    DiskStorage* storage = const_cast<DiskStorage*>(static_cast<const DiskStorage*>(base));
    prMutexLock(&storage->mutex);
    uint32_t count = storage->index.entries.count;
    prMutexUnlock(&storage->mutex);
    return count;
}

PrRawString keyStorage(const PrStorage* base, uint32_t position) {
    DiskStorage* storage = const_cast<DiskStorage*>(static_cast<const DiskStorage*>(base));
    prMutexLock(&storage->mutex);
    PrRawString key;
    if (position < storage->index.entries.count)
        key = prRawStringCopy(prRawStringView(&storage->index.entries.data[position].key));
    else
        key = prRawStringNull();
    prMutexUnlock(&storage->mutex);
    return key;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrStatus prStorageDiskCreate(const PrStorageDiskDesc* desc, PrStorage* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    if (!prFileCreateDirectory(desc->directory))
        return PrStatus_Io;
    DiskStorage* storage = allocZeroed<DiskStorage>();
    storage->destroy = destroyStorage;
    storage->read = readStorage;
    storage->release = releaseRecord;
    storage->write = writeStorage;
    storage->remove = removeStorage;
    storage->clear = clearStorage;
    storage->size = sizeStorage;
    storage->count = countStorage;
    storage->key = keyStorage;
    storage->directory = prRawStringCopy(desc->directory);
    storage->limit = desc->limitBytes;
    storage->eviction = desc->eviction;
    storage->pool = desc->pool;
    storage->nextFile = 1;
    prMutexInit(&storage->mutex);
    prConditionInit(&storage->drained);
    readIndex(storage);
    // A limit lower than the last run's evicts what no longer fits, in a store that evicts. One that does not keeps its
    // records, and refuses writes until enough are removed.
    prMutexLock(&storage->mutex);
    while (storage->eviction == PrStorageEviction_LeastRecentlyUsed && storage->index.total > storage->limit)
        removeAt(storage, storageIndexOldest(&storage->index));
    unlockAndDrain(storage);
    *out = storage;
    return PrStatus_Ok;
}
