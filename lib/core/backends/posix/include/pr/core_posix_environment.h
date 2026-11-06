// The POSIX backend of the environment, which reads its variables with getenv and the host name with gethostname. It is
// the default binding of the environment on the Unix-like targets.

#pragma once

#include "pr/base.h"
#include "pr/raw_string.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Implements prEnvironmentVariable for `name`, which is not empty.
PrRawString prCorePosixEnvironmentVariable(PrStringView name);

// Implements prEnvironmentHostName.
PrRawString prCorePosixEnvironmentHostName(void);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
