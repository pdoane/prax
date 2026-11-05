// The index of a store's records, by key and by their last use, which the backends share.

#pragma once

#include "pr/atomic.h"
#include "pr/byte_view.h"
#include "pr/hash_index.h"
#include "pr/raw_array.h"
#include "pr/raw_string.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// A record's bytes in memory, counted by the store that holds them and by each record read from them.
struct StorageBuffer {
    Atomic<uint32_t> references;
    size_t size;

    uint8_t* bytes() { return reinterpret_cast<uint8_t*>(this + 1); }
};

// Returns a buffer that holds a copy of `bytes`, with one reference.
StorageBuffer* storageBufferCreate(PrByteView bytes);

// Adds or drops a reference. The last drop frees the buffer. A null buffer is ignored.
void storageBufferRetain(StorageBuffer* buffer);
void storageBufferRelease(StorageBuffer* PR_NULLABLE buffer);

// One record that a store holds: its key, the size of its bytes, the use that last touched it, and where its bytes are,
// which is the backend's: a buffer, or the number of a file.
struct StorageEntry {
    PrRawString key;
    uint64_t size;
    uint64_t used;
    uint64_t file;
    StorageBuffer* PR_NULLABLE buffer;
};

// A store's records, found by key. `total` is the sum of their sizes and of their keys' sizes, and `uses` counts the
// reads and writes, which order the records by their last use.
struct StorageIndex {
    RawArray<StorageEntry> entries;
    HashIndex hashes;
    uint64_t total;
    uint64_t uses;
};

// Returns the position of the record at `key`, or kHashMissing.
uint32_t storageIndexFind(const StorageIndex* index, PrStringView key);

// Adds a record at `key`, which the index must not hold, and returns its position. Its key is copied.
uint32_t storageIndexInsert(StorageIndex* index, PrStringView key, uint64_t size, uint64_t used);

// Sets the size of the bytes of the record at `position`.
void storageIndexResize(StorageIndex* index, uint32_t position, uint64_t size);

// Removes the record at `position`. The last record takes its position. The caller frees what the entry held first.
void storageIndexRemove(StorageIndex* index, uint32_t position);

// Returns the position of the record that was used least recently, or kHashMissing if the index is empty.
uint32_t storageIndexOldest(const StorageIndex* index);

// Frees the index's keys and storage. The caller frees what the entries held first.
void storageIndexReset(StorageIndex* index);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
