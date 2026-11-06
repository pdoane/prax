// Processes: programs that the program starts, and the pipes or pseudoterminal that carry their standard streams.
//
// A launcher starts processes through a backend, which decides where they run and how. The backend serves each
// process's pipes or pseudoterminal, so writing never blocks the caller, and output can be read as it arrives.
//
// One thread uses a process at a time, with these exceptions. The wake and receive functions run on a thread of the
// backend. prProcessWrite and prProcessInfo are safe on any thread while the process lives, so a receive function can
// reply to what it receives.

#pragma once

#include "pr/base.h"
#include "pr/byte_view.h"
#include "pr/raw_string.h"
#include "pr/status.h"
#include "pr/string_view.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// A source of processes. Each backend provides its own constructor.
typedef struct PrProcessLauncher PrProcessLauncher;

// A program that a launcher started.
typedef struct PrProcess PrProcess;

// What a process's standard streams are.
enum {
    PrProcessStreams_Pipes,     // a pipe each for standard input and output, and standard error as `errors` says

    // One pseudoterminal for all three streams, so the process runs as it would in a terminal. Everything it writes,
    // control sequences included, is read as PrProcessStream_Output. The output ends once the process has exited and
    // its output has been read, even if a background job it started still holds the terminal open.
    PrProcessStreams_Terminal,
};
typedef uint8_t PrProcessStreams;

// What a process's standard error goes to, if its standard streams are pipes.
enum {
    PrProcessErrors_Pipe,       // a pipe that prProcessRead reads as PrProcessStream_Error
    PrProcessErrors_Inherit,    // the standard error of the program that started it
    PrProcessErrors_Discard,
};
typedef uint8_t PrProcessErrors;

// A stream that a process writes.
enum {
    PrProcessStream_Output,
    PrProcessStream_Error,
};
typedef uint8_t PrProcessStream;

// Called on a thread of the backend when output arrives after the caller has read everything before it, when the
// process's output ends, and when the process exits. Output passed to a receive function does not call the wake
// function. It must not call into the process. It usually wakes the thread that reads the process, for example with
// prAppWake.
typedef void (*PrProcessWakeFn)(void* PR_NULLABLE context);

// Called on a thread of the backend with the bytes of `stream` as they arrive, instead of holding them for
// prProcessRead. Calls are in order and never overlap. The bytes are valid only during the call. The backend reads no
// more output while the function runs, so a slow receiver makes the program wait, as an unread pipe does. The function
// may write to `process` and read its info, but must not destroy it.
typedef void (*PrProcessReceiveFn)(void* PR_NULLABLE context, PrProcess* process, PrProcessStream stream, PrByteView bytes);

// What to start.
typedef struct PrProcessDesc {
    // The program, as a path or as a name that the directories of the PATH environment variable are searched for, and
    // the arguments that follow its name.
    PrStringView program;
    const PrStringView* PR_NULLABLE arguments;
    uint32_t argumentCount;

    PrStringView directory;     // the process's working directory, or the null view for the launcher's

    // Environment variables to set, each as NAME=value. Each replaces the launcher's variable of the same name, and the
    // process inherits the launcher's other variables.
    const PrStringView* PR_NULLABLE environment;
    uint32_t environmentCount;

    PrProcessStreams streams;
    PrProcessErrors errors;
    PrUVec2 terminalExtent;      // columns and rows, each at least one, for PrProcessStreams_Terminal
    PrUVec2 terminalPixelExtent; // size of the columns and rows in pixels, or zero if unknown

    PrProcessWakeFn PR_NULLABLE wake;
    void* PR_NULLABLE wakeContext;

    // Takes the output as it arrives. If null, the output is held for prProcessRead.
    PrProcessReceiveFn PR_NULLABLE receive;
    void* PR_NULLABLE receiveContext;
} PrProcessDesc;

// What became of a process.
typedef struct PrProcessInfo {
    bool outputEnded;           // the process's output ended, and every byte of it was read or received
    bool exited;
    bool signaled;              // the process was ended by a signal, whose number `code` holds, rather than exiting
    int32_t code;               // the status that the process exited with, once `exited` is true
} PrProcessInfo;

// The function table of a launcher, placed at the start of the backend's state. Each member implements the public
// function of the same name. Callers use the functions below, never these members.
struct PrProcessLauncher {
    void (*destroy)(PrProcessLauncher* launcher);
    PrStatus (*launch)(PrProcessLauncher* launcher, const PrProcessDesc* desc, PrProcess* PR_NULLABLE* PR_NONNULL out);
};

