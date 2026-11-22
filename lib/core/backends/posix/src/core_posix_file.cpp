// The POSIX backend of files.

#include "pr/core_posix_file.h"

#include "pr/memory.h"
#include "pr/posix_string.h"
#include "pr/thread.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__linux__) && !defined(__EMSCRIPTEN__)
#include <poll.h>
#include <sys/inotify.h>
#include <sys/syscall.h>
#elif defined(__APPLE__)
#include <sys/event.h>
#endif

#if defined(__linux__) && !defined(__EMSCRIPTEN__)
// A watch on a directory: the inotify instance that watches the directory, the pipe that stops the thread when it is
// written, and the thread. The thread polls both.
struct PrFileWatch {
    PrThread thread;
    int notify;
    int stop[2];
    PrFileWatchFn fn;
    void* PR_NULLABLE context;
};
#elif defined(__APPLE__)
// A watch on a directory: the kqueue that watches the descriptor of the directory, and the thread. The queue has a user
// event that stops the thread. The thread waits on the queue.
struct PrFileWatch {
    PrThread thread;
    int queue;
    int directory;
    PrFileWatchFn fn;
    void* PR_NULLABLE context;
};
#endif

namespace pr_impl {

namespace {

// Unmaps the bytes of a blob of a mapped file.
void unmap(void* PR_NULLABLE, PrByteView bytes) {
    munmap(const_cast<uint8_t*>(bytes.data), bytes.size);
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
    info.modified = prTimeSystemFromUnix(int64_t(modified.tv_sec), uint32_t(modified.tv_nsec));
    return info;
}

// Returns true if `path` names a regular file, following any links.
bool isRegular(const char* path) {
    struct stat status;
    return stat(path, &status) == 0 && S_ISREG(status.st_mode);
}

// Returns true if `path` names anything, a dangling link included.
bool exists(const char* path) {
    struct stat status;
    return lstat(path, &status) == 0;
}

// Writes all of `size` bytes of `data` to `fd`. Returns false if a write fails.
bool writeAll(int fd, const uint8_t* data, size_t size) {
    while (size != 0) {
        ssize_t wrote = write(fd, data, size);
        if (wrote < 0 && errno == EINTR)
            continue;
        if (wrote <= 0)
            return false;
        data += wrote;
        size -= size_t(wrote);
    }
    return true;
}

// Copies the regular file `from` to `to`, a path that names nothing. Copies the contents, the permissions and the time
// of the last change. Removes any part of a copy that fails.
PrStatus copyTo(const char* from, const char* to) {
    int source = open(from, O_RDONLY | O_CLOEXEC);
    if (source < 0)
        return errno == ENOENT ? PrStatus_NotFound : PrStatus_Io;
    struct stat status;
    if (fstat(source, &status) != 0 || !S_ISREG(status.st_mode)) {
        close(source);
        return PrStatus_NotFound;
    }
    int target = open(to, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, status.st_mode & 0777);
    if (target < 0) {
        int error = errno;
        close(source);
        return error == EEXIST ? PrStatus_NotAllowed : PrStatus_Io;
    }
    constexpr size_t kChunk = 1u << 20;
    uint8_t* buffer = static_cast<uint8_t*>(malloc(kChunk));
    bool copied = buffer != nullptr;
    while (copied) {
        ssize_t got = read(source, buffer, kChunk);
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0) {
            copied = got == 0;
            break;
        }
        copied = writeAll(target, buffer, size_t(got));
    }
    free(buffer);
    close(source);
#if defined(__APPLE__)
    struct timespec times[2] = {status.st_atimespec, status.st_mtimespec};
#else
    struct timespec times[2] = {status.st_atim, status.st_mtim};
#endif
    copied = copied && futimens(target, times) == 0;
    copied = close(target) == 0 && copied;
    if (!copied)
        unlink(to);
    return copied ? PrStatus_Ok : PrStatus_Io;
}

// Renames the file `from` to `to`, a path that names nothing, on one volume. Returns 0, or the error that stopped the
// rename.
int renameExclusive(const char* from, const char* to) {
#if defined(__APPLE__)
    if (renamex_np(from, to, RENAME_EXCL) == 0)
        return 0;
    if (errno != ENOTSUP)
        return errno;
#elif defined(__linux__) && defined(SYS_renameat2)
    if (syscall(SYS_renameat2, AT_FDCWD, from, AT_FDCWD, to, 1u /* RENAME_NOREPLACE */) == 0)
        return 0;
    if (errno != ENOSYS && errno != EINVAL)
        return errno;
#endif
    // A file system without an exclusive rename gets a plain rename. A plain rename would replace a file that was made
    // at `to` after the caller looked for one.
    return rename(from, to) == 0 ? 0 : errno;
}

// Moves the regular file `from` to `to`, a path that names nothing. On one volume, the move is a rename. Across
// volumes, the move is a copy, and the original is removed after the copy.
PrStatus moveTo(const char* from, const char* to) {
    int error = renameExclusive(from, to);
    if (error == 0)
        return PrStatus_Ok;
    if (error != EXDEV)
        return error == EEXIST ? PrStatus_NotAllowed : PrStatus_Io;
    PrStatus copied = copyTo(from, to);
    if (copied != PrStatus_Ok)
        return copied;
    if (unlink(from) == 0)
        return PrStatus_Ok;
    // The original stays, so the copy goes, and the file is where it was.
    unlink(to);
    return PrStatus_Io;
}

// Runs `op` with `from` and `to` as NUL-terminated paths. `from` names a regular file, and `to` names nothing.
PrStatus withPaths(PrStringView from, PrStringView to, PrStatus (*op)(const char* from, const char* to)) {
    char* source = prPosixStringCopy(from);
    char* target = prPosixStringCopy(to);
    PrStatus status = PrStatus_NotFound;
    if (source != nullptr && target != nullptr && isRegular(source))
        status = exists(target) ? PrStatus_NotAllowed : op(source, target);
    prMemoryFree(source);
    prMemoryFree(target);
    return status;
}

#if defined(__linux__) && !defined(__EMSCRIPTEN__)
void runWatch(void* PR_NULLABLE context) {
    PrFileWatch* watch = static_cast<PrFileWatch*>(context);
    // The events are drained, but their kinds and names are not read. The callback reads the directory again.
    alignas(struct inotify_event) char events[4096];
    for (;;) {
        struct pollfd fds[2] = {
            {.fd = watch->stop[0], .events = POLLIN, .revents = 0}, {.fd = watch->notify, .events = POLLIN, .revents = 0},
        };
        int ready = poll(fds, 2, -1);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready < 0 || fds[0].revents != 0)
            return;
        if ((fds[1].revents & POLLIN) == 0)
            return;
        bool drained = false;
        while (read(watch->notify, events, sizeof events) > 0)
            drained = true;
        if (drained)
            watch->fn(watch->context);
    }
}
#elif defined(__APPLE__)
// The identity of the user event that stops a watch's thread.
constexpr uintptr_t kStopEvent = 1;

