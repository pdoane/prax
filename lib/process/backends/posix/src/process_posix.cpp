// The launcher of POSIX processes (process_posix.h): posix_spawn, and one thread for each process that polls its pipes
// and a pipe that wakes it.

#include "pr/process_posix.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/mem.h"
#include "pr/thread.h"

#include "process/backends/shared/src/process_shared_piped.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <crt_externs.h>
#endif

namespace pr_impl {

namespace {

// The process's id, the parent's ends of its pipes (-1 once closed, or for a stream that is not a pipe), the pipe that
// wakes the thread, and the thread. `stopping` is guarded by the process's mutex.
struct PosixProcess : ProcessPiped {
    pid_t pid;
    int input;
    int outputs[2];                 // by PrProcessStream
    int wakePipe[2];
    PrThread thread;
    bool stopping;
};

// How long the thread first waits between checks for the process's exit once its output has ended, and how long at
// most, in milliseconds. A process that closes its output usually exits, so the first check comes soon.
constexpr int kExitCheckFirst = 5;
constexpr int kExitCheckMax = 200;

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

// Reads what the stream's pipe has, while the stream has room.
void readOutput(PosixProcess* process, PrProcessStream stream) {
    uint8_t chunk[16 * 1024];
    for (;;) {
        ssize_t got = read(process->outputs[stream], chunk, sizeof(chunk));
        if (got > 0) {
            if (!processPipedReceive(process, stream, chunk, size_t(got)))
                return;
            continue;
        }
        if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
            return;
        closeEnd(&process->outputs[stream]);
        processPipedEnd(process, stream);
        return;
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

        pollfd polls[4];
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
        // Once the output ends, nothing in the poll reports the exit, so the thread checks for it from time to time.
        bool outputsEnded = process->outputs[0] < 0 && process->outputs[1] < 0;
        int timeout = outputsEnded && !exited ? exitCheck : -1;
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
        if (outputsEnded && !exited) {
            prMutexLock(&process->mutex);
            bool reaped = reap(process);
            prMutexUnlock(&process->mutex);
            if (reaped && process->wake != nullptr)
                process->wake(process->wakeContext);
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

// Starts the program, its pipes and the thread that serves them.
PrStatus launch(PrProcessLauncher*, const PrProcessDesc* desc, PrProcess* PR_NULLABLE* PR_NONNULL out) {
    PosixProcess* process = allocZeroed<PosixProcess>();
    processPipedInit(process, desc, notify);
    process->destroy = destroyProcess;
    process->kill = killProcess;
    process->input = -1;
    process->outputs[0] = process->outputs[1] = -1;
    process->wakePipe[0] = process->wakePipe[1] = -1;
    int input[2] = {-1, -1};
    int output[2] = {-1, -1};
    int error[2] = {-1, -1};
    bool piped = openPipe(input) && openPipe(output) && openPipe(process->wakePipe) &&
        (desc->errors != PrProcessErrors_Pipe || openPipe(error));
    if (!piped) {
        int* ends[] = {&input[0], &input[1], &output[0], &output[1], &error[0], &error[1]};
        for (int* end : ends)
            closeEnd(end);
        freeProcess(process);
        return PrStatus_Io;
    }

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
    char* directory = nullptr;
    if (desc->directory.data != nullptr) {
        directory = copyString(desc->directory);
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

    Array<char*> argv;
    argv.push(copyString(desc->program));
    for (uint32_t i = 0; i < desc->argumentCount; ++i)
        argv.push(copyString(desc->arguments[i]));
    argv.push(nullptr);
    pid_t pid = 0;
    int spawned = posix_spawnp(&pid, argv.data[0], &actions, &attributes, argv.data, environment());
    for (uint32_t i = 0; i + 1 < argv.count; ++i)
        prMemoryFree(argv.data[i]);
    argv.reset();
    prMemoryFree(directory);
    posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions);
    closeEnd(&input[0]);
    closeEnd(&output[1]);
    closeEnd(&error[1]);
    if (spawned != 0) {
        closeEnd(&input[1]);
        closeEnd(&output[0]);
        closeEnd(&error[0]);
        freeProcess(process);
        return spawned == ENOMEM || spawned == EAGAIN ? PrStatus_Io : PrStatus_NotFound;
    }

    process->pid = pid;
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
    launcher->destroy = destroyLauncher;
    launcher->launch = launch;
    return launcher;
}
