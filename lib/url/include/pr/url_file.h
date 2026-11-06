// File URLs: conversion between the paths of this machine and the file: URLs that name them.
//
// url - https://url.spec.whatwg.org/

#pragma once

#include "pr/base.h"
#include "pr/raw_string.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Returns the file URL of the absolute UTF-8 `path`, with an empty host. The bytes that a URL's path escapes
// (PrUrlPercentEncodeSet_Path) are percent-encoded, and so is `%`. On Windows, a backslash becomes a slash, and a path
// that begins with a drive gets a slash before it. On other platforms, a backslash is percent-encoded. The caller
// releases the URL with prRawStringReset.
PrRawString prUrlFromFilePath(PrStringView path);

// Writes the path of this machine that the file URL `url` names, percent-decoded, to `*out`. The URL's host must name
// this machine. It may be empty, which is also what the parser makes of localhost, or it may be the machine's host name
// or the first label of that name. On Windows, the slash before a drive is dropped. Returns false, and leaves `*out`
// unchanged, if the URL does not parse, has another scheme or another machine's host, or if its path is empty, is not
// UTF-8 or holds a NUL. The caller releases the path with prRawStringReset.
bool prUrlToFilePath(PrStringView url, PrRawString* out);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
