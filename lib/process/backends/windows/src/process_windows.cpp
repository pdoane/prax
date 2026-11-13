// The launcher of Windows processes (process_windows.h). It starts each process with CreateProcessW, over named pipes
// whose parent ends are overlapped. A process on a terminal gets a pseudoconsole that is attached to such pipes. Each
// process has a thread that waits on the pipe operations, the process itself, and an event that wakes the thread.

#include "pr/process_windows.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/atomic.h"
#include "pr/mem.h"
#include "pr/thread.h"
#include "pr/windows_string.h"

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
//
// A process on a terminal has a pseudoconsole, which holds the child ends of its pipes. The pseudoconsole keeps its
// output open until it is closed, even after the process exits. A separate thread, `closer`, closes it once the process
// exits, while the serving thread goes on reading the remaining output. `console` is guarded by the process's mutex. It
// is null once closing begins, so a resize never uses a closed pseudoconsole. The input pipe of a process on a terminal
// stays open when the caller closes the input, because the pseudoconsole ends its programs when that pipe closes.
struct WindowsProcess : ProcessPiped {
    HANDLE handle;
    ProcessPipe input;
    ProcessPipe outputs[2];         // by PrProcessStream
    HANDLE wakeEvent;
    PrThread thread;
    bool stopping;
    bool onTerminal;
    HPCON console;
    PrThread closer;
    bool closerStarted;
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

// Appends `argument` to `line` as CommandLineToArgvW reads it back: quoted if it is empty or holds a space, a tab or a
// quote, with each quote escaped and the backslashes before a quote doubled. Returns false if `argument` does not
// widen.
bool appendArgument(RawArray<wchar_t>* line, PrStringView argument) {
    uint32_t length = 0;
    wchar_t* wide = prWindowsWidenCopy(argument, &length);
    if (wide == nullptr)
        return false;
    bool quoted = length == 0;
    for (size_t i = 0; i < length; ++i)
        quoted = quoted || wide[i] == L' ' || wide[i] == L'\t' || wide[i] == L'"';
    if (line->count != 0)
        line->push(L' ');
    if (!quoted) {
        line->append(wide, length);
        prMemoryFree(wide);
        return true;
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
    return true;
}

// Returns true if the environment block entry `entry` and `variable` have the same name. Both are NAME=value in UTF-16.
// Names compare without case, as Windows treats them. A leading `=` is part of the name, as in the block's entries for
// the current directory of each drive.
bool sameName(const wchar_t* entry, const wchar_t* variable) {
    int length = 1;
    while (variable[length] != L'=')
        ++length;
    int entryLength = 1;
    while (entry[entryLength] != 0 && entry[entryLength] != L'=')
        ++entryLength;
    return entryLength == length && CompareStringOrdinal(entry, length, variable, length, TRUE) == CSTR_EQUAL;
}

// Appends the process's environment block to `block`. It is the launcher's variables, except those that `desc` sets,
// followed by the variables of `desc`. Each variable ends with a NUL, and one more NUL ends the block. Returns false,
// and appends nothing, if a variable of `desc` does not widen.
bool buildEnvironment(const PrProcessDesc* desc, RawArray<wchar_t>* block) {
    RawArray<wchar_t*> variables;
    for (uint32_t i = 0; i < desc->environmentCount; ++i) {
        wchar_t* variable = prWindowsWidenCopy(desc->environment[i], nullptr);
        if (variable == nullptr) {
            for (wchar_t* widened : variables.span())
                prMemoryFree(widened);
            variables.reset();
            return false;
        }
        variables.push(variable);
    }
    wchar_t* inherited = GetEnvironmentStringsW();
    for (const wchar_t* entry = inherited; entry != nullptr && *entry != 0;) {
        size_t length = size_t(lstrlenW(entry));
        bool replaced = false;
        for (uint32_t i = 0; i < variables.count && !replaced; ++i)
            replaced = sameName(entry, variables.data[i]);
        if (!replaced)
            block->append(entry, uint32_t(length + 1));
        entry += length + 1;
    }
    if (inherited != nullptr)
        FreeEnvironmentStringsW(inherited);
    for (wchar_t* variable : variables.span()) {
        block->append(variable, uint32_t(lstrlenW(variable)) + 1);
        prMemoryFree(variable);
    }
    variables.reset();
    block->push(0);
    return true;
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
// it and the process is not on a terminal.
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
        if (closing && !process->onTerminal)
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

// Closes the pseudoconsole, if it is still open, once the process has exited.
void closeConsole(void* PR_NULLABLE context) {
    WindowsProcess* process = static_cast<WindowsProcess*>(context);
    prMutexLock(&process->mutex);
    HPCON console = process->console;
    process->console = nullptr;
    prMutexUnlock(&process->mutex);
    if (console != nullptr)
        ClosePseudoConsole(console);
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
            if (!process->closerStarted && process->console != nullptr) {
                process->closerStarted = true;
                prThreadStart(&process->closer, closeConsole, process);
            }
        }
    }
    cancel(process->outputs[0]);
    cancel(process->outputs[1]);
    cancel(process->input);
}

// Frees the process's handles. The output pipe closes first, because closing the pseudoconsole while the pipe is open
// waits for its output to be read.
void freeProcess(WindowsProcess* process) {
    closeHandle(&process->input.handle);
    closeHandle(&process->input.operation.hEvent);
    for (ProcessPipe& pipe : process->outputs) {
        closeHandle(&pipe.handle);
        closeHandle(&pipe.operation.hEvent);
    }
    if (process->closerStarted)
        prThreadJoin(&process->closer);
    closeConsole(process);
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

// A pseudoconsole has no pixel size, so `pixelExtent` is ignored.
void resizeTerminal(PrProcess* base, PrUVec2 extent, PrUVec2) {
    WindowsProcess* process = static_cast<WindowsProcess*>(base);
    prMutexLock(&process->mutex);
    if (process->console != nullptr)
        ResizePseudoConsole(process->console, COORD{SHORT(extent.x), SHORT(extent.y)});
    prMutexUnlock(&process->mutex);
}

// Starts the program on pipes or a pseudoconsole, and the thread that serves them.
PrStatus launch(PrProcessLauncher*, const PrProcessDesc* desc, PrProcess* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    // The command line, the working directory and the environment block, in UTF-16, before anything else is made.
    RawArray<wchar_t> line;
    RawArray<wchar_t> environment;
    wchar_t* PR_NULLABLE directory = nullptr;
    bool converted = appendArgument(&line, desc->program);
    for (uint32_t i = 0; i < desc->argumentCount && converted; ++i)
        converted = appendArgument(&line, desc->arguments[i]);
    line.push(0);
    if (converted && desc->directory.data != nullptr) {
        directory = prWindowsWidenCopy(desc->directory, nullptr);
        converted = directory != nullptr;
    }
    if (converted && desc->environmentCount != 0)
        converted = buildEnvironment(desc, &environment);
    if (!converted) {
        line.reset();
        environment.reset();
        prMemoryFree(directory);
        return PrStatus_InvalidData;
    }

    bool onTerminal = desc->streams == PrProcessStreams_Terminal;
    WindowsProcess* process = allocZeroed<WindowsProcess>();
    process->onTerminal = onTerminal;
    processPipedInit(process, desc, notify);
    process->destroyFn = destroyProcess;
    process->killFn = killProcess;
    process->handle = INVALID_HANDLE_VALUE;
    process->input.handle = INVALID_HANDLE_VALUE;
    process->outputs[0].handle = process->outputs[1].handle = INVALID_HANDLE_VALUE;
    HANDLE childInput = INVALID_HANDLE_VALUE;
    HANDLE childOutput = INVALID_HANDLE_VALUE;
    HANDLE childError = INVALID_HANDLE_VALUE;
    bool piped = openPipe(false, &process->input.handle, &childInput) &&
        openPipe(true, &process->outputs[PrProcessStream_Output].handle, &childOutput);
    SECURITY_ATTRIBUTES inherited{.nLength = sizeof(SECURITY_ATTRIBUTES), .bInheritHandle = TRUE};
    if (piped && onTerminal) {
        // The pseudoconsole reads the input pipe and writes the output pipe, which also carries standard error. It
        // duplicates the handles it is given, so this program closes its copies.
        COORD size{SHORT(desc->terminalExtent.x), SHORT(desc->terminalExtent.y)};
        piped = SUCCEEDED(CreatePseudoConsole(size, childInput, childOutput, 0, &process->console));
        closeHandle(&childInput);
        closeHandle(&childOutput);
        if (!piped)
            process->console = nullptr;
    } else if (piped && desc->errors == PrProcessErrors_Pipe) {
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
    if (!piped || (!onTerminal && childError == INVALID_HANDLE_VALUE)) {
        closeHandle(&childInput);
        closeHandle(&childOutput);
        closeHandle(&childError);
        freeProcess(process);
        line.reset();
        environment.reset();
        prMemoryFree(directory);
        return PrStatus_Io;
    }

    // Only the child's ends of the pipes pass to it, whatever else the program holds that is inheritable. A process on
    // a terminal inherits no handles, and its streams come from its pseudoconsole.
    HANDLE passed[3] = {childInput, childOutput, childError};
    SIZE_T listSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &listSize);
    LPPROC_THREAD_ATTRIBUTE_LIST list = static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(prMemoryAlloc(listSize));
    InitializeProcThreadAttributeList(list, 1, 0, &listSize);
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    if (onTerminal) {
        UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, process->console, sizeof(HPCON),
            nullptr, nullptr);
        // The standard handles are explicitly invalid, so the process uses its pseudoconsole instead of inheriting this
        // program's standard handles, which may be redirected.
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = INVALID_HANDLE_VALUE;
        startup.StartupInfo.hStdOutput = INVALID_HANDLE_VALUE;
        startup.StartupInfo.hStdError = INVALID_HANDLE_VALUE;
    } else {
        UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, passed, sizeof(passed), nullptr, nullptr);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = childInput;
        startup.StartupInfo.hStdOutput = childOutput;
        startup.StartupInfo.hStdError = childError;
    }
    startup.lpAttributeList = list;

    PROCESS_INFORMATION started{};
    DWORD flags = EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT;
    if (!onTerminal)
        flags |= CREATE_NO_WINDOW;
    BOOL created = CreateProcessW(nullptr, line.data, nullptr, nullptr, onTerminal ? FALSE : TRUE, flags,
        environment.data, directory, &startup.StartupInfo, &started);
    line.reset();
    environment.reset();
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
    if (onTerminal)
        process->terminalResizeFn = resizeTerminal;
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
    launcher->destroyFn = destroyLauncher;
    launcher->launchFn = launch;
    return launcher;
}
