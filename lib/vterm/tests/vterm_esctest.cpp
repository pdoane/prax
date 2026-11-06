// The harness for the esctest suite: a terminal with no window that runs esctest.
//
// esctest drives the terminal on its standard streams, and reads the screen back through the terminal's reports. This
// program runs the command named by its arguments on a pseudoterminal and relays bytes in both directions. The
// command's output goes to the terminal, and the terminal's replies go back to the command. The harness exits with the
// command's status after the command exits and all of its output has been read.
//
// Usage: vterm_esctest <program> [arguments...]

#include "pr/mutex.h"
#include "pr/process.h"
#include "pr/thread.h"
#include "pr/vterm.h"

#include <stdio.h>

namespace {

// The extent that esctest resets the terminal to before each test.
constexpr PrUVec2 kExtent = {80, 25};

// The most arguments that the command takes.
constexpr uint32_t kMaxArguments = 64;

// How long the relay waits for a wake before it checks the process again, in milliseconds.
constexpr double kPollMillis = 100;

// Signaled by the process's wake function when output arrives or the process exits.
struct Wake {
    PrMutex mutex;
    PrCondition condition;
    bool pending;
};

void wake(void* PR_NULLABLE context) {
    Wake* signal = static_cast<Wake*>(context);
    prMutexLock(&signal->mutex);
    signal->pending = true;
    prConditionSignal(&signal->condition);
    prMutexUnlock(&signal->mutex);
}

// Relays bytes between the process and the terminal until the process exits and its output is read. Returns the
// process's exit status, or 128 plus the signal number if a signal ended it.
int relay(PrProcess* process, PrVterm* vterm, Wake* signal) {
    uint8_t chunk[16384];
    for (;;) {
        for (;;) {
            size_t got = prProcessRead(process, PrProcessStream_Output, chunk, sizeof(chunk));
            if (got == 0)
                break;
            prVtermWrite(vterm, PrByteView{chunk, got});
        }
        PrByteView output = prVtermOutput(vterm);
        if (output.size != 0) {
            prProcessWrite(process, output);
            prVtermOutputConsume(vterm, output.size);
        }
        PrProcessInfo info = prProcessInfo(process);
        if (info.outputEnded && info.exited)
            return info.signaled ? 128 + info.code : info.code;
        prMutexLock(&signal->mutex);
        if (!signal->pending)
            prConditionWaitFor(&signal->condition, &signal->mutex, kPollMillis);
        signal->pending = false;
        prMutexUnlock(&signal->mutex);
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: vterm_esctest <program> [arguments...]\n");
        return 2;
    }
    PrProcessLauncher* launcher = prProcessLauncherCreate(PrProcessBackend_Default);
    if (launcher == nullptr) {
        fprintf(stderr, "vterm_esctest: the build has no process launcher\n");
        return 2;
    }
    if (argc - 2 > int(kMaxArguments)) {
        fprintf(stderr, "vterm_esctest: more than %u arguments\n", kMaxArguments);
        return 2;
    }
    PrStringView arguments[kMaxArguments];
    uint32_t argumentCount = 0;
    for (int i = 2; i < argc; ++i)
        arguments[argumentCount++] = argv[i];
    PrStringView environment[] = {"TERM=xterm"};
    Wake signal{};
    prMutexInit(&signal.mutex);
    prConditionInit(&signal.condition);
    PrProcessDesc desc{
        .program = argv[1],
        .arguments = arguments,
        .argumentCount = argumentCount,
        .environment = environment,
        .environmentCount = 1,
        .streams = PrProcessStreams_Terminal,
        .terminalExtent = kExtent,
        .wake = wake,
        .wakeContext = &signal,
    };
    PrProcess* process = nullptr;
    PrStatus status = prProcessLaunch(launcher, &desc, &process);
    int code = 2;
    if (status == PrStatus_Ok) {
        // esctest reads characters back through checksums. Its --xterm-checksum option of 334 or later sums characters
        // only.
        PrVtermDesc vtermDesc{.extent = kExtent, .checksum = PrVtermChecksumFlag_Characters};
        PrVterm* vterm = prVtermCreate(&vtermDesc);
        code = relay(process, vterm, &signal);
        prVtermDestroy(vterm);
    } else {
        fprintf(stderr, "vterm_esctest: cannot start %s\n", argv[1]);
    }
    prProcessDestroy(process);
    prProcessLauncherDestroy(launcher);
    prConditionReset(&signal.condition);
    prMutexReset(&signal.mutex);
    return code;
}
