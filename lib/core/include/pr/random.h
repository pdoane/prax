// Random bytes through the platform, from the operating system's cryptographically secure generator.

#pragma once

#include "pr/base.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Fills the `size` bytes at `out` with random bytes that are unpredictable enough for keys and nonces. A platform that
// cannot supply them fails. May be called from any thread.
void prRandomBytes(void* PR_NULLABLE out, size_t size);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
