// The launcher of POSIX processes (process_posix.h). A process on pipes starts with posix_spawn, and a process on a
// terminal starts with fork and exec. Each process has a thread that polls its pipes or terminal, and a pipe that wakes
// that thread.

#include "pr/process_posix.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/mem.h"
#include "pr/process_backend.h"
#include "pr/string_ops.h"
#include "pr/thread.h"
#include "pr/utf8.h"

#include "process/backends/shared/src/process_shared_piped.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <crt_externs.h>
#include <libproc.h>
#include <sys/event.h>
#endif
#if defined(__linux__)
#include <sys/syscall.h>
#endif

namespace pr_impl {

namespace {

// The process's id, the parent's ends of its pipes (-1 once closed, or for a stream that is not a pipe), the pipe that
// wakes the thread, and the thread. `stopping` is guarded by the process's mutex.
//
// A process on a terminal uses the controlling side of the pseudoterminal in place of pipes. `input` and
// `outputs[PrProcessStream_Output]` are separate descriptors for it, which the thread writes and reads. `terminal` is a
// third descriptor that stays open until the process is destroyed, so a resize from the caller's thread never uses a
// descriptor that the thread has closed. The output ends once the process has exited and the thread has read everything
// it wrote, even if a background job still holds the terminal open. This matches a terminal emulator, whose window
// closes when its shell exits.
//
// `exitWatch` becomes readable when the process exits, so the thread sees the exit even if something else holds the
// output open. It is -1 if the system has no such descriptor, and the thread then polls for the exit.
struct PosixProcess : ProcessPiped {
    pid_t pid;
    int input;
    int outputs[2];                 // by PrProcessStream
    int wakePipe[2];
    int terminal;
    int exitWatch;
    PrThread thread;
    bool stopping;
};

// The longest path of the pseudoterminal's subsidiary side, in bytes.
constexpr size_t kTerminalPathMax = 128;

// The first and the longest interval between checks for the process's exit, in milliseconds, if there is no descriptor
// that reports the exit. The interval doubles after each check.
constexpr int kExitCheckFirst = 5;
constexpr int kExitCheckMax = 200;

// One past the highest descriptor that a forked child closes, if the system cannot list the open descriptors.
constexpr int kDescriptorLimitMax = 65536;

// Returns a descriptor that becomes readable once process `pid` exits, or -1 if the system has none.
int watchExit(pid_t pid) {
#if defined(__linux__) && defined(SYS_pidfd_open)
    // A pidfd closes on exec in any process that this one starts.
    return int(syscall(SYS_pidfd_open, pid, 0));
#elif defined(__APPLE__)
    int queue = kqueue();
    if (queue < 0)
        return -1;
    struct kevent change;
    EV_SET(&change, uintptr_t(pid), EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0, nullptr);
    if (kevent(queue, &change, 1, nullptr, 0, nullptr) != 0) {
        // The process has already exited. The thread's next check finds it.
        close(queue);
        return -1;
    }
    return queue;
#else
    (void)pid;
    return -1;
#endif
}

// Storage that a forked child uses to close its descriptors. The parent allocates it before the fork, because only
// async-signal-safe calls may follow a fork. On a Mac it holds room for the list of open descriptors.
struct CloseScratch {
#if defined(__APPLE__)
    proc_fdinfo* PR_NULLABLE list;
    int bytes;
#endif
};

CloseScratch closeScratchCreate() {
    CloseScratch scratch{};
#if defined(__APPLE__)
    // The system's estimate allows for a few descriptors that open meanwhile. The extra room allows for more.
    int bytes = proc_pidinfo(getpid(), PROC_PIDLISTFDS, 0, nullptr, 0);
    if (bytes > 0) {
        scratch.bytes = 2 * bytes + 64 * int(sizeof(proc_fdinfo));
        scratch.list = static_cast<proc_fdinfo*>(prMemoryAlloc(size_t(scratch.bytes)));
    }
#endif
    return scratch;
}

void closeScratchDestroy(CloseScratch* scratch) {
#if defined(__APPLE__)
    prMemoryFree(scratch->list);
    scratch->list = nullptr;
#else
    (void)scratch;
#endif
}

// Closes every descriptor from `first` on, in a forked child. This includes descriptors that another thread opened
// without FD_CLOEXEC. Linux closes the range in one call. A Mac closes the descriptors in its list of open ones. Other
// systems close each descriptor up to the system's limit.
void closeFrom(int first, const CloseScratch& scratch) {
#if defined(__linux__) && defined(SYS_close_range)
    (void)scratch;
    if (syscall(SYS_close_range, unsigned(first), ~0u, 0) == 0)
        return;
#elif defined(__APPLE__)
    if (scratch.list != nullptr) {
        int got = proc_pidinfo(getpid(), PROC_PIDLISTFDS, 0, scratch.list, scratch.bytes);
        if (got > 0) {
            int highest = -1;
            for (int i = 0; i < got / int(sizeof(proc_fdinfo)); ++i) {
                int descriptor = scratch.list[i].proc_fd;
                highest = descriptor > highest ? descriptor : highest;
                if (descriptor >= first)
                    close(descriptor);
            }
            // If the list filled its room, it may have left out descriptors above the highest one it holds.
            if (got + int(sizeof(proc_fdinfo)) <= scratch.bytes)
                return;
            first = highest + 1 > first ? highest + 1 : first;
        }
    }
#else
    (void)scratch;
#endif
    long limit = sysconf(_SC_OPEN_MAX);
    if (limit < 0 || limit > kDescriptorLimitMax)
        limit = kDescriptorLimitMax;
    for (int descriptor = first; descriptor < int(limit); ++descriptor)
        close(descriptor);
}

char** environment() {
#if defined(__APPLE__)
    return *_NSGetEnviron();
#else
    extern char** environ;
    return environ;
#endif
}

// Makes a pipe whose ends do not pass to processes that other threads start.
bool openPipe(int ends[2]) {
#if defined(__linux__)
    return pipe2(ends, O_CLOEXEC) == 0;
#else
    if (pipe(ends) != 0)
        return false;
    fcntl(ends[0], F_SETFD, FD_CLOEXEC);
    fcntl(ends[1], F_SETFD, FD_CLOEXEC);
    return true;
#endif
}

void closeEnd(int* end) {
    if (*end >= 0) {
        close(*end);
        *end = -1;
    }
}

// Returns a copy of `descriptor` that does not pass to processes that other threads start, or -1.
int duplicate(int descriptor) {
    return fcntl(descriptor, F_DUPFD_CLOEXEC, 0);
}

void setNonBlocking(int descriptor) {
    fcntl(descriptor, F_SETFL, fcntl(descriptor, F_GETFL) | O_NONBLOCK);
}

// A NUL-terminated copy of `text`.
char* copyString(PrStringView text) {
    char* copy = static_cast<char*>(prMemoryAlloc(text.size + 1));
    prMemCopy(copy, text.data, text.size);
    copy[text.size] = 0;
    return copy;
}

// The window size of a pseudoterminal with `extent` columns and rows, which fill `pixelExtent` pixels. Each value is
// clamped to 16 bits. The pixel size is zero if it is unknown.
winsize terminalSize(PrUVec2 extent, PrUVec2 pixelExtent) {
    auto clamp = [](uint32_t value) { return uint16_t(value < 0xffff ? value : 0xffff); };
    return winsize{
        .ws_row = clamp(extent.y),
        .ws_col = clamp(extent.x),
        .ws_xpixel = clamp(pixelExtent.x),
        .ws_ypixel = clamp(pixelExtent.y),
    };
}

// Opens a pseudoterminal with `extent` columns and rows, which fill `pixelExtent` pixels. The parent keeps the
// controlling side, `*controller`. The child opens the subsidiary side at `path` as its standard streams.
//
// The parent holds the subsidiary side open as `*held` until the child has opened it. Some systems reset a terminal's
// settings when it is opened while no one else has it open. If the system supports IUTF8, the terminal treats its input
// as UTF-8, so erasing a character in line editing erases all of its bytes.
bool openTerminal(PrUVec2 extent, PrUVec2 pixelExtent, int* controller, int* held, char (&path)[kTerminalPathMax]) {
    int descriptor = posix_openpt(O_RDWR | O_NOCTTY);
    if (descriptor < 0)
        return false;
    fcntl(descriptor, F_SETFD, FD_CLOEXEC);
    int other = -1;
    if (grantpt(descriptor) == 0 && unlockpt(descriptor) == 0 && ptsname_r(descriptor, path, sizeof(path)) == 0)
        other = open(path, O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (other < 0) {
        close(descriptor);
        return false;
    }
#if defined(IUTF8)
    termios settings{};
    if (tcgetattr(other, &settings) == 0) {
        settings.c_iflag |= IUTF8;
        tcsetattr(other, TCSANOW, &settings);
    }
#endif
    winsize size = terminalSize(extent, pixelExtent);
    ioctl(other, TIOCSWINSZ, &size);
    *controller = descriptor;
    *held = other;
    return true;
}

// Returns true if the environment entry `entry` and `variable` have the same name. Both are NAME=value.
bool sameName(const char* entry, PrStringView variable) {
    size_t length = prStringFindByte(variable, '=') + 1;
    for (size_t i = 0; i < length; ++i) {
        // The comparison stops at the entry's NUL, which matches no byte of the name or the `=`, so it never reads past
        // the end of the entry.
        if (entry[i] != variable.data[i])
            return false;
    }
    return true;
}

// Appends the process's environment to `out`, followed by a null. It is the launcher's variables, except those that
// `desc` sets, followed by the variables of `desc`. `owned` holds the copies of the variables of `desc`, which the
// caller frees.
void buildEnvironment(const PrProcessDesc* desc, RawArray<char*>* out, RawArray<char*>* owned) {
    for (char** entry = environment(); *entry != nullptr; ++entry) {
        bool replaced = false;
        for (uint32_t i = 0; i < desc->environmentCount && !replaced; ++i)
            replaced = sameName(*entry, desc->environment[i]);
        if (!replaced)
            out->push(*entry);
    }
    for (uint32_t i = 0; i < desc->environmentCount; ++i) {
        owned->push(copyString(desc->environment[i]));
        out->push(owned->last());
    }
    out->push(nullptr);
}

// Finds the file that `program` runs, as execvp does, and writes a copy of its path to `*resolved`. If `program`
// contains a slash, it is the path. Otherwise the path is the first executable file of that name in the directories of
// PATH in `envp`. The caller frees the copy. Returns false if no file is found.
bool resolveProgram(PrStringView program, char* const* envp, char** resolved) {
    if (prStringFindByte(program, '/') < program.size) {
        *resolved = copyString(program);
        return true;
    }
    PrStringView path = "/usr/bin:/bin";
    for (char* const* entry = envp; *entry != nullptr; ++entry) {
        if (prMemCompare(*entry, "PATH=", 5) == 0) {
            path = PrStringView(*entry + 5);
            break;
        }
    }
    while (true) {
        size_t end = prStringFindByte(path, ':');
        PrStringView directory{path.data, end};
        if (directory.size == 0)
            directory = ".";
        char* candidate = static_cast<char*>(prMemoryAlloc(directory.size + program.size + 2));
        prMemCopy(candidate, directory.data, directory.size);
        candidate[directory.size] = '/';
        prMemCopy(candidate + directory.size + 1, program.data, program.size);
        candidate[directory.size + 1 + program.size] = 0;
        if (access(candidate, X_OK) == 0) {
            *resolved = candidate;
            return true;
        }
        prMemoryFree(candidate);
        if (end >= path.size)
            return false;
        path = PrStringView{path.data + end + 1, path.size - end - 1};
    }
}

// Writes errno to `report` and exits a forked child that could not start. Only async-signal-safe calls may follow a
// fork.
[[noreturn]] void childFailed(int report) {
    int code = errno;
    (void)!write(report, &code, sizeof(code));
    _exit(127);
}

// Runs in a forked child and executes the program on the terminal at `terminalPath`. The child starts a new session and
// makes the terminal its controlling terminal. Opening the terminal does that on some systems, and TIOCSCTTY does it on
// the others. Only async-signal-safe calls may follow a fork, so the parent prepares everything the child needs.
[[noreturn]] void startOnTerminal(const char* terminalPath, const char* path, char* const* argv, char* const* envp,
        const char* PR_NULLABLE directory, int reportEnd, const CloseScratch& scratch) {
    setsid();
    // The terminal takes the three standard streams, so the report pipe moves to descriptor 3. It still closes on exec.
    constexpr int kReport = 3;
    if (reportEnd != kReport && dup2(reportEnd, kReport) < 0)
        _exit(127);
    fcntl(kReport, F_SETFD, FD_CLOEXEC);
    int descriptor = open(terminalPath, O_RDWR);
    if (descriptor < 0)
        childFailed(kReport);
#if defined(TIOCSCTTY)
    if (ioctl(descriptor, TIOCSCTTY, 0) != 0)
        childFailed(kReport);
#endif
    for (int stream = 0; stream < 3; ++stream) {
        if (dup2(descriptor, stream) < 0)
            childFailed(kReport);
    }
    if (descriptor > 2)
        close(descriptor);
    if (directory != nullptr && chdir(directory) != 0)
        childFailed(kReport);
    // The program starts with no signals blocked and default signal actions, whatever the forking thread had.
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, nullptr);
    struct sigaction defaults{};
    defaults.sa_handler = SIG_DFL;
    const int kSignals[] = {SIGPIPE, SIGINT, SIGQUIT, SIGHUP, SIGTERM, SIGCHLD, SIGTSTP, SIGTTIN, SIGTTOU, SIGWINCH};
    for (int signal : kSignals)
        sigaction(signal, &defaults, nullptr);
    // Every other descriptor closes now. The report closes on exec.
    closeFrom(kReport + 1, scratch);
    execve(path, argv, envp);
    childFailed(kReport);
}

// Starts `argv` on the terminal at `terminalPath` and writes its id to `*pid`. Returns once the program has started,
// which is after the child has opened the terminal. Returns NotFound if the program cannot be found or started, and Io
// if no process can be made.
PrStatus forkOnTerminal(const char* terminalPath, char* const* argv, char* const* envp,
        const char* PR_NULLABLE directory, pid_t* pid) {
    char* path = nullptr;
    if (!resolveProgram(PrStringView(argv[0]), envp, &path))
        return PrStatus_NotFound;
    int report[2] = {-1, -1};
    if (!openPipe(report)) {
        prMemoryFree(path);
        return PrStatus_Io;
    }
    CloseScratch scratch = closeScratchCreate();
    pid_t child = fork();
    if (child == 0)
        startOnTerminal(terminalPath, path, argv, envp, directory, report[1], scratch);
    closeScratchDestroy(&scratch);
    prMemoryFree(path);
    closeEnd(&report[1]);
    if (child < 0) {
        closeEnd(&report[0]);
        return PrStatus_Io;
    }
    // The report pipe closes empty on exec. If the child fails, it holds the errno.
    int code = 0;
    ssize_t got = 0;
    do {
        got = read(report[0], &code, sizeof(code));
    } while (got < 0 && errno == EINTR);
    closeEnd(&report[0]);
    if (got == ssize_t(sizeof(code))) {
        int status = 0;
        while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
        }
        return code == ENOENT || code == EACCES || code == ENOTDIR || code == ENOEXEC ? PrStatus_NotFound : PrStatus_Io;
    }
    *pid = child;
    return PrStatus_Ok;
}

void wakeThread(PosixProcess* process) {
    uint8_t byte = 1;
    // A full pipe already wakes the poll.
    (void)!write(process->wakePipe[1], &byte, 1);
}

void notify(ProcessPiped* process) {
    wakeThread(static_cast<PosixProcess*>(process));
}

// Reaps the process if it exited. The process's mutex is held, so that prProcessKill never signals an id that was
// reaped. Returns true if the process exited in this call.
bool reap(PosixProcess* process) {
    if (process->exited)
        return false;
    int status = 0;
    if (waitpid(process->pid, &status, WNOHANG) != process->pid)
        return false;
    process->exited = true;
    process->signaled = WIFSIGNALED(status);
    process->code = WIFSIGNALED(status) ? WTERMSIG(status) : WEXITSTATUS(status);
    return true;
}

// Ends the stream's output.
void endOutput(PosixProcess* process, PrProcessStream stream) {
    closeEnd(&process->outputs[stream]);
    processPipedEnd(process, stream);
}

// Reads what the stream's descriptor holds, while the stream has room. Returns true if it read everything. A process
// with a receive function stops after kProcessReadChunkBytes, so input that the receiver writes is sent between reads
// of a flood of output.
bool readOutput(PosixProcess* process, PrProcessStream stream) {
    uint8_t chunk[16 * 1024];
    size_t received = 0;
    for (;;) {
        ssize_t got = read(process->outputs[stream], chunk, sizeof(chunk));
        if (got > 0) {
            if (!processPipedReceive(process, stream, chunk, size_t(got)))
                return false;
            received += size_t(got);
            if (process->receive != nullptr && received >= kProcessReadChunkBytes)
                return false;
            continue;
        }
        if (got < 0 && errno == EINTR)
            continue;
        if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return true;
        endOutput(process, stream);
        return false;
    }
}

// Writes what is queued for the input until the pipe takes no more, and closes the input once the queue drains if the
// caller closed it.
void writeInput(PosixProcess* process) {
    for (;;) {
        // The caller appends while the thread writes, and an append may move the bytes, so they are copied.
        uint8_t chunk[16 * 1024];
        prMutexLock(&process->mutex);
        uint32_t size = process->queued.count - process->queuedStart;
        uint32_t taken = size < sizeof(chunk) ? size : uint32_t(sizeof(chunk));
        prMemCopy(chunk, process->queued.data + process->queuedStart, taken);
        bool closing = process->inputClosing;
        prMutexUnlock(&process->mutex);
        if (taken == 0) {
            if (closing)
                closeEnd(&process->input);
            return;
        }
        ssize_t sent = write(process->input, chunk, taken);
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
            return;
        prMutexLock(&process->mutex);
        if (sent < 0) {
            // The process closed its input. What is queued can never arrive.
            process->inputBroken = true;
            process->queued.clear();
            process->queuedStart = 0;
        } else {
            process->queuedStart += uint32_t(sent);
        }
        prMutexUnlock(&process->mutex);
        if (sent < 0) {
#if !defined(F_SETNOSIGPIPE)
            // The write raised SIGPIPE at this thread, which blocks it, so it is taken before it can reach anyone.
            sigset_t pipeSignal;
            sigemptyset(&pipeSignal);
            sigaddset(&pipeSignal, SIGPIPE);
            timespec none{};
            sigtimedwait(&pipeSignal, nullptr, &none);
#endif
            closeEnd(&process->input);
            return;
        }
    }
}

void serve(void* PR_NULLABLE context) {
    PosixProcess* process = static_cast<PosixProcess*>(context);
#if !defined(F_SETNOSIGPIPE)
    // A write to a pipe that the process closed raises SIGPIPE, which this thread blocks and takes (writeInput).
    sigset_t pipeSignal;
    sigemptyset(&pipeSignal);
    sigaddset(&pipeSignal, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &pipeSignal, nullptr);
#endif
    int exitCheck = kExitCheckFirst;
    for (;;) {
        prMutexLock(&process->mutex);
        bool stopping = process->stopping;
        bool pending = process->queuedStart < process->queued.count;
        bool closing = process->inputClosing;
        bool room[2];
        for (uint32_t i = 0; i < 2; ++i)
            room[i] = process->received[i].bytes.count - process->received[i].start < kProcessReceivedMaxBytes;
        bool exited = process->exited;
        prMutexUnlock(&process->mutex);
        if (stopping)
            break;
        if (process->input >= 0 && !pending && closing)
            closeEnd(&process->input);

        // A terminal's output ends once the process has exited and everything it wrote has been read.
        constexpr PrProcessStream kOutput = PrProcessStream_Output;
        if (exited && process->terminal >= 0 && process->outputs[kOutput] >= 0 && room[kOutput] &&
                readOutput(process, kOutput)) {
            endOutput(process, kOutput);
        }

        pollfd polls[5];
        uint32_t count = 0;
        polls[count++] = pollfd{process->wakePipe[0], POLLIN, 0};
        int polled[2] = {-1, -1};   // by PrProcessStream: the stream's entry in `polls`, or -1
        for (uint32_t i = 0; i < 2; ++i) {
            if (process->outputs[i] >= 0 && room[i]) {
                polled[i] = int(count);
                polls[count++] = pollfd{process->outputs[i], POLLIN, 0};
            }
        }
        int inputAt = -1;
        if (process->input >= 0 && pending) {
            inputAt = int(count);
            polls[count++] = pollfd{process->input, POLLOUT, 0};
        }
        int exitAt = -1;
        if (!exited && process->exitWatch >= 0) {
            exitAt = int(count);
            polls[count++] = pollfd{process->exitWatch, POLLIN, 0};
        }
        // Without a descriptor that reports the exit, the thread checks for it from time to time.
        bool checking = !exited && process->exitWatch < 0;
        int timeout = checking ? exitCheck : -1;
        int ready = poll(polls, nfds_t(count), timeout);
        if (ready < 0 && errno != EINTR)
            PR_FATAL("process: poll failed");
        if ((polls[0].revents & POLLIN) != 0) {
            uint8_t drained[64];
            while (read(process->wakePipe[0], drained, sizeof(drained)) > 0) {
            }
        }
        for (uint32_t i = 0; i < 2; ++i) {
            if (polled[i] >= 0 && (polls[polled[i]].revents & (POLLIN | POLLHUP | POLLERR)) != 0)
                readOutput(process, PrProcessStream(i));
        }
        if (inputAt >= 0 && (polls[inputAt].revents & (POLLOUT | POLLHUP | POLLERR)) != 0)
            writeInput(process);
        bool exitReady = exitAt >= 0 && (polls[exitAt].revents & (POLLIN | POLLHUP | POLLERR)) != 0;
        if (exitReady || checking) {
            prMutexLock(&process->mutex);
            bool reaped = reap(process);
            prMutexUnlock(&process->mutex);
            // If the watch reported an exit that the reap did not find, it reports nothing more, so polling takes over.
            if (reaped || exitReady)
                closeEnd(&process->exitWatch);
            if (reaped && process->wake != nullptr)
                process->wake(process->wakeContext);
            if (checking)
                exitCheck = exitCheck * 2 < kExitCheckMax ? exitCheck * 2 : kExitCheckMax;
        }
    }
}

void freeProcess(PosixProcess* process) {
    closeEnd(&process->input);
    closeEnd(&process->outputs[0]);
    closeEnd(&process->outputs[1]);
    closeEnd(&process->wakePipe[0]);
    closeEnd(&process->wakePipe[1]);
    closeEnd(&process->terminal);
    closeEnd(&process->exitWatch);
    processPipedReset(process);
    prMemoryFree(process);
}

void destroyProcess(PrProcess* base) {
    PosixProcess* process = static_cast<PosixProcess*>(base);
    prMutexLock(&process->mutex);
    process->stopping = true;
    if (!process->exited)
        kill(process->pid, SIGKILL);
    prMutexUnlock(&process->mutex);
    wakeThread(process);
    prThreadJoin(&process->thread);
    // The parent's ends close before the wait, because nothing reads them now. A process on a terminal may wait at exit
    // for its output to drain, and closing the controlling side ends that wait.
    closeEnd(&process->input);
    closeEnd(&process->outputs[0]);
    closeEnd(&process->outputs[1]);
    closeEnd(&process->terminal);
    if (!process->exited) {
        int status = 0;
        while (waitpid(process->pid, &status, 0) < 0 && errno == EINTR) {
        }
    }
    freeProcess(process);
}

void killProcess(PrProcess* base) {
    PosixProcess* process = static_cast<PosixProcess*>(base);
    prMutexLock(&process->mutex);
    if (!process->exited)
        kill(process->pid, SIGKILL);
    prMutexUnlock(&process->mutex);
}

void resizeTerminal(PrProcess* base, PrUVec2 extent, PrUVec2 pixelExtent) {
    PosixProcess* process = static_cast<PosixProcess*>(base);
    winsize size = terminalSize(extent, pixelExtent);
    ioctl(process->terminal, TIOCSWINSZ, &size);
}

// Returns a copy of the directory `path`, or the null string if it is empty or not UTF-8.
PrRawString directoryCopy(PrStringView path) {
    if (path.size == 0 || prUtf8Validate(path) != path.size)
        return prRawStringNull();
    return prRawStringCopy(path);
}

PrRawString terminalForegroundDirectory(const PrProcess* base) {
    const PosixProcess* process = static_cast<const PosixProcess*>(base);
    pid_t leader = tcgetpgrp(process->terminal);
    if (leader <= 0)
        return prRawStringNull();
#if defined(__APPLE__)
    proc_vnodepathinfo info{};
    int size = proc_pidinfo(leader, PROC_PIDVNODEPATHINFO, 0, &info, sizeof(info));
    if (size != int(sizeof(info)))
        return prRawStringNull();
    const char* path = info.pvi_cdir.vip_path;
    size_t length = 0;
    while (length < sizeof(info.pvi_cdir.vip_path) && path[length] != 0)
        ++length;
    return directoryCopy(PrStringView{path, length});
#elif defined(__linux__)
    // /proc/<leader>/cwd is a link to the directory.
    char link[32] = "/proc/";
    size_t at = 6;
    char digits[16];
    size_t count = 0;
    for (uint32_t id = uint32_t(leader); id != 0; id /= 10)
        digits[count++] = char('0' + id % 10);
    while (count != 0)
        link[at++] = digits[--count];
    prMemCopy(link + at, "/cwd", 5);
    char path[4096];
    ssize_t size = readlink(link, path, sizeof(path));
    if (size <= 0 || size_t(size) >= sizeof(path))
        return prRawStringNull();
    return directoryCopy(PrStringView{path, size_t(size)});
#else
    return prRawStringNull();
#endif
}

// Starts `argv` with posix_spawn over the child's ends of `input`, `output` and `error`, and writes its id to `*pid`.
// Returns NotFound if the program cannot be found or started, and Io if no process can be made.
PrStatus spawnOnPipes(const PrProcessDesc* desc, const int input[2], const int output[2], const int error[2],
        char* const* argv, char* const* envp, const char* PR_NULLABLE directory, pid_t* pid) {
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, input[0], 0);
    posix_spawn_file_actions_adddup2(&actions, output[1], 1);
    if (desc->errors == PrProcessErrors_Pipe)
        posix_spawn_file_actions_adddup2(&actions, error[1], 2);
    else if (desc->errors == PrProcessErrors_Discard)
        posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
#if defined(__APPLE__)
    else
        posix_spawn_file_actions_addinherit_np(&actions, 2);
#endif
    if (directory != nullptr) {
#if defined(__APPLE__)
        posix_spawn_file_actions_addchdir(&actions, directory);
#else
        posix_spawn_file_actions_addchdir_np(&actions, directory);
#endif
    }
    // The child starts with no signal blocked and SIGPIPE's default action, whatever the thread that starts it has.
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    sigset_t none;
    sigemptyset(&none);
    sigset_t defaults;
    sigemptyset(&defaults);
    sigaddset(&defaults, SIGPIPE);
    posix_spawnattr_setsigmask(&attributes, &none);
    posix_spawnattr_setsigdefault(&attributes, &defaults);
    short flags = POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
#if defined(__APPLE__)
    // Every other descriptor closes in the child, even one that another thread opened without FD_CLOEXEC.
    flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#endif
    posix_spawnattr_setflags(&attributes, flags);
    int spawned = posix_spawnp(pid, argv[0], &actions, &attributes, argv, envp);
    posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions);
    if (spawned == 0)
        return PrStatus_Ok;
    return spawned == ENOMEM || spawned == EAGAIN ? PrStatus_Io : PrStatus_NotFound;
}

