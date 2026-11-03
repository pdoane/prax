// JavaScript binding of Blob and File.
//
// FileAPI - https://w3c.github.io/FileAPI/

#pragma once

#include "pr/fetch_blob.h"

#include "quickjs.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Defines Blob and File on the global object of `ctx`.
void prJsFetchBlobBind(JSContext* ctx);

// Returns the blob of `value`, or null if it is not a Blob. Throws nothing.
const PrFetchBlob* PR_NULLABLE prJsFetchBlobFromValue(JSContext* ctx, JSValueConst value);

// Returns a new object of `blob`, as a File if it is one. Takes the caller's reference.
JSValue prJsFetchBlobNew(JSContext* ctx, const PrFetchBlob* blob);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
