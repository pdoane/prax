// Files by path, through the platform: files read into memory and written whole, what the file system records about
// them, the entries of directories, and the directories that the platform names for the user's caches and data. And the
// parts of paths, which are read from their text alone.

#pragma once

#include "pr/base.h"
#include "pr/byte_view.h"
#include "pr/raw_string.h"
#include "pr/string_view.h"
#include "pr/text_buffer.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Maps the file at the UTF-8 `path` and writes its bytes to `out`. The bytes are read only, and stay valid until
// prFileUnmap. The platform either maps the file or reads it. An empty file maps to an empty view. Returns false, and
// leaves `out` empty, if the file cannot be opened or is not a regular file.
bool prFileMap(PrStringView path, PrByteView* out);

// Unmaps the bytes that prFileMap wrote to `mapping`, and leaves `mapping` empty. An empty view is left as it is.
void prFileUnmap(PrByteView* mapping);

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

// Removes the file or the empty directory at the UTF-8 `path`. A mapping of the file stays valid. Returns false if
// there is no such file, or it cannot be removed, as a directory that holds files cannot.
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

// Returns the process's working directory, against which a relative path is read, or the null string if it cannot be
// read. The caller releases the string with prRawStringReset.
PrRawString prFileWorkingDirectory(void);

// The process's standard streams.
enum {
    PrFileStream_Input,
    PrFileStream_Output,
    PrFileStream_Error,
};
typedef uint8_t PrFileStream;

// Writes all of `bytes` to the output or error stream, as they are, with no translation of line endings and no
// buffering. Returns false if the stream cannot be written, as when it is closed.
bool prFileStreamWrite(PrFileStream stream, PrByteView bytes);

// Reads up to `capacity` bytes of the input stream into `out`, waiting until some are available, and returns how many
// it read. Returns zero at the end of the stream, and if it cannot be read.
size_t prFileStreamRead(PrFileStream stream, void* out, size_t capacity);

// The parts of a UTF-8 path, read from its text alone: the file system is not asked, and a part that names a link is
// not followed. A slash separates the parts, and so does a backslash on Windows and the web, whose file system can be a
// Windows host's.

// Returns the last part of `path`: what follows its last separator, or all of it if it has none.
PrStringView prFilePathName(PrStringView path);

// Returns `path` without its last part and the separators before it: the directory that holds what the path names.
// Returns the empty view for a path of one part, and the root, such as "/", for a part directly under the root.
PrStringView prFilePathParent(PrStringView path);

// Returns the extension of the last part of `path`, from its last dot: ".txt" for "notes.txt". Returns the empty view
// if the part has no dot, or its only dot begins it, as in ".profile".
PrStringView prFilePathExtension(PrStringView path);

// Appends `part` to the path in `path`, after a separator unless the path is empty or already ends in one.
void prFilePathAppend(PrTextBuffer* path, PrStringView part);

// Returns `path` as an absolute path: under the working directory if it is relative, and normalized from its text.
// Normalizing removes each empty part and each "." part, removes each ".." part with the part before it, and writes
// each separator as a slash. A ".." at the root stays at the root. A link is not followed, so the path names the file
// that `path` does only if no ".." part follows a link. On Windows, a path that begins with one separator is under the
// working directory's drive. Returns the null string if `path` is empty, or is relative and the working directory
// cannot be read. The caller releases the string with prRawStringReset.
PrRawString prFilePathAbsolute(PrStringView path);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
