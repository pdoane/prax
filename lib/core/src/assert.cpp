// Assertions and fatal errors through the platform.

#include "pr/assert.h"

#include "core/src/config.h"

#if !defined(PR_PLATFORM_FAIL)
#error "a build without the default platform binds PR_PLATFORM_FAIL"
#endif

// Kept out of line and cold, so that the call sites that reach it stay small.
PR_NOINLINE PR_COLD void prFail(const PrFailSite* site) {
    PR_PLATFORM_FAIL(PrStringView(site->message), PrStringView(site->file), site->line);
}

void prCrashHandler(void) {
    PR_PLATFORM_CRASH_HANDLER();
}
