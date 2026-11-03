// The launcher of Windows processes (process_windows.h): CreateProcessW, over named pipes whose parent's ends are
// overlapped, and one thread for each process that waits on the pipes' reads and writes, the process and an event that
// wakes it.

#include "pr/process_windows.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/atomic.h"
#include "pr/mem.h"
#include "pr/thread.h"

#include "process/backends/shared/src/process_shared_piped.h"

#include <windows.h>

namespace pr_impl {

namespace {

// One direction of a pipe whose end is overlapped: its handle, the operation in flight, and the bytes it moves.
struct ProcessPipe {
    HANDLE handle;                  // INVALID_HANDLE_VALUE once closed, or for a stream that is not a pipe
    OVERLAPPED operation;
    bool pending;
    uint8_t buffer[kProcessReadChunkBytes];
    DWORD size;                     // of a write in flight
};

// The process's handle, the parent's ends of its pipes, the event that wakes the thread, and the thread. `stopping` is
// guarded by the process's mutex.
struct WindowsProcess : ProcessPiped {
    HANDLE handle;
    ProcessPipe input;
    ProcessPipe outputs[2];         // by PrProcessStream
    HANDLE wakeEvent;
    PrThread thread;
    bool stopping;
};

// The bytes that the system buffers in each pipe.
constexpr DWORD kPipeBufferBytes = 64 * 1024;

// The number that makes each pipe's name unique in the program.
Atomic<uint32_t> pipeSerial;

void closeHandle(HANDLE* handle) {
    if (*handle != INVALID_HANDLE_VALUE && *handle != nullptr) {
        CloseHandle(*handle);
        *handle = INVALID_HANDLE_VALUE;
    }
}

// Converts UTF-8 to a NUL-terminated UTF-16 string that the caller frees.
wchar_t* wideString(PrStringView text) {
    int count = text.size != 0 ? MultiByteToWideChar(CP_UTF8, 0, text.data, int(text.size), nullptr, 0) : 0;
    wchar_t* wide = static_cast<wchar_t*>(prMemoryAlloc((size_t(count) + 1) * sizeof(wchar_t)));
    if (count != 0)
        MultiByteToWideChar(CP_UTF8, 0, text.data, int(text.size), wide, count);
    wide[count] = 0;
    return wide;
}

// Appends `argument` to `line` as CommandLineToArgvW reads it back: quoted if it is empty or holds a space, a tab or a
// quote, with each quote escaped and the backslashes before a quote doubled.
void appendArgument(RawArray<wchar_t>* line, PrStringView argument) {
    wchar_t* wide = wideString(argument);
    size_t length = 0;
    while (wide[length] != 0)
        ++length;
    bool quoted = length == 0;
    for (size_t i = 0; i < length; ++i)
        quoted = quoted || wide[i] == L' ' || wide[i] == L'\t' || wide[i] == L'"';
    if (line->count != 0)
        line->push(L' ');
    if (!quoted) {
        line->append(wide, uint32_t(length));
        prMemoryFree(wide);
        return;
    }
    line->push(L'"');
    size_t backslashes = 0;
    for (size_t i = 0; i < length; ++i) {
        if (wide[i] == L'\\') {
            ++backslashes;
            continue;
        }
        uint32_t repeat = uint32_t(wide[i] == L'"' ? backslashes * 2 + 1 : backslashes);
        for (uint32_t j = 0; j < repeat; ++j)
            line->push(L'\\');
        backslashes = 0;
        line->push(wide[i]);
    }
    for (size_t j = 0; j < backslashes * 2; ++j)
        line->push(L'\\');
    line->push(L'"');
    prMemoryFree(wide);
}

// Makes a pipe whose parent's end is overlapped and whose child's end the child inherits. `inbound` is true for a pipe
// that the parent reads.
bool openPipe(bool inbound, HANDLE* parent, HANDLE* child) {
    wchar_t name[96];
    wsprintfW(name, L"\\\\.\\pipe\\prax-process-%lu-%lu", GetCurrentProcessId(), pipeSerial.fetchAddRelaxed(1));
    DWORD access = (inbound ? PIPE_ACCESS_INBOUND : PIPE_ACCESS_OUTBOUND) | FILE_FLAG_OVERLAPPED |
        FILE_FLAG_FIRST_PIPE_INSTANCE;
    *parent = CreateNamedPipeW(name, access, PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
        kPipeBufferBytes, kPipeBufferBytes, 0, nullptr);
    if (*parent == INVALID_HANDLE_VALUE)
        return false;
    SECURITY_ATTRIBUTES inherited{.nLength = sizeof(SECURITY_ATTRIBUTES), .bInheritHandle = TRUE};
    *child = CreateFileW(name, inbound ? GENERIC_WRITE : GENERIC_READ, 0, &inherited, OPEN_EXISTING, 0, nullptr);
    if (*child == INVALID_HANDLE_VALUE) {
        closeHandle(parent);
        return false;
    }
    return true;
}

void notify(ProcessPiped* process) {
    SetEvent(static_cast<WindowsProcess*>(process)->wakeEvent);
}

// Starts a read of the stream's pipe if none is in flight and the stream has room, or ends the stream if its pipe
// ended.
void startRead(WindowsProcess* process, PrProcessStream stream) {
    ProcessPipe& pipe = process->outputs[stream];
    if (pipe.handle == INVALID_HANDLE_VALUE || pipe.pending || !processPipedHasRoom(process, stream))
        return;
    ResetEvent(pipe.operation.hEvent);
    if (ReadFile(pipe.handle, pipe.buffer, sizeof(pipe.buffer), nullptr, &pipe.operation) ||
            GetLastError() == ERROR_IO_PENDING) {
        pipe.pending = true;
        return;
    }
    closeHandle(&pipe.handle);
    processPipedEnd(process, stream);
}

void finishRead(WindowsProcess* process, PrProcessStream stream) {
    ProcessPipe& pipe = process->outputs[stream];
    pipe.pending = false;
    DWORD got = 0;
    if (GetOverlappedResult(pipe.handle, &pipe.operation, &got, FALSE) && got != 0) {
        processPipedReceive(process, stream, pipe.buffer, got);
        return;
    }
    if (GetLastError() == ERROR_MORE_DATA) {
        processPipedReceive(process, stream, pipe.buffer, got);
        return;
    }
    closeHandle(&pipe.handle);
    processPipedEnd(process, stream);
}

// Starts a write of what is queued if none is in flight, or closes the input once the queue drains if the caller closed
// it.
void startWrite(WindowsProcess* process) {
    ProcessPipe& pipe = process->input;
    if (pipe.handle == INVALID_HANDLE_VALUE || pipe.pending)
        return;
    prMutexLock(&process->mutex);
    uint32_t size = process->queued.count - process->queuedStart;
    uint32_t taken = size < sizeof(pipe.buffer) ? size : uint32_t(sizeof(pipe.buffer));
    prMemCopy(pipe.buffer, process->queued.data + process->queuedStart, taken);
    bool closing = process->inputClosing;
    prMutexUnlock(&process->mutex);
    if (taken == 0) {
        if (closing)
            closeHandle(&pipe.handle);
        return;
    }
    pipe.size = taken;
    ResetEvent(pipe.operation.hEvent);
    if (WriteFile(pipe.handle, pipe.buffer, taken, nullptr, &pipe.operation) || GetLastError() == ERROR_IO_PENDING) {
        pipe.pending = true;
        return;
    }
    prMutexLock(&process->mutex);
    process->inputBroken = true;
    process->queued.clear();
    process->queuedStart = 0;
    prMutexUnlock(&process->mutex);
    closeHandle(&pipe.handle);
}

void finishWrite(WindowsProcess* process) {
    ProcessPipe& pipe = process->input;
    pipe.pending = false;
    DWORD sent = 0;
    bool ok = GetOverlappedResult(pipe.handle, &pipe.operation, &sent, FALSE);
    prMutexLock(&process->mutex);
    if (ok) {
        process->queuedStart += sent;
    } else {
        process->inputBroken = true;
        process->queued.clear();
        process->queuedStart = 0;
    }
    prMutexUnlock(&process->mutex);
    if (!ok)
        closeHandle(&pipe.handle);
}

// Cancels the operation in flight on `pipe`, and waits for it to end, so that its buffer is no longer the system's.
void cancel(ProcessPipe& pipe) {
    if (!pipe.pending)
        return;
    CancelIoEx(pipe.handle, &pipe.operation);
    DWORD moved = 0;
    GetOverlappedResult(pipe.handle, &pipe.operation, &moved, TRUE);
    pipe.pending = false;
}

void serve(void* PR_NULLABLE context) {
    WindowsProcess* process = static_cast<WindowsProcess*>(context);
    bool reaped = false;
    for (;;) {
        prMutexLock(&process->mutex);
        bool stopping = process->stopping;
        prMutexUnlock(&process->mutex);
        if (stopping)
            break;
        startRead(process, PrProcessStream_Output);
        startRead(process, PrProcessStream_Error);
        startWrite(process);

        HANDLE waits[5];
        DWORD count = 0;
        waits[count++] = process->wakeEvent;
        int processAt = -1;
        if (!reaped) {
            processAt = int(count);
            waits[count++] = process->handle;
        }
        int outputAt[2] = {-1, -1};
        for (uint32_t i = 0; i < 2; ++i) {
            if (process->outputs[i].pending) {
                outputAt[i] = int(count);
                waits[count++] = process->outputs[i].operation.hEvent;
            }
        }
        int inputAt = -1;
        if (process->input.pending) {
            inputAt = int(count);
            waits[count++] = process->input.operation.hEvent;
        }
        DWORD woke = WaitForMultipleObjects(count, waits, FALSE, INFINITE);
        if (woke >= WAIT_OBJECT_0 + count)
            PR_FATAL("process: a wait failed");
        // Every object that is signaled is served, not only the first that the wait reports.
        for (uint32_t i = 0; i < 2; ++i) {
            if (outputAt[i] >= 0 && WaitForSingleObject(waits[outputAt[i]], 0) == WAIT_OBJECT_0)
                finishRead(process, PrProcessStream(i));
        }
        if (inputAt >= 0 && WaitForSingleObject(waits[inputAt], 0) == WAIT_OBJECT_0)
            finishWrite(process);
        if (processAt >= 0 && WaitForSingleObject(process->handle, 0) == WAIT_OBJECT_0) {
            DWORD code = 0;
            GetExitCodeProcess(process->handle, &code);
            reaped = true;
            processPipedExit(process, false, int32_t(code));
        }
    }
    cancel(process->outputs[0]);
    cancel(process->outputs[1]);
    cancel(process->input);
}

void freeProcess(WindowsProcess* process) {
    closeHandle(&process->input.handle);
    closeHandle(&process->input.operation.hEvent);
    for (ProcessPipe& pipe : process->outputs) {
        closeHandle(&pipe.handle);
        closeHandle(&pipe.operation.hEvent);
    }
    closeHandle(&process->wakeEvent);
    closeHandle(&process->handle);
    processPipedReset(process);
    prMemoryFree(process);
}

void destroyProcess(PrProcess* base) {
    WindowsProcess* process = static_cast<WindowsProcess*>(base);
    prMutexLock(&process->mutex);
    process->stopping = true;
    if (!process->exited)
        TerminateProcess(process->handle, 1);
    prMutexUnlock(&process->mutex);
    SetEvent(process->wakeEvent);
    prThreadJoin(&process->thread);
    WaitForSingleObject(process->handle, INFINITE);
    freeProcess(process);
}

void killProcess(PrProcess* base) {
    WindowsProcess* process = static_cast<WindowsProcess*>(base);
    prMutexLock(&process->mutex);
    if (!process->exited)
        TerminateProcess(process->handle, 1);
    prMutexUnlock(&process->mutex);
}

// Starts the program, its pipes and the thread that serves them.
PrStatus launch(PrProcessLauncher*, const PrProcessDesc* desc, PrProcess* PR_NULLABLE* PR_NONNULL out) {
    WindowsProcess* process = allocZeroed<WindowsProcess>();
    processPipedInit(process, desc, notify);
    process->destroy = destroyProcess;
    process->kill = killProcess;
    process->handle = INVALID_HANDLE_VALUE;
    process->input.handle = INVALID_HANDLE_VALUE;
    process->outputs[0].handle = process->outputs[1].handle = INVALID_HANDLE_VALUE;
    HANDLE childInput = INVALID_HANDLE_VALUE;
    HANDLE childOutput = INVALID_HANDLE_VALUE;
    HANDLE childError = INVALID_HANDLE_VALUE;
    bool piped = openPipe(false, &process->input.handle, &childInput) &&
        openPipe(true, &process->outputs[PrProcessStream_Output].handle, &childOutput);
    SECURITY_ATTRIBUTES inherited{.nLength = sizeof(SECURITY_ATTRIBUTES), .bInheritHandle = TRUE};
    if (piped && desc->errors == PrProcessErrors_Pipe) {
        piped = openPipe(true, &process->outputs[PrProcessStream_Error].handle, &childError);
    } else if (piped && desc->errors == PrProcessErrors_Discard) {
        childError = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherited, OPEN_EXISTING,
            0, nullptr);
    } else if (piped) {
        HANDLE own = GetStdHandle(STD_ERROR_HANDLE);
        if (own == nullptr || own == INVALID_HANDLE_VALUE ||
                !DuplicateHandle(GetCurrentProcess(), own, GetCurrentProcess(), &childError, 0, TRUE,
                    DUPLICATE_SAME_ACCESS))
            childError = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherited,
                OPEN_EXISTING, 0, nullptr);
    }
    if (!piped || childError == INVALID_HANDLE_VALUE) {
        closeHandle(&childInput);
        closeHandle(&childOutput);
        closeHandle(&childError);
        freeProcess(process);
        return PrStatus_Io;
    }

