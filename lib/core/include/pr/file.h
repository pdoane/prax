// Files by path, through the platform: files read into memory and written whole, what the file system records about
// them, the entries of directories, and the directories that the platform names for the user's caches and data.

#pragma once

#include "pr/base.h"
#include "pr/byte_view.h"
#include "pr/raw_string.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The bytes of a file, read only, mapped or read by the platform. They stay valid until prFileUnmap.
typedef struct PrFileMapping {
    const uint8_t* PR_NULLABLE data;
    size_t size;
} PrFileMapping;

// Maps the file at the UTF-8 `path` into `out`. An empty file maps to an empty mapping. Returns false, and leaves `out`
// empty, if the file cannot be opened or is not a regular file.
bool prFileMap(PrStringView path, PrFileMapping* out);

// Unmaps the file and leaves `mapping` empty. An empty mapping is left as it is.
void prFileUnmap(PrFileMapping* mapping);

// What a path names.
enum {
    PrFileKind_None,            // nothing, or something that cannot be read
    PrFileKind_Regular,
    PrFileKind_Directory,
    PrFileKind_Other,           // a device, a pipe or a socket
};
typedef uint8_t PrFileKind;

// What the file system records about a file. A symbolic link is described by the file that it names.
typedef struct PrFileInfo {
    PrFileKind kind;
    uint64_t size;              // the number of bytes of a regular file, and zero for any other kind
    double modified;            // the time of the last change to the file's contents, in milliseconds since the Unix
                                // epoch, which prTimeWallMilliseconds also measures from
} PrFileInfo;

// Reads into `out` what the file system records about the file at the UTF-8 `path`. Returns false, and leaves `out`
// zeroed with kind None, if there is no such file or it cannot be read.
bool prFileInfo(PrStringView path, PrFileInfo* out);

// Returns true if the UTF-8 paths `a` and `b` name the same file, however each spells it: through a link, with other
// separators, or in another case on a file system that ignores case. Returns false if either names no file or cannot be
// read.
bool prFileSame(PrStringView a, PrStringView b);

// An entry of a directory: the name of a file in it, which is UTF-8 and has no separator, and what the file system
// records about that file.
typedef struct PrFileEntry {
    PrStringView name;
    PrFileInfo info;
} PrFileEntry;

// Called with each entry of a directory. The entry is valid only during the call.
typedef void (*PrFileEntryFn)(void* PR_NULLABLE context, const PrFileEntry* entry);

// Calls `fn` with each entry of the directory at the UTF-8 `path`, in no particular order. The directory's links to
// itself and to its parent are not entries. A name that is not valid UTF-8 is skipped. Returns false if the path is not
// a directory or cannot be read. The callback must not change the directory.
bool prFileEachEntry(PrStringView path, PrFileEntryFn fn, void* PR_NULLABLE context);

// Writes `bytes` as the whole of the file at the UTF-8 `path`, in place of any file there. A reader sees the file that
// was there or the new one, never part of either: the bytes go to a file beside it, which then takes its name. A
// mapping of the file that was there stays valid. Returns false if the file cannot be written.
bool prFileWrite(PrStringView path, PrByteView bytes);

// Removes the file at the UTF-8 `path`. A mapping of it stays valid. Returns false if there is no such file, or it
// cannot be removed.
bool prFileRemove(PrStringView path);

// Creates the directory at the UTF-8 `path`, and each directory above it that is missing. Returns true if the directory
// exists after the call.
bool prFileCreateDirectory(PrStringView path);

// Returns the directory where the platform keeps the user's caches, which applications share by naming their own
// directories under it: ~/Library/Caches on Apple's platforms, %LOCALAPPDATA% on Windows, and $XDG_CACHE_HOME or
// ~/.cache on other Unix-like platforms. Returns the null string if the platform names none. The caller releases the
// string with prRawStringReset.
PrRawString prFileCacheDirectory(void);

// Returns the directory where the platform keeps the data that applications keep for the user, as the cache directory
// does: ~/Library/Application Support on Apple's platforms, %APPDATA% on Windows, and $XDG_DATA_HOME or ~/.local/share
// on other Unix-like platforms. Returns the null string if the platform names none. The caller releases the string with
// prRawStringReset.
PrRawString prFileDataDirectory(void);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
