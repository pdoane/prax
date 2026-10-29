// The Windows backend of failure. It prints a failure, and the stack of the thread that failed, to the standard error
// and ends the process. It serves Windows, and it is the default binding of failure there.

#pragma once

#include "pr/base.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Reports a failure that the library cannot continue past, such as an assertion that does not hold, raised at `line` of
// `file`, and ends the process. A debugger that is attached breaks first. This function never returns, and neither does
// a binding of PR_PLATFORM_FAIL.
PR_NORETURN void prCoreWindowsFailed(PrStringView message, PrStringView file, uint32_t line);

// Makes an exception that no handler takes, such as an access violation, report its code, its address and the stack of
// the thread that crashed before it ends the process.
void prCoreWindowsCrashHandler(void);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