    // Only the child's ends of the pipes pass to it, whatever else the program holds that is inheritable.
    HANDLE passed[3] = {childInput, childOutput, childError};
    SIZE_T listSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &listSize);
    LPPROC_THREAD_ATTRIBUTE_LIST list = static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(prMemoryAlloc(listSize));
    InitializeProcThreadAttributeList(list, 1, 0, &listSize);
    UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, passed, sizeof(passed), nullptr, nullptr);
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = childInput;
    startup.StartupInfo.hStdOutput = childOutput;
    startup.StartupInfo.hStdError = childError;
    startup.lpAttributeList = list;

    RawArray<wchar_t> line;
    appendArgument(&line, desc->program);
    for (uint32_t i = 0; i < desc->argumentCount; ++i)
        appendArgument(&line, desc->arguments[i]);
    line.push(0);
    wchar_t* directory = desc->directory.data != nullptr ? wideString(desc->directory) : nullptr;
    PROCESS_INFORMATION started{};
    BOOL created = CreateProcessW(nullptr, line.data, nullptr, nullptr, TRUE,
        EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW, nullptr, directory,
        &startup.StartupInfo, &started);
    line.reset();
    prMemoryFree(directory);
    DeleteProcThreadAttributeList(list);
    prMemoryFree(list);
    closeHandle(&childInput);
    closeHandle(&childOutput);
    closeHandle(&childError);
    if (!created) {
        DWORD error = GetLastError();
        freeProcess(process);
        return error == ERROR_NOT_ENOUGH_MEMORY ? PrStatus_Io : PrStatus_NotFound;
    }
    CloseHandle(started.hThread);
    process->handle = started.hProcess;
    process->wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    process->input.operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    for (ProcessPipe& pipe : process->outputs)
        pipe.operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
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

PrProcessLauncher* prProcessWindowsCreate(void) {
    PrProcessLauncher* launcher = allocZeroed<PrProcessLauncher>();
    launcher->destroy = destroyLauncher;
    launcher->launch = launch;
    return launcher;
}
