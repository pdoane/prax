// The POSIX backend of failure. It prints a failure, and the stack of the thread that failed, to the standard error and
// aborts the process. It serves the Unix-like targets, and it is the default binding of failure on them.

#pragma once

#include "pr/base.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Reports a failure that the library cannot continue past, such as an assertion that does not hold, raised at `line` of
// `file`, and ends the process. This function never returns, and neither does a binding of PR_PLATFORM_FAIL.
PR_NORETURN void prCorePosixFailed(PrStringView message, PrStringView file, uint32_t line);

// Makes the signals of a crash (SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP and SIGABRT, as an uncaught exception raises)
// report the signal, its address and the stack of the thread that crashed before they end the process. A crash of the
// calling thread is reported on a stack of its own, so that a stack overflow reports too.
void prCorePosixCrashHandler(void);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
