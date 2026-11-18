// The Windows backend of the environment, which reads its variables as UTF-16 with GetEnvironmentVariableW, changes
// them with SetEnvironmentVariableW, and reads the host name with GetComputerNameExW. It is the default binding of the
// environment on Windows.

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

// Implements prEnvironmentSetVariable for `name`, which is not empty and has no `=` or NUL, and `value`, which has no
// NUL. The null view of `value` removes the variable (prEnvironmentRemoveVariable).
bool prCoreWindowsEnvironmentSetVariable(PrStringView name, PrStringView value);

// Implements prEnvironmentHostName.
PrRawString prCoreWindowsEnvironmentHostName(void);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