// The function table of a process, placed at the start of the backend's state. Each member implements the public
// function of the same name. Callers use the functions below, never these members.
struct PrProcess {
    void (*destroy)(PrProcess* process);
    void (*write)(PrProcess* process, PrByteView bytes);
    void (*closeInput)(PrProcess* process);
    size_t (*read)(PrProcess* process, PrProcessStream stream, uint8_t* PR_NULLABLE output, size_t capacity);
    PrProcessInfo (*info)(const PrProcess* process);
    void (*kill)(PrProcess* process);
    void (*terminalResize)(PrProcess* process, PrUVec2 extent, PrUVec2 pixelExtent);
    PrRawString (*terminalForegroundDirectory)(const PrProcess* process);
};

// The library's backends of launchers, which the build turns on (README.md), and Default, the first that the build has.
enum {
    PrProcessBackend_Default,
    PrProcessBackend_Posix,
    PrProcessBackend_Windows,
    PrProcessBackend_Count,
};
typedef uint8_t PrProcessBackend;

// Returns the backend that `name` names, as prProcessBackendName spells it, ignoring case, or Count if it names none.
PrProcessBackend prProcessBackendFromName(PrStringView name);

// Returns the name of a backend in lowercase, as a command line spells it.
PrStringView prProcessBackendName(PrProcessBackend backend);

// Returns true if the build has the backend. The build has Default if it has any backend.
bool prProcessBackendIsBuilt(PrProcessBackend backend);

// Creates a launcher by the backend. Returns null if the build does not have the backend.
PrProcessLauncher* PR_NULLABLE prProcessLauncherCreate(PrProcessBackend backend);

// Frees a launcher. The processes that it started must be destroyed first.
void prProcessLauncherDestroy(PrProcessLauncher* PR_NULLABLE launcher);

// Starts the program that `desc` names and writes its process to `*out`. Returns NotFound if the program cannot be
// found or started, Io if the backend cannot make its pipes or terminal, and NotSupported if the launcher cannot start
// programs, or cannot start them on a terminal. On failure, `*out` is null.
PrStatus prProcessLaunch(PrProcessLauncher* launcher, const PrProcessDesc* desc, PrProcess* PR_NULLABLE* PR_NONNULL out);

// Ends the process if it is still running, as prProcessKill does, waits for it to exit, and frees it. The wake function
// is not called after this returns.
void prProcessDestroy(PrProcess* PR_NULLABLE process);

// Queues a copy of `bytes` for the process's standard input, after what was written before. A write after
// prProcessCloseInput, or once the process stops reading its input, is dropped. Safe on any thread while the process
// lives. Writes from different threads queue in the order they are made.
void prProcessWrite(PrProcess* process, PrByteView bytes);

// Closes the process's standard input once the bytes written before it reach the process, so that the process reads the
// end of its input. A terminal has no end of input, so on a terminal the process simply receives no more input, and
// later writes are dropped.
void prProcessCloseInput(PrProcess* process);

// Copies up to `capacity` bytes of `stream` that arrived into `output`, and returns how many it copied. Returns zero
// once every byte that arrived has been read, and always if the process has a receive function. A process holds a
// bounded amount of unread output and stops reading its pipe while that much is held, so a program that writes more
// waits for its reader.
size_t prProcessRead(PrProcess* process, PrProcessStream stream, uint8_t* PR_NULLABLE output, size_t capacity);

// Returns what became of the process. Safe on any thread while the process lives.
PrProcessInfo prProcessInfo(const PrProcess* process);

// Ends the process at once, without letting it clean up. A process that already exited is left as it is. A backend's
// own header says how it ends one.
void prProcessKill(PrProcess* process);

// Resizes the terminal of a process that was started on one. `extent` is the columns and rows, each at least one.
// `pixelExtent` is their size in pixels, or zero if unknown. Programs read it to size images. The process is told that
// the size changed, by SIGWINCH on POSIX. A backend whose terminals have no pixel size ignores `pixelExtent`.
void prProcessTerminalResize(PrProcess* process, PrUVec2 extent, PrUVec2 pixelExtent);

// Returns the working directory of the leader of the foreground process group on the process's terminal. That leader is
// the process itself, unless it moved a job into the foreground, as a shell does. Returns the null string if the
// process was not started on a terminal, if the leader has exited, or if the backend cannot read the directory. The
// caller releases the string with prRawStringReset.
PrRawString prProcessTerminalForegroundDirectory(const PrProcess* process);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
