// The Apple backend of resources, which keeps an application bundle's resources in the bundle's resources directory:
// the bundle itself on iOS, and Contents/Resources on macOS. A program outside a bundle keeps them in
// <executable>.resources beside its executable. It is the default binding of resources on Apple's targets.

#pragma once

#include "pr/base.h"
#include "pr/blob.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Returns a blob of the resource `name`, a well-formed name, or null if the program has no such resource.
PrBlob* PR_NULLABLE prCoreAppleResourceOpen(PrStringView name);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
