// The Windows backend of files.

#include "pr/core_windows_file.h"

#include "pr/mem.h"
#include "pr/memory.h"
#include "pr/thread.h"
#include "pr/windows_string.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

// A watch on a directory: the change notification of the directory, the event that stops the thread, and the thread.
// The thread waits on both.
struct PrFileWatch {
    PrThread thread;
    HANDLE change;
    HANDLE stop;
    PrFileWatchFn fn;
    void* PR_NULLABLE context;
};

namespace pr_impl {

namespace {

// Unmaps the bytes of a blob of a mapped file.
void unmap(void* PR_NULLABLE, PrByteView bytes) {
    UnmapViewOfFile(bytes.data);
}

// Counts the files that writes have made beside the files that they replace, so that two writes of one process never
// share one.
volatile LONG writes = 0;

// Returns the NUL-terminated `wide` as UTF-8, or the null string if it is empty or is not valid UTF-16.
PrRawString narrow(const wchar_t* wide) {
    size_t units = size_t(lstrlenW(wide));
    return units != 0 ? prWindowsNarrowToRawString(wide, uint32_t(units)) : prRawStringNull();
}

// Returns the directory that the environment variable `name` holds, as UTF-8, or the null string if it holds none.
PrRawString directoryOf(const wchar_t* name) {
    wchar_t* value = nullptr;
    size_t count = 0;
    if (_wdupenv_s(&value, &count, name) != 0 || value == nullptr)
        return prRawStringNull();
    PrRawString directory = narrow(value);
    free(value);
    return directory;
}

// Returns what the attributes, size and time of last write that Windows records describe.
PrFileInfo infoOf(DWORD attributes, DWORD sizeHigh, DWORD sizeLow, FILETIME written) {
    uint64_t ticks = uint64_t(written.dwHighDateTime) << 32 | written.dwLowDateTime;
    PrFileInfo info{};
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        info.kind = PrFileKind_Directory;
    } else if ((attributes & FILE_ATTRIBUTE_DEVICE) != 0) {
        info.kind = PrFileKind_Other;
    } else {
        info.kind = PrFileKind_Regular;
        info.size = uint64_t(sizeHigh) << 32 | sizeLow;
    }
    info.modified = prTimeSystemFromWindowsFileTime(ticks);
    return info;
}

// Gives the open `file` the NUL-terminated name `target`, in place of any file there. The file that was there goes as a
// file that is removed does, so a handle open on it that shares deletion does not stop the rename. A file system that
// does not offer that falls back to a rename that only a file without open handles yields to.
bool renameOver(HANDLE file, const wchar_t* target) {
    size_t length = size_t(lstrlenW(target));
    size_t size = offsetof(FILE_RENAME_INFO, FileName) + (length + 1) * sizeof(wchar_t);
    FILE_RENAME_INFO* info = static_cast<FILE_RENAME_INFO*>(calloc(1, size));
    if (info == nullptr)
        return false;
    info->Flags = FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS;
    info->FileNameLength = DWORD(length * sizeof(wchar_t));
    prMemCopy(info->FileName, target, (length + 1) * sizeof(wchar_t));
    bool renamed = SetFileInformationByHandle(file, FileRenameInfoEx, info, DWORD(size)) != 0;
    DWORD error = renamed ? ERROR_SUCCESS : GetLastError();
    if (!renamed && (error == ERROR_INVALID_PARAMETER || error == ERROR_NOT_SUPPORTED)) {
        info->Flags = 0;
        info->ReplaceIfExists = TRUE;
        renamed = SetFileInformationByHandle(file, FileRenameInfo, info, DWORD(size)) != 0;
    }
    free(info);
    return renamed;
}

// Returns true if the NUL-terminated `path` names a regular file. Returns false if it names nothing, a directory or a
// device.
bool isRegular(const wchar_t* path) {
    DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_DEVICE)) == 0;
}

