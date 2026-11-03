// The Windows backend of files, which maps them with file mapping objects. It serves Windows, and it is the default
// binding of files there.

#pragma once

#include "pr/base.h"
#include "pr/byte_view.h"
#include "pr/file.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Maps the file at the UTF-8 `path` into `out`. Returns false and leaves `out` empty if the file cannot be mapped.
bool prCoreWindowsFileMap(PrStringView path, PrFileMapping* out);

// Unmaps `mapping`, which is not empty.
void prCoreWindowsFileUnmap(PrFileMapping* mapping);

// Reads what the file system records about the file at the UTF-8 `path` into `out`. Returns false if there is no such
// file or it cannot be read.
bool prCoreWindowsFileInfo(PrStringView path, PrFileInfo* out);

// Returns true if the UTF-8 paths `a` and `b` name one file, as prFileSame does: the same file index on the same
// volume.
bool prCoreWindowsFileSame(PrStringView a, PrStringView b);

// Calls `fn` with each entry of the directory at the UTF-8 `path`, as prFileEachEntry does, with names converted to
// UTF-8.
bool prCoreWindowsFileEachEntry(PrStringView path, PrFileEntryFn fn, void* PR_NULLABLE context);

// Write, remove and create as prFileWrite, prFileRemove and prFileCreateDirectory do.
bool prCoreWindowsFileWrite(PrStringView path, PrByteView bytes);
bool prCoreWindowsFileRemove(PrStringView path);
bool prCoreWindowsFileCreateDirectory(PrStringView path);

// Return the directories of the user's caches and data, as prFileCacheDirectory and prFileDataDirectory do.
PrString prCoreWindowsFileCacheDirectory(void);
PrString prCoreWindowsFileDataDirectory(void);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
