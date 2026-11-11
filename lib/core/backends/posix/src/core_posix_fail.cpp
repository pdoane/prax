// The POSIX backend of failure.

#include "pr/core_posix_fail.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// The C library's backtraces, where it has them. WebAssembly's has none, and its runtime reports the stack of an abort.
#if __has_include(<execinfo.h>)
#include <execinfo.h>
#define PR_POSIX_BACKTRACE 1
#else
#define PR_POSIX_BACKTRACE 0
#endif

// An alternate stack for signals, which tvOS lacks. There, a crash reports on the stack that it happened on, which a
// stack that overflowed cannot do.
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#if defined(__APPLE__) && TARGET_OS_TV
#define PR_POSIX_SIGNAL_STACK 0
#else
#define PR_POSIX_SIGNAL_STACK 1
#endif

namespace pr_impl {
namespace {

#if PR_POSIX_SIGNAL_STACK
// The stack that a crash is reported on, so that a thread that overflows its own stack still reports.
alignas(16) char crashStack[64 * 1024];
#endif

void writeText(const char* text) {
    ssize_t written = write(STDERR_FILENO, text, strlen(text));
    (void)written;
}

// Writes `value` in hexadecimal. It allocates nothing, so a signal handler may call it.
void writeHex(uintptr_t value) {
    char text[2 + 2 * sizeof(value) + 1];
    char* at = text + sizeof(text) - 1;
    *at = '\0';
    do {
        *--at = "0123456789abcdef"[value & 0xf];
        value >>= 4;
    } while (value != 0);
    *--at = 'x';
    *--at = '0';
    writeText(at);
}

// Writes the stack of the calling thread to the standard error, one frame a line, without its `skip` innermost frames.
// It allocates nothing, so a signal handler may call it.
void writeStack(int skip) {
#if PR_POSIX_BACKTRACE
    constexpr int kMaxFrames = 128;
    void* frames[kMaxFrames];
    int count = backtrace(frames, kMaxFrames);
    if (count > skip)
        backtrace_symbols_fd(frames + skip, count - skip, STDERR_FILENO);
#else
    (void)skip;
#endif
}

const char* nameOf(int number) {
    switch (number) {
        case SIGSEGV: return "SIGSEGV";
        case SIGBUS: return "SIGBUS";
        case SIGILL: return "SIGILL";
        case SIGFPE: return "SIGFPE";
        case SIGTRAP: return "SIGTRAP";
        case SIGABRT: return "SIGABRT";
        default: return "a signal";
    }
}

// Reports the crash, then lets the signal end the process as it would have. The handler restores the default action
// itself, because another handler that chains to it, as a test framework's does, may have reinstalled it without the
// reset on delivery (SA_RESETHAND). The signal raised here waits until the handler returns, and the default action then
// ends the process.
void crashed(int number, siginfo_t* info, void*) {
    signal(number, SIG_DFL);
    writeText("crashed: ");
    writeText(nameOf(number));
    writeText(" at address ");
    writeHex(reinterpret_cast<uintptr_t>(info->si_addr));
    writeText("\n");
    // The frames of the handler and of the system's trampoline.
    writeStack(2);
    raise(number);
}

} // namespace
} // namespace pr_impl

using namespace pr_impl;

void prCorePosixFailed(PrStringView message, PrStringView file, uint32_t line) {
    fprintf(stderr, "%.*s:%u: %.*s\n", int(file.size), file.data, line, int(message.size), message.data);
    fflush(stderr);
    writeStack(1);
    // The stack is written: the abort ends the process without the crash's report.
    signal(SIGABRT, SIG_DFL);
    abort();
}

void prCorePosixCrashHandler(void) {
    if (PR_ADDRESS_SANITIZER)
        return;
    struct sigaction action {};
    action.sa_sigaction = crashed;
    // Some C libraries' flags are unsigned, as musl's SA_RESETHAND is, and the field is an int.
    action.sa_flags = int(SA_SIGINFO | SA_RESETHAND);
#if PR_POSIX_SIGNAL_STACK
    stack_t stack{};
    stack.ss_sp = crashStack;
    stack.ss_size = sizeof(crashStack);
    sigaltstack(&stack, nullptr);
    action.sa_flags |= SA_ONSTACK;
#endif
    sigemptyset(&action.sa_mask);
    constexpr int kCrashes[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP, SIGABRT};
    for (int number : kCrashes)
        sigaction(number, &action, nullptr);
}
