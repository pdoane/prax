// The process's environment through the platform: the variables that it was started with.

#pragma once

#include "pr/base.h"
#include "pr/raw_string.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Returns the value of the environment variable `name`, as UTF-8, or the empty string if it is not set, is set to
// nothing, or has a value that is not valid UTF-8. A name of the empty string is set to nothing. The caller releases
// the string with prRawStringReset. May be called from any thread, while no thread changes the environment.
PrRawString prEnvironmentVariable(PrStringView name);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
