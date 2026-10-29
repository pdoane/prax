// The POSIX backend of random bytes, which reads them with getentropy. It serves the Unix-like targets, and it is the
// default binding of random bytes on them.

#pragma once

#include "pr/base.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Implements prRandomBytes for `size` bytes, which is not zero.
void prCorePosixRandomBytes(void* out, size_t size);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
