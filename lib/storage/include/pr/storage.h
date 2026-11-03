// Storage: records kept by key, such as the entries of an HTTP cache or the items of a page's local storage, held in
// memory or on disk.
//
// A store keeps the bytes of its records, keys included, under a limit. A store that evicts makes room for a write by
// removing the records that were read or written least recently, and one that does not evict refuses a write that would
// not fit. A store may be used from any number of threads at once.

#pragma once

#include "pr/base.h"
#include "pr/byte_view.h"
#include "pr/raw_string.h"
#include "pr/status.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// A store of records. Each backend provides its own constructor.
typedef struct PrStorage PrStorage;

// How a store stays under its limit.
enum {
    PrStorageEviction_LeastRecentlyUsed,    // a write removes the records used least recently until it fits
    PrStorageEviction_None,                 // a write that does not fit is refused, and the store is unchanged
};
typedef uint8_t PrStorageEviction;

// A record read from a store. Its bytes stay valid until it is released, even if the store replaces or removes the
// record meanwhile. `handle` is the backend's.
typedef struct PrStorageRecord {
    PrByteView bytes;
    void* PR_NULLABLE handle;
} PrStorageRecord;

// The function table of a store, placed at the start of the backend's state. Each member implements the public function
// of the same name. Callers use the functions below, never these members.
struct PrStorage {
    void (*destroy)(PrStorage* storage);
    bool (*read)(PrStorage* storage, PrStringView key, PrStorageRecord* out);
    void (*release)(PrStorage* storage, PrStorageRecord* record);
    bool (*write)(PrStorage* storage, PrStringView key, PrByteView bytes);
    void (*remove)(PrStorage* storage, PrStringView key);
    void (*clear)(PrStorage* storage);
    uint64_t (*size)(const PrStorage* storage);
    uint32_t (*count)(const PrStorage* storage);
    PrRawString (*key)(const PrStorage* storage, uint32_t position);
};

// Finishes the store's work, such as the writes that a disk store has queued, and frees it. Its records must be
// released first.
void prStorageDestroy(PrStorage* PR_NULLABLE storage);

// Reads the record at `key` into `*out`, and returns true if there is one. A record that the store has written or
// queued is read. In a store that evicts, reading a record makes it the most recently used. On false, `*out` is empty.
bool prStorageRead(PrStorage* storage, PrStringView key, PrStorageRecord* out);

// Releases a record that prStorageRead read, and empties it. An empty record is left as it is.
void prStorageRecordRelease(PrStorage* storage, PrStorageRecord* record);

// Replaces the record at `key` with a copy of `bytes`, makes it the most recently used, and returns true. A record that
// replaces another keeps its position (prStorageKey). Returns false if the record is not kept: a record larger than the
// store's limit, or, in a store that does not evict, one that would take the store past its limit. A store that evicts
// then removes the record that it would replace, and a store that does not evict is unchanged.
bool prStorageWrite(PrStorage* storage, PrStringView key, PrByteView bytes);

// Removes the record at `key`, if there is one.
void prStorageRemove(PrStorage* storage, PrStringView key);

// Removes every record.
void prStorageClear(PrStorage* storage);

// Returns the number of bytes that the store's records hold, their keys included.
uint64_t prStorageSize(const PrStorage* storage);

// Returns the number of records that the store holds.
uint32_t prStorageCount(const PrStorage* storage);

// Returns a copy of the key of the record at `position`, below prStorageCount, or the null string past the end. Each
// record keeps its position until a record is added or removed.
PrRawString prStorageKey(const PrStorage* storage, uint32_t position);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
