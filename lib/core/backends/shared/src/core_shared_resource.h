// What the default backends of resources share: each family keeps a program's resources as files under a directory that
// only the family knows how to find.

#pragma once

#include "pr/base.h"
#include "pr/blob.h"
#include "pr/raw_string.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Returns a blob of the file of the resource `name`, a well-formed name, under `directory`, or null if `directory` is
// empty or the file cannot be opened.
PrBlob* PR_NULLABLE resourceFileOpen(PrStringView directory, PrStringView name);

// The directory of the resources of a program that is not in an application bundle: <executable>.resources beside the
// executable at `executable`, without the executable's extension, so that a.exe's are in a.resources. Returns the empty
// string if `executable` is empty. The caller releases the string with prRawStringReset.
PrRawString resourceDirectoryBeside(PrStringView executable);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