// Starts the program on pipes or a terminal, and the thread that serves them.
PrStatus launch(PrProcessLauncher*, const PrProcessDesc* desc, PrProcess* PR_NULLABLE* PR_NONNULL out) {
    PosixProcess* process = allocZeroed<PosixProcess>();
    processPipedInit(process, desc, notify);
    process->destroyFn = destroyProcess;
    process->killFn = killProcess;
    process->input = -1;
    process->outputs[0] = process->outputs[1] = -1;
    process->wakePipe[0] = process->wakePipe[1] = -1;
    process->terminal = -1;
    process->exitWatch = -1;
    int input[2] = {-1, -1};
    int output[2] = {-1, -1};
    int error[2] = {-1, -1};
    bool onTerminal = desc->streams == PrProcessStreams_Terminal;
    char terminalPath[kTerminalPathMax];
    bool opened = false;
    if (onTerminal) {
        opened = openTerminal(desc->terminalExtent, desc->terminalPixelExtent, &output[0], &output[1], terminalPath) &&
            openPipe(process->wakePipe);
        if (opened) {
            input[1] = duplicate(output[0]);
            process->terminal = duplicate(output[0]);
            opened = input[1] >= 0 && process->terminal >= 0;
        }
    } else {
        opened = openPipe(input) && openPipe(output) && openPipe(process->wakePipe) &&
            (desc->errors != PrProcessErrors_Pipe || openPipe(error));
    }
    if (!opened) {
        int* ends[] = {&input[0], &input[1], &output[0], &output[1], &error[0], &error[1]};
        for (int* end : ends)
            closeEnd(end);
        freeProcess(process);
        return PrStatus_Io;
    }

    RawArray<char*> argv;
    argv.push(copyString(desc->program));
    for (uint32_t i = 0; i < desc->argumentCount; ++i)
        argv.push(copyString(desc->arguments[i]));
    argv.push(nullptr);
    RawArray<char*> variables;
    RawArray<char*> owned;
    char** envp = environment();
    if (desc->environmentCount != 0) {
        buildEnvironment(desc, &variables, &owned);
        envp = variables.data;
    }
    char* directory = desc->directory.data != nullptr ? copyString(desc->directory) : nullptr;
    pid_t pid = 0;
    PrStatus status = PrStatus_Ok;
    if (onTerminal)
        status = forkOnTerminal(terminalPath, argv.data, envp, directory, &pid);
    else
        status = spawnOnPipes(desc, input, output, error, argv.data, envp, directory, &pid);
    for (uint32_t i = 0; i + 1 < argv.count; ++i)
        prMemoryFree(argv.data[i]);
    argv.reset();
    for (char* variable : owned.span())
        prMemoryFree(variable);
    owned.reset();
    variables.reset();
    prMemoryFree(directory);
    closeEnd(&input[0]);
    closeEnd(&output[1]);
    closeEnd(&error[1]);
    if (status != PrStatus_Ok) {
        closeEnd(&input[1]);
        closeEnd(&output[0]);
        closeEnd(&error[0]);
        freeProcess(process);
        return status;
    }

    process->pid = pid;
    process->exitWatch = watchExit(pid);
    if (onTerminal) {
        process->terminalResizeFn = resizeTerminal;
        process->terminalForegroundDirectoryFn = terminalForegroundDirectory;
    }
    process->input = input[1];
    process->outputs[PrProcessStream_Output] = output[0];
    process->outputs[PrProcessStream_Error] = error[0];
    int ends[] = {process->input, process->outputs[0], process->outputs[1], process->wakePipe[0], process->wakePipe[1]};
    for (int end : ends) {
        if (end >= 0)
            setNonBlocking(end);
    }
#if defined(F_SETNOSIGPIPE)
    fcntl(process->input, F_SETNOSIGPIPE, 1);
#endif
    prThreadStart(&process->thread, serve, process);
    *out = process;
    return PrStatus_Ok;
}

void destroyLauncher(PrProcessLauncher* launcher) {
    prMemoryFree(launcher);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrProcessLauncher* prProcessPosixCreate(void) {
    PrProcessLauncher* launcher = allocZeroed<PrProcessLauncher>();
    launcher->destroyFn = destroyLauncher;
    launcher->launchFn = launch;
    return launcher;
}
