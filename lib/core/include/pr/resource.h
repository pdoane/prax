// Resources through the platform: the files that a program is packaged with, which it reads by name.
//
// A program's build declares its resources and stages them where the platform keeps a program's files (pr_resources):
// in an application bundle's resources on Apple's platforms, in the page's file system on the web, beside the
// executable elsewhere, and in an application package's assets on a platform that has them. A resource is reached by
// its name alone, never by a path of the file system, since a platform need not keep resources as files.
//
// A name is a relative path whose parts a slash separates, as in "font_bundle/Tinos-Regular.ttf". No part is empty,
// ".", or "..", and no part holds a backslash, so a name reaches nothing outside the program's resources. Resources are
// only read: they are what the program was packaged with, and the platform may share them between its runs.

#pragma once

#include "pr/base.h"
#include "pr/blob.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Returns true if `name` is a well-formed resource name.
bool prResourceNameIsValid(PrStringView name);

// Returns a blob of the bytes of the resource `name`, which the caller releases (prBlobRelease). The platform either
// maps the resource or reads it. An empty resource gives a blob of no bytes. Returns null if `name` is not well formed
// or the program has no such resource. May be called from any thread.
PrBlob* PR_NULLABLE prResourceOpen(PrStringView name);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
