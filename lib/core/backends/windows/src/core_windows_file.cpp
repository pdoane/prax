// The Windows backend of files.

#include "pr/core_windows_file.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

namespace pr_impl {

namespace {

// Counts the files that writes have made beside the files that they replace, so that two writes of one process never
// share one.
volatile LONG writes = 0;

// Returns `path` as a NUL-terminated UTF-16 string that the caller frees, or null if it is not valid UTF-8.
wchar_t* PR_NULLABLE widen(PrStringView path) {
    if (path.size > size_t(INT32_MAX))
        return nullptr;
    int count = path.size == 0
                    ? 0
                    : MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.data, int(path.size), nullptr, 0);
    if (path.size != 0 && count == 0)
        return nullptr;
    wchar_t* wide = static_cast<wchar_t*>(malloc((size_t(count) + 1) * sizeof(wchar_t)));
    if (wide == nullptr)
        return nullptr;
    if (count != 0)
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.data, int(path.size), wide, count);
    wide[count] = 0;
    return wide;
}

// Returns the NUL-terminated `wide` as UTF-8, or the null string if it is empty or is not valid UTF-16.
PrString narrow(const wchar_t* wide) {
    int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, nullptr, 0, nullptr, nullptr);
    char* utf8 = size > 1 ? static_cast<char*>(malloc(size_t(size))) : nullptr;
    PrString text = prStringNull();
    if (utf8 != nullptr
        && WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, utf8, size, nullptr, nullptr) == size)
        text = prStringCopy(PrStringView{utf8, size_t(size - 1)});
    free(utf8);
    return text;
}

// Returns the directory that the environment variable `name` holds, as UTF-8, or the null string if it holds none.
PrString directoryOf(const wchar_t* name) {
    wchar_t* value = nullptr;
    size_t count = 0;
    if (_wdupenv_s(&value, &count, name) != 0 || value == nullptr)
        return prStringNull();
    PrString directory = narrow(value);
    free(value);
    return directory;
}

// Returns what the attributes, size and time of last write that Windows records describe.
PrFileInfo infoOf(DWORD attributes, DWORD sizeHigh, DWORD sizeLow, FILETIME written) {
    // A FILETIME counts 100 ns intervals since 1601, which is 11644473600 seconds before the Unix epoch.
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
    info.modified = (double(ticks) - 116444736000000000.0) / 10000.0;
    return info;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

bool prCoreWindowsFileMap(PrStringView path, PrFileMapping* out) {
    *out = PrFileMapping{nullptr, 0};
    wchar_t* wide = widen(path);
    if (wide == nullptr)
        return false;
    // A directory does not open without FILE_FLAG_BACKUP_SEMANTICS, so only files reach the checks below.
    HANDLE file = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    free(wide);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER size;
    bool mapped = false;
    bool sized = GetFileType(file) == FILE_TYPE_DISK && GetFileSizeEx(file, &size);
    // An empty file has nothing to map.
    mapped = sized && size.QuadPart == 0;
    if (sized && size.QuadPart > 0 && uint64_t(size.QuadPart) <= SIZE_MAX) {
        HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (mapping != nullptr) {
            // The view keeps the mapping object alive, so neither handle outlives this call.
            void* data = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
            if (data != nullptr) {
                *out = PrFileMapping{static_cast<const uint8_t*>(data), size_t(size.QuadPart)};
                mapped = true;
            }
            CloseHandle(mapping);
        }
    }
    CloseHandle(file);
    return mapped;
}

void prCoreWindowsFileUnmap(PrFileMapping* mapping) {
    UnmapViewOfFile(mapping->data);
}

bool prCoreWindowsFileInfo(PrStringView path, PrFileInfo* out) {
    *out = PrFileInfo{};
    wchar_t* wide = widen(path);
    if (wide == nullptr)
        return false;
    WIN32_FILE_ATTRIBUTE_DATA data;
    bool found = GetFileAttributesExW(wide, GetFileExInfoStandard, &data) != 0;
    free(wide);
    if (found)
        *out = infoOf(data.dwFileAttributes, data.nFileSizeHigh, data.nFileSizeLow, data.ftLastWriteTime);
    return found;
}

bool prCoreWindowsFileEachEntry(PrStringView path, PrFileEntryFn fn, void* PR_NULLABLE context) {
    wchar_t* wide = widen(path);
    if (wide == nullptr)
        return false;
    // The pattern is the directory followed by a separator and a wildcard.
    size_t length = wcslen(wide);
    wchar_t* pattern = static_cast<wchar_t*>(malloc((length + 3) * sizeof(wchar_t)));
    if (pattern == nullptr) {
        free(wide);
        return false;
    }
    _snwprintf_s(pattern, length + 3, _TRUNCATE, L"%s\\*", wide);
    free(wide);
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
        PrString utf8 = narrow(name);
        if (prStringIsNull(&utf8))
            continue;
        PrFileEntry entry{.name = prStringView(&utf8),
                          .info = infoOf(data.dwFileAttributes, data.nFileSizeHigh, data.nFileSizeLow,
                                         data.ftLastWriteTime)};
        fn(context, &entry);
        prStringReset(&utf8);
    } while (FindNextFileW(find, &data));
    FindClose(find);
    return true;
}

bool prCoreWindowsFileWrite(PrStringView path, PrByteView bytes) {
    wchar_t* target = widen(path);
    if (target == nullptr)
        return false;
    // The bytes go to a file in the same directory, which takes the path's name once it is complete.
    size_t length = wcslen(target);
    size_t capacity = length + 64;
    wchar_t* temporary = static_cast<wchar_t*>(malloc(capacity * sizeof(wchar_t)));
    if (temporary == nullptr) {
        free(target);
        return false;
    }
    _snwprintf_s(temporary, capacity, _TRUNCATE, L"%s.%lu.%u.tmp", target, GetCurrentProcessId(),
                 unsigned(InterlockedIncrement(&writes)));
    bool written = false;
    HANDLE file = CreateFileW(temporary, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
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
        written = CloseHandle(file) && written;
        written = written && MoveFileExW(temporary, target, MOVEFILE_REPLACE_EXISTING);
        if (!written)
            DeleteFileW(temporary);
    }
    free(temporary);
    free(target);
    return written;
}

bool prCoreWindowsFileRemove(PrStringView path) {
    wchar_t* wide = widen(path);
    if (wide == nullptr)
        return false;
    bool removed = DeleteFileW(wide) != 0;
    free(wide);
    return removed;
}

bool prCoreWindowsFileCreateDirectory(PrStringView path) {
    wchar_t* wide = widen(path);
    if (wide == nullptr)
        return false;
    // Each directory from the root down is made, so that a parent exists before its child. A drive's root exists, and
    // making it fails, which the loop allows.
    size_t length = wcslen(wide);
    for (size_t i = 1; i <= length; ++i) {
        if (i != length && wide[i] != L'/' && wide[i] != L'\\')
            continue;
        wchar_t saved = wide[i];
        wide[i] = 0;
        if (!CreateDirectoryW(wide, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS
            && GetLastError() != ERROR_ACCESS_DENIED) {
            free(wide);
            return false;
        }
        wide[i] = saved;
    }
    DWORD attributes = GetFileAttributesW(wide);
    free(wide);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

PrString prCoreWindowsFileCacheDirectory(void) {
    return directoryOf(L"LOCALAPPDATA");
}

PrString prCoreWindowsFileDataDirectory(void) {
    return directoryOf(L"APPDATA");
}
