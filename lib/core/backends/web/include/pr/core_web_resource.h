// The web backend of resources. A page keeps its resources in /resources of its file system, where the page's build
// packages them. A program that runs under Node.js keeps them in <script>.resources beside its script, which it reads
// through the host's file system. It is the default binding of resources on the web.

#pragma once

#include "pr/base.h"
#include "pr/blob.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Returns a blob of the resource `name`, a well-formed name, or null if the program has no such resource.
PrBlob* PR_NULLABLE prCoreWebResourceOpen(PrStringView name);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
