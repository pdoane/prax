// A store that keeps its records on disk, in a directory of its own, across the runs of the program.

#pragma once

#include "pr/base.h"
#include "pr/status.h"
#include "pr/storage.h"
#include "pr/string_view.h"
#include "pr/task.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Where a store on disk keeps its records, how much they may hold, and the pool that writes them.
typedef struct PrStorageDiskDesc {
    PrStringView directory;     // the UTF-8 path of the store's directory, which is made if it is missing
    uint64_t limitBytes;
    PrStorageEviction eviction;

    // The pool whose threads write the store's files, in jobs (prTaskPoolPost). Any number of stores may share a pool,
    // which must outlive them.
    PrTaskPool* pool;
} PrStorageDiskDesc;

// Opens the store in `desc`'s directory, with the records that it held when it was last destroyed, and writes it to
// `*out`. Each record is a file of the directory, and an index file names them. A write is copied and queued, and a job
// of the store's pool writes it, after the writes queued before it: a read sees it at once, and the disk sees it soon
// after. A read maps the record's file. One store at a time may use a directory. Returns Io if the directory cannot be
// made. On failure, `*out` is null.
PrStatus prStorageDiskCreate(const PrStorageDiskDesc* desc, PrStorage* PR_NULLABLE* PR_NONNULL out);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
