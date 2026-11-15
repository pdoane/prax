// The Windows backend of resources, which keeps a program's resources in <executable>.resources beside its executable.
// It is the default binding of resources on Windows.

#pragma once

#include "pr/base.h"
#include "pr/blob.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Returns a blob of the resource `name`, a well-formed name, or null if the program has no such resource.
PrBlob* PR_NULLABLE prCoreWindowsResourceOpen(PrStringView name);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
