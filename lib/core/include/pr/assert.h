// Failures that the library cannot continue past: assertions and fatal errors. Each one is reported through the
// platform, and the platform ends the process.
//
// This header includes the build's configuration (PR_USER_CONFIG), which may replace PR_ASSERT. A configuration that a
// C file reaches through this header is C.

#pragma once

#include "pr/base.h"

#if defined(PR_USER_CONFIG)
#include PR_USER_CONFIG
#endif

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Where a failure was raised, and what it says. Each PR_ASSERT and PR_FATAL holds one, so that a call site passes one
// pointer.
typedef struct PrFailSite {
    const char* message;
    const char* file;
    uint32_t line;
} PrFailSite;

// Reports `site` and ends the process.
PR_NORETURN void prFail(const PrFailSite* site);

// Installs the platform's handler of a crash of the process, such as an access to memory that it may not access, which
// runs before the process ends. The default platforms' handlers print what crashed and the stack of the thread that
// crashed. An application calls this once, as it starts.
void prCrashHandler(void);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END

// Calls prFail with a site for the string literal `message` at the current line: a constant in C++, and a compound
// literal, made only when the failure happens, in C.
#if defined(__cplusplus)
#define PR_FAIL_AT(message)                                                                            \
    prFail([]() -> const PrFailSite* {                                                                 \
        static constexpr PrFailSite site = {message, __FILE__, uint32_t(__LINE__)};                    \
        return &site;                                                                                  \
    }())
#else
#define PR_FAIL_AT(message) prFail(&(const PrFailSite){message, __FILE__, (uint32_t)__LINE__})
#endif

// Reports `message`, a string literal, and ends the process, in every build. Use it for a failure that the library
// cannot handle.
#define PR_FATAL(message) PR_FAIL_AT(message)

// Checks that `cond` holds, and ends the process if it does not. Under NDEBUG, `cond` is not evaluated but still counts
// as a use, so a variable or function used only by assertions is not unused.
#if !defined(PR_ASSERT)
#if defined(NDEBUG)
#define PR_ASSERT(cond) ((void)(false && (cond)))
#else
#define PR_ASSERT(cond) ((cond) ? (void)0 : PR_FAIL_AT("assertion failed: " #cond))
#endif
#endif
