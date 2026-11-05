// The POSIX backend of files, which maps them with mmap. It serves the Unix-like targets, and it is the default binding
// of files on them.

#pragma once

#include "pr/base.h"
#include "pr/byte_view.h"
#include "pr/file.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Maps the file at `path` into `out`. Returns false and leaves `out` empty if the file cannot be mapped.
bool prCorePosixFileMap(PrStringView path, PrByteView* out);

// Unmaps `mapping`, which is not empty.
void prCorePosixFileUnmap(PrByteView* mapping);

// Reads what the file system records about the file at `path` into `out`. Returns false if there is no such file or it
// cannot be read.
bool prCorePosixFileInfo(PrStringView path, PrFileInfo* out);

// Returns true if `a` and `b` name one file, as prFileSame does: the same inode on the same device.
bool prCorePosixFileSame(PrStringView a, PrStringView b);

// Calls `fn` with each entry of the directory at `path`, as prFileEachEntry does, with names as the file system stores
// them, which need not be UTF-8.
bool prCorePosixFileEachEntry(PrStringView path, PrFileEntryFn fn, void* PR_NULLABLE context);

// Write, remove and create as prFileWrite, prFileRemove and prFileCreateDirectory do.
bool prCorePosixFileWrite(PrStringView path, PrByteView bytes);
bool prCorePosixFileRemove(PrStringView path);
bool prCorePosixFileCreateDirectory(PrStringView path);

// Return the directories of the user's caches and data, and the working directory, as prFileCacheDirectory,
// prFileDataDirectory and prFileWorkingDirectory do.
PrRawString prCorePosixFileCacheDirectory(void);
PrRawString prCorePosixFileDataDirectory(void);
PrRawString prCorePosixFileWorkingDirectory(void);

// Write and read the process's standard streams as prFileStreamWrite and prFileStreamRead do.
bool prCorePosixFileStreamWrite(PrFileStream stream, PrByteView bytes);
size_t prCorePosixFileStreamRead(PrFileStream stream, void* out, size_t capacity);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
