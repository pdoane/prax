// The Windows backend of the environment, which reads it as UTF-16 with GetEnvironmentVariableW. It is the default
// binding of the environment on Windows.

#pragma once

#include "pr/base.h"
#include "pr/raw_string.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Implements prEnvironmentVariable for `name`, which is not empty.
PrRawString prCoreWindowsEnvironmentVariable(PrStringView name);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
