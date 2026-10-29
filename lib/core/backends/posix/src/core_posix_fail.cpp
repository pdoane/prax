// The POSIX backend of failure.

#include "pr/core_posix_fail.h"

#include <execinfo.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

namespace pr_impl {
namespace {

constexpr int kMaxFrames = 128;

// The stack that a crash is reported on, so that a thread that overflows its own stack still reports.
alignas(16) char crashStack[64 * 1024];

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
    void* frames[kMaxFrames];
    int count = backtrace(frames, kMaxFrames);
    if (count > skip)
        backtrace_symbols_fd(frames + skip, count - skip, STDERR_FILENO);
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

// Reports the crash, then lets the signal end the process as it would have: the handler is reset as it runs.
void crashed(int number, siginfo_t* info, void*) {
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
    stack_t stack{};
    stack.ss_sp = crashStack;
    stack.ss_size = sizeof(crashStack);
    sigaltstack(&stack, nullptr);
    struct sigaction action {};
    action.sa_sigaction = crashed;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
    sigemptyset(&action.sa_mask);
    constexpr int kCrashes[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP, SIGABRT};
    for (int number : kCrashes)
        sigaction(number, &action, nullptr);
}