// Returns true if the NUL-terminated `path` names anything.
bool exists(const wchar_t* path) {
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

// Returns the status of a move or a copy that failed with `error`.
PrStatus failureOf(DWORD error) {
    return error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS ? PrStatus_NotAllowed : PrStatus_Io;
}

// Copies the regular file `from` to `to`, a path that names nothing. Removes any part of a copy that fails. Windows
// copies the time of the last change with the contents.
PrStatus copyTo(const wchar_t* from, const wchar_t* to) {
    if (CopyFileExW(from, to, nullptr, nullptr, nullptr, COPY_FILE_FAIL_IF_EXISTS))
        return PrStatus_Ok;
    // A copy that failed because a file is at `to` made nothing to remove.
    PrStatus status = failureOf(GetLastError());
    if (status != PrStatus_NotAllowed)
        DeleteFileW(to);
    return status;
}

// Moves the regular file `from` to `to`, a path that names nothing. On one volume, the move is a rename. Across
// volumes, the move is a copy, and the original is removed after the copy.
PrStatus moveTo(const wchar_t* from, const wchar_t* to) {
    // Without MOVEFILE_REPLACE_EXISTING, a file at `to` stops the move. Without MOVEFILE_COPY_ALLOWED, a move to
    // another volume fails. The copy below makes that move, so that a failed copy leaves the original.
    if (MoveFileExW(from, to, 0))
        return PrStatus_Ok;
    DWORD error = GetLastError();
    if (error != ERROR_NOT_SAME_DEVICE)
        return failureOf(error);
    PrStatus copied = copyTo(from, to);
    if (copied != PrStatus_Ok)
        return copied;
    if (DeleteFileW(from))
        return PrStatus_Ok;
    // The original stays, so the copy goes, and the file is where it was.
    DeleteFileW(to);
    return PrStatus_Io;
}

// Runs `op` with `from` and `to` as wide paths. `from` names a regular file, and `to` names nothing.
PrStatus withPaths(PrStringView from, PrStringView to, PrStatus (*op)(const wchar_t* from, const wchar_t* to)) {
    wchar_t* source = prWindowsWidenCopy(from, nullptr);
    wchar_t* target = prWindowsWidenCopy(to, nullptr);
    PrStatus status = PrStatus_NotFound;
    if (source != nullptr && target != nullptr && isRegular(source))
        status = exists(target) ? PrStatus_NotAllowed : op(source, target);
    prMemoryFree(source);
    prMemoryFree(target);
    return status;
}

void runWatch(void* PR_NULLABLE context) {
    PrFileWatch* watch = static_cast<PrFileWatch*>(context);
    HANDLE handles[2] = {watch->stop, watch->change};
    for (;;) {
        DWORD woken = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
        if (woken != WAIT_OBJECT_0 + 1)
            return;
        watch->fn(watch->context);
        if (!FindNextChangeNotification(watch->change))
            return;
    }
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrBlob* PR_NULLABLE prCoreWindowsFileOpen(PrStringView path) {
    wchar_t* wide = prWindowsWidenCopy(path, nullptr);
    if (wide == nullptr)
        return nullptr;
    // A directory does not open without FILE_FLAG_BACKUP_SEMANTICS, so only files reach the checks below.
    HANDLE file = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    prMemoryFree(wide);
    if (file == INVALID_HANDLE_VALUE)
        return nullptr;
    LARGE_INTEGER size;
    PrBlob* blob = nullptr;
    bool sized = GetFileType(file) == FILE_TYPE_DISK && GetFileSizeEx(file, &size);
    // An empty file has nothing to map.
    if (sized && size.QuadPart == 0)
        blob = prBlobCopy(PrByteView{});
    if (sized && size.QuadPart > 0 && uint64_t(size.QuadPart) <= SIZE_MAX) {
        HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (mapping != nullptr) {
            // The view keeps the mapping object alive, so neither handle outlives this call.
            void* data = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
            if (data != nullptr)
                blob = prBlobCreate(PrByteView{static_cast<const uint8_t*>(data), size_t(size.QuadPart)}, unmap, nullptr);
            CloseHandle(mapping);
        }
    }
    CloseHandle(file);
    return blob;
}

bool prCoreWindowsFileInfo(PrStringView path, PrFileInfo* out) {
    *out = PrFileInfo{};
    wchar_t* wide = prWindowsWidenCopy(path, nullptr);
    if (wide == nullptr)
        return false;
    WIN32_FILE_ATTRIBUTE_DATA data;
    bool found = GetFileAttributesExW(wide, GetFileExInfoStandard, &data) != 0;
    prMemoryFree(wide);
    if (found)
        *out = infoOf(data.dwFileAttributes, data.nFileSizeHigh, data.nFileSizeLow, data.ftLastWriteTime);
    return found;
}

bool prCoreWindowsFileSame(PrStringView a, PrStringView b) {
    BY_HANDLE_FILE_INFORMATION found[2];
    PrStringView paths[2] = {a, b};
    for (int i = 0; i < 2; ++i) {
        wchar_t* wide = prWindowsWidenCopy(paths[i], nullptr);
        if (wide == nullptr)
            return false;
        // No access is asked for, which reads what the file system records, and a directory opens only with backup
        // semantics.
        DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
        HANDLE file = CreateFileW(wide, 0, share, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        prMemoryFree(wide);
        if (file == INVALID_HANDLE_VALUE)
            return false;
        bool read = GetFileInformationByHandle(file, &found[i]) != 0;
        CloseHandle(file);
        if (!read)
            return false;
    }
    return found[0].dwVolumeSerialNumber == found[1].dwVolumeSerialNumber &&
        found[0].nFileIndexHigh == found[1].nFileIndexHigh && found[0].nFileIndexLow == found[1].nFileIndexLow;
}

bool prCoreWindowsFileEachEntry(PrStringView path, PrFileEntryFn fn, void* PR_NULLABLE context) {
    wchar_t* wide = prWindowsWidenCopy(path, nullptr);
    if (wide == nullptr)
        return false;
    // The pattern is the directory followed by a separator and a wildcard.
    size_t length = size_t(lstrlenW(wide));
    wchar_t* pattern = static_cast<wchar_t*>(malloc((length + 3) * sizeof(wchar_t)));
    if (pattern == nullptr) {
        prMemoryFree(wide);
        return false;
    }
    _snwprintf_s(pattern, length + 3, _TRUNCATE, L"%s\\*", wide);
    prMemoryFree(wide);
    WIN32_FIND_DATAW data;
    HANDLE find = FindFirstFileExW(pattern, FindExInfoBasic, &data, FindExSearchNameMatch, nullptr,
        FIND_FIRST_EX_LARGE_FETCH);
    free(pattern);
    if (find == INVALID_HANDLE_VALUE)
        return GetLastError() == ERROR_FILE_NOT_FOUND;
    do {
        const wchar_t* name = data.cFileName;
        if (name[0] == L'.' && (name[1] == 0 || (name[1] == L'.' && name[2] == 0)))
            continue;
        PrRawString utf8 = narrow(name);
        if (prRawStringIsNull(&utf8))
            continue;
        PrFileEntry entry{
            .name = prRawStringView(&utf8),
            .info = infoOf(data.dwFileAttributes, data.nFileSizeHigh, data.nFileSizeLow, data.ftLastWriteTime),
        };
        fn(context, &entry);
        prRawStringReset(&utf8);
    } while (FindNextFileW(find, &data));
    FindClose(find);
    return true;
}

bool prCoreWindowsFileWrite(PrStringView path, PrByteView bytes) {
    wchar_t* target = prWindowsWidenCopy(path, nullptr);
    if (target == nullptr)
        return false;
    // The bytes go to a file in the same directory, which takes the path's name once it is complete.
    size_t length = size_t(lstrlenW(target));
    size_t capacity = length + 64;
    wchar_t* temporary = static_cast<wchar_t*>(malloc(capacity * sizeof(wchar_t)));
    if (temporary == nullptr) {
        prMemoryFree(target);
        return false;
    }
    _snwprintf_s(temporary, capacity, _TRUNCATE, L"%s.%lu.%u.tmp", target, GetCurrentProcessId(),
        unsigned(InterlockedIncrement(&writes)));
    bool written = false;
    HANDLE file = CreateFileW(temporary, GENERIC_WRITE | DELETE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        written = true;
        size_t done = 0;
        while (done < bytes.size) {
            DWORD chunk = bytes.size - done > 0x40000000 ? 0x40000000 : DWORD(bytes.size - done);
            DWORD wrote = 0;
            if (!WriteFile(file, bytes.data + done, chunk, &wrote, nullptr) || wrote == 0) {
                written = false;
                break;
            }
            done += wrote;
        }
        written = written && renameOver(file, target);
        written = CloseHandle(file) && written;
        if (!written)
            DeleteFileW(temporary);
    }
    free(temporary);
    prMemoryFree(target);
    return written;
}

bool prCoreWindowsFileRemove(PrStringView path) {
    wchar_t* wide = prWindowsWidenCopy(path, nullptr);
    if (wide == nullptr)
        return false;
    DWORD attributes = GetFileAttributesW(wide);
    bool directory = attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    bool removed = (directory ? RemoveDirectoryW(wide) : DeleteFileW(wide)) != 0;
    prMemoryFree(wide);
    return removed;
}

bool prCoreWindowsFileCreateDirectory(PrStringView path) {
    wchar_t* wide = prWindowsWidenCopy(path, nullptr);
    if (wide == nullptr)
        return false;
    // Each directory from the root down is made, so that a parent exists before its child. A drive's root exists, and
    // making it fails, which the loop allows.
    size_t length = size_t(lstrlenW(wide));
    for (size_t i = 1; i <= length; ++i) {
        if (i != length && wide[i] != L'/' && wide[i] != L'\\')
            continue;
        wchar_t saved = wide[i];
        wide[i] = 0;
        if (!CreateDirectoryW(wide, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS &&
                GetLastError() != ERROR_ACCESS_DENIED) {
            prMemoryFree(wide);
            return false;
        }
        wide[i] = saved;
    }
    DWORD attributes = GetFileAttributesW(wide);
    prMemoryFree(wide);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

PrStatus prCoreWindowsFileMove(PrStringView from, PrStringView to) {
    return withPaths(from, to, moveTo);
}

PrStatus prCoreWindowsFileCopy(PrStringView from, PrStringView to) {
    return withPaths(from, to, copyTo);
}

PrFileWatch* PR_NULLABLE prCoreWindowsFileWatchCreate(PrStringView path, PrFileWatchFn fn, void* PR_NULLABLE context) {
    wchar_t* wide = prWindowsWidenCopy(path, nullptr);
    if (wide == nullptr)
        return nullptr;
    // The name filters cover files that are added, removed and renamed. The last write and size filters cover files
    // that are written in place.
    DWORD filter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE |
        FILE_NOTIFY_CHANGE_SIZE;
    HANDLE change = FindFirstChangeNotificationW(wide, FALSE, filter);
    prMemoryFree(wide);
    if (change == INVALID_HANDLE_VALUE)
        return nullptr;
    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (stop == nullptr) {
        FindCloseChangeNotification(change);
        return nullptr;
    }
    PrFileWatch* watch = static_cast<PrFileWatch*>(prMemoryAllocZeroed(sizeof(PrFileWatch)));
    watch->change = change;
    watch->stop = stop;
    watch->fn = fn;
    watch->context = context;
    prThreadStart(&watch->thread, runWatch, watch);
    return watch;
}

void prCoreWindowsFileWatchDestroy(PrFileWatch* watch) {
    SetEvent(watch->stop);
    prThreadJoin(&watch->thread);
    FindCloseChangeNotification(watch->change);
    CloseHandle(watch->stop);
    prMemoryFree(watch);
}

PrRawString prCoreWindowsFileUserDirectory(PrFileUserDirectory directory) {
    switch (directory) {
        case PrFileUserDirectory_Home:
            return directoryOf(L"USERPROFILE");
        case PrFileUserDirectory_Cache:
            return directoryOf(L"LOCALAPPDATA");
        case PrFileUserDirectory_Data:
            return directoryOf(L"APPDATA");
        case PrFileUserDirectory_Temporary:
            return directoryOf(L"TEMP");
        default:
            return prRawStringNull();
    }
}

PrRawString prCoreWindowsFileWorkingDirectory(void) {
    DWORD count = GetCurrentDirectoryW(0, nullptr);
    if (count == 0)
        return prRawStringNull();
    wchar_t* wide = static_cast<wchar_t*>(malloc(size_t(count) * sizeof(wchar_t)));
    if (wide == nullptr)
        return prRawStringNull();
    // The directory can change between the two calls, and a longer one is not written.
    DWORD written = GetCurrentDirectoryW(count, wide);
    PrRawString directory = written != 0 && written < count ? narrow(wide) : prRawStringNull();
    free(wide);
    return directory;
}

bool prCoreWindowsFileStreamWrite(PrFileStream stream, PrByteView bytes) {
    HANDLE handle = GetStdHandle(stream == PrFileStream_Error ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE)
        return false;
    size_t written = 0;
    while (written < bytes.size) {
        DWORD chunk = bytes.size - written > 0x40000000 ? 0x40000000 : DWORD(bytes.size - written);
        DWORD wrote = 0;
        if (!WriteFile(handle, bytes.data + written, chunk, &wrote, nullptr) || wrote == 0)
            return false;
        written += wrote;
    }
    return true;
}

size_t prCoreWindowsFileStreamRead(PrFileStream, void* out, size_t capacity) {
    HANDLE handle = GetStdHandle(STD_INPUT_HANDLE);
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE)
        return 0;
    DWORD chunk = capacity > 0x40000000 ? 0x40000000 : DWORD(capacity);
    DWORD got = 0;
    if (!ReadFile(handle, out, chunk, &got, nullptr))
        return 0;
    return got;
}

