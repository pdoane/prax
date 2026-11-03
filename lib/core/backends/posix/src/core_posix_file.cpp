// The POSIX backend of files.

#include "pr/core_posix_file.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace pr_impl {

namespace {

// Returns a copy of `path` with a terminator, which the caller frees, or null if memory runs out.
char* PR_NULLABLE terminate(PrStringView path) {
    char* terminated = static_cast<char*>(malloc(path.size + 1));
    if (terminated == nullptr)
        return nullptr;
    if (path.size != 0)
        memcpy(terminated, path.data, path.size);
    terminated[path.size] = 0;
    return terminated;
}

// Returns the directory that the XDG base directory variable `xdg` names, or else `suffix` under the user's home.
// Apple's platforms name no such variables.
PrRawString userDirectory(const char* PR_NULLABLE xdg, const char* suffix) {
#if !defined(__APPLE__)
    const char* named = getenv(xdg);
    // The XDG base directory specification ignores a relative path.
    if (named != nullptr && named[0] == '/')
        return prRawStringCopy(PrStringView{named, strlen(named)});
#else
    (void)xdg;
#endif
    const char* home = getenv("HOME");
    if (home == nullptr || home[0] != '/')
        return prRawStringNull();
    PrRawString directory = prRawStringCopy(PrStringView{home, strlen(home)});
    prRawStringAppend(&directory, PrStringView{suffix, strlen(suffix)});
    return directory;
}

// Returns what `status` records, which stat or fstatat read.
PrFileInfo infoOf(const struct stat& status) {
#if defined(__APPLE__)
    const struct timespec& modified = status.st_mtimespec;
#else
    const struct timespec& modified = status.st_mtim;
#endif
    PrFileInfo info{};
    if (S_ISREG(status.st_mode)) {
        info.kind = PrFileKind_Regular;
        info.size = uint64_t(status.st_size);
    } else {
        info.kind = S_ISDIR(status.st_mode) ? PrFileKind_Directory : PrFileKind_Other;
    }
    info.modified = double(modified.tv_sec) * 1000.0 + double(modified.tv_nsec) / 1e6;
    return info;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

bool prCorePosixFileMap(PrStringView path, PrFileMapping* out) {
    *out = PrFileMapping{nullptr, 0};
    char* terminated = terminate(path);
    if (terminated == nullptr)
        return false;
    int fd = open(terminated, O_RDONLY | O_CLOEXEC);
    free(terminated);
    if (fd < 0)
        return false;
    struct stat info;
    bool mapped = false;
    if (fstat(fd, &info) == 0 && S_ISREG(info.st_mode)) {
        // An empty file has nothing to map.
        mapped = info.st_size == 0;
        void* data = mapped ? MAP_FAILED : mmap(nullptr, size_t(info.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
        if (data != MAP_FAILED) {
            *out = PrFileMapping{static_cast<const uint8_t*>(data), size_t(info.st_size)};
            mapped = true;
        }
    }
    close(fd);
    return mapped;
}

void prCorePosixFileUnmap(PrFileMapping* mapping) {
    munmap(const_cast<uint8_t*>(mapping->data), mapping->size);
}

bool prCorePosixFileInfo(PrStringView path, PrFileInfo* out) {
    *out = PrFileInfo{};
    char* terminated = terminate(path);
    if (terminated == nullptr)
        return false;
    struct stat status;
    bool found = stat(terminated, &status) == 0;
    free(terminated);
    if (found)
        *out = infoOf(status);
    return found;
}

bool prCorePosixFileSame(PrStringView a, PrStringView b) {
    struct stat found[2];
    PrStringView paths[2] = {a, b};
    for (int i = 0; i < 2; ++i) {
        char* terminated = terminate(paths[i]);
        if (terminated == nullptr)
            return false;
        bool read = stat(terminated, &found[i]) == 0;
        free(terminated);
        if (!read)
            return false;
    }
    return found[0].st_dev == found[1].st_dev && found[0].st_ino == found[1].st_ino;
}

bool prCorePosixFileEachEntry(PrStringView path, PrFileEntryFn fn, void* PR_NULLABLE context) {
    char* terminated = terminate(path);
    if (terminated == nullptr)
        return false;
    DIR* directory = opendir(terminated);
    free(terminated);
    if (directory == nullptr)
        return false;
    int fd = dirfd(directory);
    while (struct dirent* entry = readdir(directory)) {
        const char* name = entry->d_name;
        if (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0)))
            continue;
        // An entry that is removed after it is read, or a link that names nothing, is described as None.
        struct stat status;
        PrFileEntry described{.name = {name, strlen(name)}};
        if (fstatat(fd, name, &status, 0) == 0)
            described.info = infoOf(status);
        fn(context, &described);
    }
    closedir(directory);
    return true;
}

bool prCorePosixFileWrite(PrStringView path, PrByteView bytes) {
    // The bytes go to a file in the same directory, which takes the path's name once it is complete.
    size_t capacity = path.size + 64;
    char* temporary = static_cast<char*>(malloc(capacity));
    char* target = terminate(path);
    if (temporary == nullptr || target == nullptr) {
        free(temporary);
        free(target);
        return false;
    }
    // mkstemp makes a file of a name that no other write uses.
    snprintf(temporary, capacity, "%s.XXXXXX", target);
    bool written = false;
    int fd = mkstemp(temporary);
    if (fd >= 0) {
        fchmod(fd, 0644);
        written = true;
        size_t done = 0;
        while (done < bytes.size) {
            ssize_t wrote = write(fd, bytes.data + done, bytes.size - done);
            if (wrote < 0 && errno == EINTR)
                continue;
            if (wrote <= 0) {
                written = false;
                break;
            }
            done += size_t(wrote);
        }
        written = close(fd) == 0 && written;
        written = written && rename(temporary, target) == 0;
        if (!written)
            unlink(temporary);
    }
    free(temporary);
    free(target);
    return written;
}

bool prCorePosixFileRemove(PrStringView path) {
    char* terminated = terminate(path);
    if (terminated == nullptr)
        return false;
    bool removed = unlink(terminated) == 0;
    free(terminated);
    return removed;
}

bool prCorePosixFileCreateDirectory(PrStringView path) {
    char* terminated = terminate(path);
    if (terminated == nullptr)
        return false;
    // Each directory from the root down is made, so that a parent exists before its child.
    for (size_t i = 1; i <= path.size; ++i) {
        if (i != path.size && terminated[i] != '/')
            continue;
        char saved = terminated[i];
        terminated[i] = 0;
        if (mkdir(terminated, 0755) != 0 && errno != EEXIST) {
            free(terminated);
            return false;
        }
        terminated[i] = saved;
    }
    struct stat info;
    bool exists = stat(terminated, &info) == 0 && S_ISDIR(info.st_mode);
    free(terminated);
    return exists;
}

PrRawString prCorePosixFileCacheDirectory(void) {
#if defined(__APPLE__)
    return userDirectory(nullptr, "/Library/Caches");
#else
    return userDirectory("XDG_CACHE_HOME", "/.cache");
#endif
}

PrRawString prCorePosixFileDataDirectory(void) {
#if defined(__APPLE__)
    return userDirectory(nullptr, "/Library/Application Support");
#else
    return userDirectory("XDG_DATA_HOME", "/.local/share");
#endif
}
