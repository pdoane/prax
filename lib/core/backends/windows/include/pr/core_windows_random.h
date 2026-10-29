// The Windows backend of random bytes, which reads them with BCryptGenRandom from the system's preferred generator. It
// is the default binding of random bytes on Windows.

#pragma once

#include "pr/base.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Implements prRandomBytes for `size` bytes, which is not zero.
void prCoreWindowsRandomBytes(void* out, size_t size);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