void runWatch(void* PR_NULLABLE context) {
    PrFileWatch* watch = static_cast<PrFileWatch*>(context);
    for (;;) {
        struct kevent event;
        int got = kevent(watch->queue, nullptr, 0, &event, 1, nullptr);
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0 || event.filter == EVFILT_USER)
            return;
        watch->fn(watch->context);
    }
}
#endif

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrBlob* PR_NULLABLE prCorePosixFileOpen(PrStringView path) {
    char* terminated = prPosixStringCopy(path);
    if (terminated == nullptr)
        return nullptr;
    int fd = open(terminated, O_RDONLY | O_CLOEXEC);
    prMemoryFree(terminated);
    if (fd < 0)
        return nullptr;
    struct stat info;
    PrBlob* blob = nullptr;
    if (fstat(fd, &info) == 0 && S_ISREG(info.st_mode)) {
        // An empty file has nothing to map.
        if (info.st_size == 0) {
            blob = prBlobCopy(PrByteView{});
        } else {
            void* data = mmap(nullptr, size_t(info.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
            if (data != MAP_FAILED)
                blob = prBlobCreate(PrByteView{static_cast<const uint8_t*>(data), size_t(info.st_size)}, unmap, nullptr);
        }
    }
    close(fd);
    return blob;
}

bool prCorePosixFileInfo(PrStringView path, PrFileInfo* out) {
    *out = PrFileInfo{};
    char* terminated = prPosixStringCopy(path);
    if (terminated == nullptr)
        return false;
    struct stat status;
    bool found = stat(terminated, &status) == 0;
    prMemoryFree(terminated);
    if (found)
        *out = infoOf(status);
    return found;
}

bool prCorePosixFileSame(PrStringView a, PrStringView b) {
    struct stat found[2];
    PrStringView paths[2] = {a, b};
    for (int i = 0; i < 2; ++i) {
        char* terminated = prPosixStringCopy(paths[i]);
        if (terminated == nullptr)
            return false;
        bool read = stat(terminated, &found[i]) == 0;
        prMemoryFree(terminated);
        if (!read)
            return false;
    }
    return found[0].st_dev == found[1].st_dev && found[0].st_ino == found[1].st_ino;
}

bool prCorePosixFileEachEntry(PrStringView path, PrFileEntryFn fn, void* PR_NULLABLE context) {
    char* terminated = prPosixStringCopy(path);
    if (terminated == nullptr)
        return false;
    DIR* directory = opendir(terminated);
    prMemoryFree(terminated);
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
    char* target = prPosixStringCopy(path);
    if (temporary == nullptr || target == nullptr) {
        free(temporary);
        prMemoryFree(target);
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
    prMemoryFree(target);
    return written;
}

bool prCorePosixFileRemove(PrStringView path) {
    char* terminated = prPosixStringCopy(path);
    if (terminated == nullptr)
        return false;
    // unlink refuses a directory, with EISDIR on Linux and EPERM on Apple's platforms, which rmdir removes if it is
    // empty.
    bool removed = unlink(terminated) == 0 || ((errno == EISDIR || errno == EPERM) && rmdir(terminated) == 0);
    prMemoryFree(terminated);
    return removed;
}

bool prCorePosixFileCreateDirectory(PrStringView path) {
    char* terminated = prPosixStringCopy(path);
    if (terminated == nullptr)
        return false;
    // Each directory from the root down is made, so that a parent exists before its child. A directory that exists may
    // refuse to be made with an error other than EEXIST, as a drive's root does under Node.js on Windows, so it is
    // looked at instead.
    struct stat info;
    for (size_t i = 1; i <= path.size; ++i) {
        if (i != path.size && terminated[i] != '/')
            continue;
        char saved = terminated[i];
        terminated[i] = 0;
        if (mkdir(terminated, 0755) != 0 && errno != EEXIST && !(stat(terminated, &info) == 0 && S_ISDIR(info.st_mode))) {
            prMemoryFree(terminated);
            return false;
        }
        terminated[i] = saved;
    }
    bool exists = stat(terminated, &info) == 0 && S_ISDIR(info.st_mode);
    prMemoryFree(terminated);
    return exists;
}

PrStatus prCorePosixFileMove(PrStringView from, PrStringView to) {
    return withPaths(from, to, moveTo);
}

PrStatus prCorePosixFileCopy(PrStringView from, PrStringView to) {
    return withPaths(from, to, copyTo);
}

PrFileWatch* PR_NULLABLE prCorePosixFileWatchCreate(PrStringView path, PrFileWatchFn fn, void* PR_NULLABLE context) {
#if defined(__linux__) && !defined(__EMSCRIPTEN__)
    char* terminated = prPosixStringCopy(path);
    if (terminated == nullptr)
        return nullptr;
    int notify = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    uint32_t mask = IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_CLOSE_WRITE | IN_DELETE_SELF |
        IN_MOVE_SELF | IN_ONLYDIR;
    bool watching = notify >= 0 && inotify_add_watch(notify, terminated, mask) >= 0;
    prMemoryFree(terminated);
    int stop[2] = {-1, -1};
    if (!watching || pipe2(stop, O_CLOEXEC) != 0) {
        if (notify >= 0)
            close(notify);
        return nullptr;
    }
    PrFileWatch* watch = static_cast<PrFileWatch*>(prMemoryAllocZeroed(sizeof(PrFileWatch)));
    watch->notify = notify;
    watch->stop[0] = stop[0];
    watch->stop[1] = stop[1];
    watch->fn = fn;
    watch->context = context;
    prThreadStart(&watch->thread, runWatch, watch);
    return watch;
#elif defined(__APPLE__)
    char* terminated = prPosixStringCopy(path);
    if (terminated == nullptr)
        return nullptr;
    // O_EVTONLY asks for events alone, so the descriptor does not keep the directory's volume from unmounting.
    int directory = open(terminated, O_EVTONLY | O_DIRECTORY | O_CLOEXEC);
    prMemoryFree(terminated);
    if (directory < 0)
        return nullptr;
    int queue = kqueue();
    if (queue < 0) {
        close(directory);
        return nullptr;
    }
    // A directory is written when its entries are added, removed or renamed.
    struct kevent events[2];
    EV_SET(&events[0], uintptr_t(directory), EVFILT_VNODE, EV_ADD | EV_CLEAR,
        NOTE_WRITE | NOTE_DELETE | NOTE_RENAME | NOTE_EXTEND | NOTE_ATTRIB | NOTE_REVOKE, 0, nullptr);
    EV_SET(&events[1], kStopEvent, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, nullptr);
    if (kevent(queue, events, 2, nullptr, 0, nullptr) != 0) {
        close(queue);
        close(directory);
        return nullptr;
    }
    PrFileWatch* watch = static_cast<PrFileWatch*>(prMemoryAllocZeroed(sizeof(PrFileWatch)));
    watch->queue = queue;
    watch->directory = directory;
    watch->fn = fn;
    watch->context = context;
    prThreadStart(&watch->thread, runWatch, watch);
    return watch;
#else
    // A target that has no watches.
    (void)path;
    (void)fn;
    (void)context;
    return nullptr;
#endif
}

void prCorePosixFileWatchDestroy(PrFileWatch* watch) {
#if defined(__linux__) && !defined(__EMSCRIPTEN__)
    char byte = 0;
    while (write(watch->stop[1], &byte, 1) < 0 && errno == EINTR) {
    }
    prThreadJoin(&watch->thread);
    close(watch->notify);
    close(watch->stop[0]);
    close(watch->stop[1]);
    prMemoryFree(watch);
#elif defined(__APPLE__)
    struct kevent stop;
    EV_SET(&stop, kStopEvent, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
    kevent(watch->queue, &stop, 1, nullptr, 0, nullptr);
    prThreadJoin(&watch->thread);
    close(watch->queue);
    close(watch->directory);
    prMemoryFree(watch);
#else
    // No watch is created, so none is destroyed.
    (void)watch;
#endif
}

PrRawString prCorePosixFileUserDirectory(PrFileUserDirectory directory) {
    switch (directory) {
        case PrFileUserDirectory_Home: {
            const char* home = getenv("HOME");
            if (home == nullptr || home[0] != '/')
                return prRawStringNull();
            return prRawStringCopy(PrStringView{home, strlen(home)});
        }
        case PrFileUserDirectory_Cache:
#if defined(__APPLE__)
            return userDirectory(nullptr, "/Library/Caches");
#else
            return userDirectory("XDG_CACHE_HOME", "/.cache");
#endif
        case PrFileUserDirectory_Data:
#if defined(__APPLE__)
            return userDirectory(nullptr, "/Library/Application Support");
#else
            return userDirectory("XDG_DATA_HOME", "/.local/share");
#endif
        case PrFileUserDirectory_Temporary: {
            const char* variable = getenv("TMPDIR");
            PrStringView temporary = "/tmp";
            if (variable != nullptr && variable[0] == '/')
                temporary = PrStringView{variable, strlen(variable)};
            while (temporary.size > 1 && temporary.data[temporary.size - 1] == '/')
                --temporary.size;
            return prRawStringCopy(temporary);
        }
        default:
            return prRawStringNull();
    }
}

PrRawString prCorePosixFileWorkingDirectory(void) {
    // getcwd reports ERANGE while the buffer is too small for the directory, so the buffer grows until it fits.
    for (size_t capacity = 256; capacity <= (size_t(1) << 20); capacity *= 2) {
        char* buffer = static_cast<char*>(malloc(capacity));
        if (buffer == nullptr)
            return prRawStringNull();
        if (getcwd(buffer, capacity) != nullptr) {
            PrRawString directory = prRawStringCopy(PrStringView{buffer, strlen(buffer)});
            free(buffer);
            return directory;
        }
        free(buffer);
        if (errno != ERANGE)
            return prRawStringNull();
    }
    return prRawStringNull();
}

bool prCorePosixFileStreamWrite(PrFileStream stream, PrByteView bytes) {
    int descriptor = stream == PrFileStream_Error ? STDERR_FILENO : STDOUT_FILENO;
    size_t written = 0;
    while (written < bytes.size) {
        ssize_t wrote = write(descriptor, bytes.data + written, bytes.size - written);
        if (wrote < 0 && errno == EINTR)
            continue;
        if (wrote <= 0)
            return false;
        written += size_t(wrote);
    }
    return true;
}

size_t prCorePosixFileStreamRead(PrFileStream, void* out, size_t capacity) {
    for (;;) {
        ssize_t got = read(STDIN_FILENO, out, capacity);
        if (got < 0 && errno == EINTR)
            continue;
        return got > 0 ? size_t(got) : 0;
    }
}

