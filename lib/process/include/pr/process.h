// Processes: programs that the program starts, and the pipes to their standard streams.
//
// A launcher starts processes, by a backend that decides where they run and how. A process's input and output are pipes
// that the backend serves, so that writing never blocks the caller and output is read as it arrives. One thread uses a
// process at a time. The wake function is the exception: it runs on a thread of the backend.

#pragma once

#include "pr/base.h"
#include "pr/byte_view.h"
#include "pr/status.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// A source of processes. Each backend provides its own constructor.
typedef struct PrProcessLauncher PrProcessLauncher;

// A program that a launcher started.
typedef struct PrProcess PrProcess;

// What a process's standard error goes to. Its standard input and output are always pipes.
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

// Called on a thread of the backend when output arrives that the caller had read all of before, when the process's
// output ends, and when the process exits. It must not call into the process, and usually wakes the thread that reads
// it, as prAppWake does.
typedef void (*PrProcessWakeFn)(void* PR_NULLABLE context);

// What to start.
typedef struct PrProcessDesc {
    // The program, as a path or as a name that the directories of the PATH environment variable are searched for, and
    // the arguments that follow its name.
    PrStringView program;
    const PrStringView* PR_NULLABLE arguments;
    uint32_t argumentCount;

    PrStringView directory;     // the process's working directory, or the null view for the launcher's
    PrProcessErrors errors;

    PrProcessWakeFn PR_NULLABLE wake;
    void* PR_NULLABLE wakeContext;
} PrProcessDesc;

// What became of a process.
typedef struct PrProcessInfo {
    bool outputEnded;           // the process's output ended, and every byte of it was read
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
// found or started, Io if the backend cannot make its pipes, and NotSupported if the launcher starts no programs. On
// failure, `*out` is null.
PrStatus prProcessLaunch(PrProcessLauncher* launcher, const PrProcessDesc* desc, PrProcess* PR_NULLABLE* PR_NONNULL out);

// Ends the process if it is still running, as prProcessKill does, waits for it to exit, and frees it. The wake function
// is not called after this returns.
void prProcessDestroy(PrProcess* PR_NULLABLE process);

// Queues a copy of `bytes` for the process's standard input, after what was written before. A write after
// prProcessCloseInput, or once the process stops reading its input, is dropped.
void prProcessWrite(PrProcess* process, PrByteView bytes);

// Closes the process's standard input once the bytes written before it reach the process, so that the process reads the
// end of its input.
void prProcessCloseInput(PrProcess* process);

// Copies up to `capacity` bytes of `stream` that arrived into `output`, and returns how many it copied. Returns zero
// once every byte that arrived has been read. A process holds a bounded amount of unread output, and stops reading its
// pipe while it holds that much, so that a process that writes more waits for its reader.
size_t prProcessRead(PrProcess* process, PrProcessStream stream, uint8_t* PR_NULLABLE output, size_t capacity);

// Returns what became of the process.
PrProcessInfo prProcessInfo(const PrProcess* process);

// Ends the process at once, without letting it clean up. A process that already exited is left as it is. A backend's
// own header says how it ends one.
void prProcessKill(PrProcess* process);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
