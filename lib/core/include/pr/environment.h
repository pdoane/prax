// The process's environment through the platform: its variables, and the machine that it runs on.

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

// Sets the environment variable `name` to `value`, both UTF-8, for this process and the processes that it starts after.
// Returns false, and changes nothing, if the name is empty or contains `=` or NUL, if the value contains NUL, or if the
// platform refuses. Must not be called while another thread reads or changes the environment.
bool prEnvironmentSetVariable(PrStringView name, PrStringView value);

// Removes the environment variable `name`, as prEnvironmentSetVariable changes it. Returns true if the variable is not
// set afterward, whether or not it was before, and false for a name that prEnvironmentSetVariable refuses.
bool prEnvironmentRemoveVariable(PrStringView name);

// Returns the name of the machine that the process runs on, as the system reports it. Returns the empty string if the
// system reports no name, or a name that is not valid UTF-8. The caller releases the string with prRawStringReset. May
// be called from any thread.
PrRawString prEnvironmentHostName(void);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
